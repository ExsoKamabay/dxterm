package com.dracxterm.update

import com.dracxterm.net.HttpsOnly
import org.json.JSONObject
import java.io.IOException

/**
 * Reads the project's latest GitHub release so the app can tell the user whether a newer
 * build exists and what changed in it.
 *
 * One unauthenticated GET to the public releases API, over HTTPS only (through [HttpsOnly]).
 * No token, so a manual tap plus the occasional background check stay well inside GitHub's
 * anonymous rate limit. Every failure, from no network to a rate-limit reply, comes back as a
 * [Result] with [Result.error] set, never as a thrown exception, so a check can never take the
 * terminal down.
 */
object UpdateChecker {
    const val REPO = "ExsoKamabay/dxterm"
    private const val API = "https://api.github.com/repos/%s/releases/latest"

    // The versionCode is baked into the release APK's name, e.g. dracxterm-1.1.0-vc7-release.apk.
    // Comparing codes is exact where comparing dotted names is only a heuristic.
    private val VC = Regex("""-vc(\d+)""")

    data class Result(
        val ok: Boolean,
        val tag: String = "",          // release tag, e.g. "v1.2.0"
        val name: String = "",         // release title
        val versionCode: Int = -1,     // parsed from the .apk asset name, or -1 when absent
        val notes: String = "",        // release body, as published
        val pageUrl: String = "",      // the release page on github.com
        val apkUrl: String = "",       // direct .apk download, or [pageUrl] when there is no apk asset
        val error: String? = null,
    )

    fun fetchLatest(repo: String = REPO, userAgent: String = "dracxterm"): Result {
        return try {
            val conn = HttpsOnly.open(API.format(repo)) { c ->
                c.requestMethod = "GET"
                c.connectTimeout = 10_000
                c.readTimeout = 15_000
                c.setRequestProperty("Accept", "application/vnd.github+json")
                c.setRequestProperty("X-GitHub-Api-Version", "2022-11-28")
                // GitHub rejects a request with no User-Agent, so this header is required, not polish.
                c.setRequestProperty("User-Agent", userAgent)
            }
            try {
                when (val code = conn.responseCode) {
                    200 -> parse(conn.inputStream.bufferedReader().use { it.readText() })
                    403 -> Result(false, error = "GitHub rate limit reached; try again later")
                    404 -> Result(false, error = "no releases published for $repo yet")
                    else -> Result(false, error = "GitHub returned HTTP $code")
                }
            } finally {
                conn.disconnect()
            }
        } catch (e: IOException) {
            Result(false, error = e.message ?: "network error")
        } catch (e: Throwable) {
            Result(false, error = e.message ?: e.javaClass.simpleName)
        }
    }

    private fun parse(json: String): Result {
        val o = JSONObject(json)
        val tag = o.optString("tag_name", "")
        if (tag.isBlank()) return Result(false, error = "the latest release has no tag")
        val name = o.optString("name", "").ifBlank { tag }
        val notes = o.optString("body", "").replace("\r\n", "\n").trim()
        val page = o.optString("html_url", "")
        var apk = ""
        var vc = -1
        o.optJSONArray("assets")?.let { arr ->
            for (i in 0 until arr.length()) {
                val a = arr.getJSONObject(i)
                val assetName = a.optString("name", "")
                if (assetName.endsWith(".apk")) {
                    apk = a.optString("browser_download_url", "")
                    VC.find(assetName)?.groupValues?.getOrNull(1)?.toIntOrNull()?.let { vc = it }
                    break
                }
            }
        }
        return Result(
            ok = true, tag = tag, name = name, versionCode = vc, notes = notes,
            pageUrl = page, apkUrl = apk.ifBlank { page },
        )
    }

    /**
     * Compares two dotted version names numerically after stripping a leading "v". Returns a
     * positive number when [a] is newer than [b], used as the fallback when a release carries no
     * APK asset to read a versionCode from.
     */
    fun compareVersionNames(a: String, b: String): Int {
        fun parts(s: String) = s.trim().trimStart('v', 'V')
            .split('.', '-', '_', '+')
            .map { seg -> seg.takeWhile { it.isDigit() }.toIntOrNull() ?: 0 }
        val pa = parts(a)
        val pb = parts(b)
        for (i in 0 until maxOf(pa.size, pb.size)) {
            val cmp = pa.getOrElse(i) { 0 }.compareTo(pb.getOrElse(i) { 0 })
            if (cmp != 0) return cmp
        }
        return 0
    }
}
