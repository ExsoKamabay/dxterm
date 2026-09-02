package com.xdrac.net

import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * Redirect handling for the two things this app downloads.
 *
 * `HttpURLConnection` will not follow a redirect that changes protocol, and it must not:
 * both downloads end up as executable code inside the user's sandbox, so a hop answered
 * with `Location: http://...` is refused rather than followed. The SHA-256 pin still
 * rejects a tampered file, but a cleartext hop leaks what is being fetched, hands an
 * on-path attacker a free denial of service, and contradicts what the app tells users
 * about its network use.
 *
 * The Ollama installer and the rootfs downloader each grew their own copy of this loop,
 * and the copies diverged: one resolved a relative Location and rejected plaintext, the
 * other did neither. One implementation, one set of tests.
 */
object HttpsOnly {

    const val MAX_HOPS = 5

    /** A redirect this loop follows. 307 and 308 are not in [HttpURLConnection]'s constants. */
    fun isRedirect(code: Int): Boolean = code == HttpURLConnection.HTTP_MOVED_PERM ||
        code == HttpURLConnection.HTTP_MOVED_TEMP ||
        code == HttpURLConnection.HTTP_SEE_OTHER ||
        code == 307 || code == 308

    /** [url] unchanged, or throws when it is not HTTPS. */
    fun requireHttps(url: String): URL {
        val parsed = URL(url)
        if (!parsed.protocol.equals("https", ignoreCase = true)) {
            throw IOException("refusing a non-HTTPS URL: $url")
        }
        return parsed
    }

    /**
     * The absolute URL a `Location` header names.
     *
     * Location is allowed to be a relative reference (RFC 9110 §10.2.2) and `URL(String)`
     * throws on one, which turned a spec-legal redirect into a failed install.
     */
    fun resolve(current: URL, location: String?): String {
        if (location.isNullOrBlank()) throw IOException("redirect without Location")
        return URL(current, location).toString()
    }

    /**
     * Opens [startUrl], following redirects itself, and returns the connection carrying the
     * final response. [configure] is applied to every hop, so per-request headers such as
     * `Range` survive a redirect. The caller owns the connection and must disconnect it.
     */
    /**
     * [onConnection] is handed every hop's connection as soon as it exists, before the request is
     * sent. It exists so a caller can close a connection that is still setting up: connect, TLS
     * and each redirect all block inside this function, and a caller polling a cancel flag cannot
     * see any of them. Optional; callers that do not cancel pass nothing.
     */
    fun open(
        startUrl: String,
        onConnection: ((HttpURLConnection) -> Unit)? = null,
        configure: (HttpURLConnection) -> Unit
    ): HttpURLConnection {
        var url = startUrl
        for (hop in 0..MAX_HOPS) {
            val parsed = requireHttps(url)
            val conn = (parsed.openConnection() as HttpURLConnection).apply {
                instanceFollowRedirects = false
                configure(this)
            }
            onConnection?.invoke(conn)
            // responseCode is where the request is actually sent, so it is also where a
            // connect/read timeout or a TLS failure surfaces. Without this the socket is left
            // to the finaliser, and a retry loop opens one per attempt.
            val code = try {
                conn.responseCode
            } catch (e: Throwable) {
                conn.disconnect()
                throw e
            }
            if (!isRedirect(code)) return conn
            val location = conn.getHeaderField("Location")
            conn.disconnect()
            url = resolve(parsed, location)
        }
        throw IOException("too many redirects (more than $MAX_HOPS) starting at $startUrl")
    }
}
