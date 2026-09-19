package com.dracxterm.rootfs

import android.content.Context
import android.os.StatFs
import android.util.Log
import com.dracxterm.R
import com.dracxterm.net.HttpsOnly
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.net.HttpURLConnection
import java.security.MessageDigest

/**
 * Fetches a Linux image the user has explicitly asked for.
 *
 * Contract, in order of importance:
 *
 *  1. **Never runs on its own.** The caller must have obtained explicit consent first
 *     (see ProvisioningActivity). Nothing here is triggered by app startup.
 *  2. **HTTPS only.** A plaintext URL, or a redirect that downgrades to plaintext, is a
 *     hard failure rather than something to shrug at: this file becomes executable code
 *     inside the user's sandbox.
 *  3. **A partial file is never promoted.** Bytes land in `<name>.part`. Only after the
 *     SHA-256 of the completed file matches the pinned digest is it renamed to the name
 *     [RootfsDiscovery] scans for. An interrupted transfer therefore cannot be mistaken
 *     for a usable image on the next launch.
 *  4. **Resumable.** A leftover `.part` is continued with a Range request when the server
 *     honours it, and restarted from zero when it does not.
 */
class RootfsDownloader(private val ctx: Context) {

    private val tag = ShellLocator.TAG

    /**
     * The connection the transfer is currently reading from, so a cancel can interrupt it.
     *
     * Cancellation used to be polled only, once per loop turn, immediately BEFORE a blocking
     * read of up to BUFFER bytes. Pressing Cancel therefore did nothing until that read returned:
     * one buffer's worth of data on a good link, and up to READ_TIMEOUT_MS on a stalled one.
     * Closing the connection from [abort] makes the blocked read throw at once.
     */
    private val liveConn = java.util.concurrent.atomic.AtomicReference<HttpURLConnection?>(null)

    /**
     * Interrupt a transfer that is blocked in a read. Safe to call at any time and from any
     * thread, including when nothing is running. The caller still sets its own cancel flag: this
     * only stops the waiting, it does not decide the outcome.
     */
    fun abort() {
        liveConn.getAndSet(null)?.let { runCatching { it.disconnect() } }
    }

    fun interface Progress {
        /** [total] is -1 when the server does not report a length. */
        fun onProgress(downloaded: Long, total: Long)
    }

    sealed class Result {
        data class Ok(val file: File) : Result()
        object Cancelled : Result()
        data class Failed(val reason: String) : Result()
    }

    private companion object {
        const val CONNECT_TIMEOUT_MS = 20_000
        const val READ_TIMEOUT_MS = 30_000
        const val BUFFER = 128 * 1024
        const val USER_AGENT = "drac-Xterm"
    }

    /**
     * Downloads [entry] into [RootfsDiscovery.IMAGE_DIR].
     *
     * Returns [Result.Ok] only when the completed file's digest matches
     * [RootfsCatalog.Entry.sha256]. Blocking; call from a background thread.
     */
    fun download(
        entry: RootfsCatalog.Entry,
        progress: Progress?,
        isCancelled: () -> Boolean,
        /** Something the user should be told while the transfer runs, in their language. Called
         *  on the worker thread; the caller marshals it. Optional so existing callers are
         *  unaffected. */
        notice: ((String) -> Unit)? = null
    ): Result {
        val dir = File(ctx.filesDir, RootfsDiscovery.IMAGE_DIR)
        if (!dir.isDirectory && !dir.mkdirs()) return Result.Failed("cannot create ${dir.absolutePath}")

        val target = File(dir, entry.fileName)
        val part = File(dir, entry.fileName + ".part")

        // An image from a previous run that already matches: nothing to do.
        if (target.isFile && target.length() > 0L) {
            Log.i(tag, "[DOWNLOAD] ${target.name} already present, verifying")
            return when (val v = verify(target, entry.sha256, isCancelled)) {
                is Verify.Match -> Result.Ok(target)
                Verify.Cancelled -> Result.Cancelled
                // Present but unreadable. Nothing can be claimed about it, so it is not handed
                // to the extractor and not silently re-downloaded over: the user is told.
                is Verify.Unreadable -> Result.Failed(
                    "the image already on this device could not be read to check it " +
                        "(${v.reason}); delete ${target.name} and try again"
                )
                is Verify.Mismatch -> {
                    Log.w(tag, "[DOWNLOAD] existing file failed verification (${v.actual}); discarding")
                    // The delete has to be checked. Re-entering with the bad file still on disk
                    // re-verifies the same bytes, fails again, and recurses until the stack runs
                    // out; an undeletable file has to be reported, not looped on.
                    if (!target.delete() && target.exists()) {
                        return Result.Failed(
                            "${target.name} does not match its expected checksum and cannot be deleted"
                        )
                    }
                    download(entry, progress, isCancelled, notice)
                }
            }
        }

        // Capacity: the archive plus what it expands to, with a little headroom.
        val need = entry.approxBytes + entry.approxExtractedBytes
        // -1 when StatFs fails: unknown, so let it through. Zero means full, so refuse.
        val free = runCatching { StatFs(ctx.filesDir.absolutePath).availableBytes }
            .getOrDefault(-1L)
        if (free in 0 until need) {
            return Result.Failed(
                "not enough free space: about ${human(need)} is needed, ${human(free)} available"
            )
        }

        val existing = if (part.isFile) part.length() else 0L
        if (existing > 0L) Log.i(tag, "[DOWNLOAD] resuming ${part.name} at $existing bytes")

        // Cancellation is checked at every boundary that can block, not only inside the read
        // loop. Connecting, following redirects and negotiating TLS can take seconds, and a
        // cancel pressed in that window used to be dropped entirely: the button sat disabled
        // while the transfer it was meant to stop ran to completion.
        if (isCancelled()) {
            Log.i(tag, "[DOWNLOAD] cancelled by user before connecting")
            return Result.Cancelled
        }

        var conn: HttpURLConnection? = null
        try {
            // Every hop is published as it is created, so an abort during connect, TLS or a
            // redirect closes the socket that is actually blocking instead of waiting out
            // CONNECT_TIMEOUT_MS. Measured before this: 18.9 s from pressing Cancel to the
            // paused state, all of it inside connection setup.
            conn = HttpsOnly.open(entry.url, onConnection = { c -> liveConn.set(c) }) { c ->
                c.requestMethod = "GET"
                c.connectTimeout = CONNECT_TIMEOUT_MS
                c.readTimeout = READ_TIMEOUT_MS
                c.setRequestProperty("User-Agent", USER_AGENT)
                c.setRequestProperty("Accept-Encoding", "identity")   // keep contentLength honest
                // Re-set on every hop: a redirect drops the request headers with the request.
                if (existing > 0L) c.setRequestProperty("Range", "bytes=$existing-")
            }
            val code = conn.responseCode
            if (isCancelled()) {
                Log.i(tag, "[DOWNLOAD] cancelled by user while connecting")
                return Result.Cancelled
            }

            val appending: Boolean
            when {
                existing > 0L && code == HttpURLConnection.HTTP_PARTIAL -> appending = true
                code == HttpURLConnection.HTTP_OK -> {
                    // Server ignored the Range header: start over rather than concatenating
                    // a fresh body onto a partial file and producing a corrupt archive.
                    appending = false
                    if (existing > 0L) {
                        Log.i(tag, "[DOWNLOAD] server ignored Range; restarting from 0")
                        // Said before the restarted body is written, so the user learns why the
                        // progress bar went back to zero instead of watching it happen.
                        notice?.invoke(ctx.getString(R.string.consent_resume_unsupported))
                    }
                }
                else -> return Result.Failed("server returned HTTP $code")
            }

            val reported = conn.contentLengthLong
            val total = when {
                reported < 0L -> -1L
                appending -> existing + reported
                else -> reported
            }

            var written = if (appending) existing else 0L
            if (isCancelled()) {
                Log.i(tag, "[DOWNLOAD] cancelled by user before the first byte")
                return Result.Cancelled
            }
            FileOutputStream(part, appending).use { out ->
                conn.inputStream.use { ins ->
                    val buf = ByteArray(BUFFER)
                    var lastReport = 0L
                    while (true) {
                        if (isCancelled()) {
                            Log.i(tag, "[DOWNLOAD] cancelled by user at $written bytes (partial file kept)")
                            return Result.Cancelled
                        }
                        val n = ins.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        written += n
                        if (written - lastReport >= 512 * 1024) {
                            lastReport = written
                            progress?.onProgress(written, total)
                        }
                    }
                    out.fd.sync()
                }
            }
            progress?.onProgress(written, total)

            if (total > 0L && written != total) {
                return Result.Failed("transfer ended early: $written of $total bytes")
            }
        } catch (t: Throwable) {
            // The .part file is deliberately kept so the next attempt can resume.
            //
            // An exception raised because [abort] closed the connection under a blocked read is
            // the user's cancel arriving, not a transfer failure; reporting it as "download
            // failed" would put an error on screen for something they asked for.
            if (isCancelled()) {
                Log.i(tag, "[DOWNLOAD] cancelled by user (read interrupted)")
                return Result.Cancelled
            }
            return Result.Failed("download failed: ${t.javaClass.simpleName}: ${t.message}")
        } finally {
            liveConn.set(null)
            runCatching { conn?.disconnect() }
        }

        Log.i(tag, "[DOWNLOAD] verifying SHA-256 of ${part.name}")
        return when (val v = verify(part, entry.sha256, isCancelled)) {
            Verify.Cancelled -> Result.Cancelled
            // The transfer finished but the bytes cannot be read back. The partial file is kept
            // so a retry can resume rather than start over; an unverified file is never promoted.
            is Verify.Unreadable -> Result.Failed(
                "the download finished but could not be read to check it (${v.reason})"
            )
            is Verify.Mismatch -> {
                // A wrong digest means the bytes are not what we pinned. Keeping them
                // around would only invite a later attempt to extract them.
                part.delete()
                Result.Failed(
                    "the downloaded file does not match its expected checksum and was discarded " +
                        "(expected ${entry.sha256.take(16)}…, got ${v.actual.take(16)}…)"
                )
            }
            is Verify.Match -> {
                if (target.exists()) target.delete()
                if (!part.renameTo(target)) return Result.Failed("cannot finalise ${target.name}")
                Log.i(tag, "[DOWNLOAD] ${target.name} ready (${human(target.length())})")
                Result.Ok(target)
            }
        }
    }

    /** Removes any partial transfer for [entry]. */
    fun discardPartial(entry: RootfsCatalog.Entry) {
        val part = File(File(ctx.filesDir, RootfsDiscovery.IMAGE_DIR), entry.fileName + ".part")
        if (part.exists() && part.delete()) Log.i(tag, "[DOWNLOAD] discarded ${part.name}")
    }

    /** Bytes already fetched for [entry], for showing "resume" instead of "download". */
    fun partialBytes(entry: RootfsCatalog.Entry): Long =
        File(File(ctx.filesDir, RootfsDiscovery.IMAGE_DIR), entry.fileName + ".part")
            .takeIf { it.isFile }?.length() ?: 0L

    // ---------------------------------------------------------------- internals

    private sealed class Verify {
        object Cancelled : Verify()
        /** The bytes could not be read at all, so nothing can be said about the digest. */
        data class Unreadable(val reason: String) : Verify()
        data class Match(val digest: String) : Verify()
        data class Mismatch(val actual: String) : Verify()
    }

    /**
     * Hash [file] and compare, never throwing.
     *
     * Both call sites sit outside the transfer's own try/catch, so an I/O error here -- an
     * unreadable file, a failing card, storage pulled mid-read -- used to escape [download]
     * entirely and land as an uncaught exception on the caller's background thread, which on
     * Android takes the process down. It is reported as [Verify.Unreadable] instead, which the
     * caller turns into a normal Result.Failed the screen already knows how to show.
     */
    private fun verify(file: File, expected: String, isCancelled: () -> Boolean): Verify =
        runCatching { verifyOrThrow(file, expected, isCancelled) }
            .getOrElse { t ->
                Log.w(tag, "[DOWNLOAD] cannot read ${file.name} to verify it: ${t.message}")
                Verify.Unreadable(t.javaClass.simpleName + ": " + (t.message ?: "I/O error"))
            }

    private fun verifyOrThrow(file: File, expected: String, isCancelled: () -> Boolean): Verify {
        val digest = MessageDigest.getInstance("SHA-256")
        val buf = ByteArray(BUFFER)
        file.inputStream().use { ins: InputStream ->
            while (true) {
                if (isCancelled()) return Verify.Cancelled
                val n = ins.read(buf)
                if (n < 0) break
                digest.update(buf, 0, n)
            }
        }
        val actual = digest.digest().joinToString("") { "%02x".format(it) }
        return if (actual.equals(expected, ignoreCase = true)) Verify.Match(actual)
        else Verify.Mismatch(actual)
    }

    private fun human(bytes: Long): String = when {
        bytes >= 1L shl 30 -> "%.1f GB".format(bytes.toDouble() / (1L shl 30))
        bytes >= 1L shl 20 -> "%.0f MB".format(bytes.toDouble() / (1L shl 20))
        else -> "$bytes B"
    }
}
