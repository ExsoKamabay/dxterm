package com.xdrac.rootfs

import android.content.Context
import android.os.Build
import android.util.Log
import org.json.JSONObject

/**
 * The Linux images drac-Xterm offers to fetch, read from `assets/rootfsURLS.json`.
 *
 * The catalogue lives in JSON rather than in Kotlin so that adding a distribution is a
 * data change, not a code change. Two rules govern it, and both are enforced here rather
 * than left to whoever edits the file:
 *
 *  1. **Only images the device can actually run are offered.** The JSON is keyed by
 *     Android ABI, and [entriesFor] returns the entries matching the device's own ABI
 *     list. PRoot cannot execute an aarch64 rootfs on an armeabi-v7a device; showing it
 *     would be offering a download that is guaranteed to fail after several hundred
 *     megabytes.
 *
 *  2. **The hash is the gate, not a formality.** [RootfsDownloader] refuses to hand an
 *     archive to the extractor unless the digest of the completed file matches
 *     [Entry.sha256] exactly. An entry without a valid 64-character digest is dropped
 *     when the catalogue is parsed, a silently unverified 200 MB download is worse than
 *     a missing option.
 *
 * Digests are pinned to specific point releases. Upstream publishes a checksum file next
 * to each image, but reading that from the same mirror verifies nothing: whoever can
 * replace the image can replace the checksum beside it. `scripts/verify-rootfs-urls.sh`
 * compares the pins against upstream and reports what moved.
 */
object RootfsCatalog {

    private const val TAG = "dracXterm"
    private const val ASSET = "rootfsURLS.json"

    data class Entry(
        val id: String,
        /** Shown to the user in the picker, e.g. "Alpine Linux 3.21". */
        val label: String,
        val fileName: String,
        val url: String,
        val sha256: String,
        /** Download size in bytes, as published upstream. Progress and the pre-flight
         *  free-space check both use it. */
        val approxBytes: Long,
        /** Rough size of the extracted tree, for the capacity check before unpacking. */
        val approxExtractedBytes: Long,
        val arch: String,
        /** One line of plain language about what this image is, for the picker. */
        val note: String = ""
    )

    @Volatile
    private var cache: List<Entry>? = null

    /** Drop the parsed catalogue. Call after a language change: the notes are read
     *  per-locale at parse time, so a cached list keeps the old language. */
    fun invalidate() {
        cache = null
    }

    /**
     * Every entry the device can run, in the order the JSON lists them.
     *
     * ABI order matters and is the device's, not ours: `Build.SUPPORTED_ABIS` is ranked
     * best-first, so a 64-bit device that can also run 32-bit code gets its 64-bit
     * images offered first.
     */
    fun entriesFor(ctx: Context): List<Entry> {
        cache?.let { return it }
        synchronized(this) {
            cache?.let { return it }
            val parsed = parse(ctx)
            cache = parsed
            return parsed
        }
    }

    /**
     * The image the picker starts on, and the one the offline installer bundles.
     *
     * Read from the catalogue's `_default` key rather than taken as "whatever is
     * first". Position is not a decision: reordering the file would otherwise change
     * which image ships, silently. Falls back to the first runnable entry when
     * `_default` is missing or names something this device cannot run.
     */
    fun default(ctx: Context): Entry? {
        val entries = entriesFor(ctx)
        val wanted = defaultLabel(ctx)
        return entries.firstOrNull { it.label == wanted } ?: entries.firstOrNull()
    }

    /** The label named by `_default`, or empty when the catalogue does not say. */
    private fun defaultLabel(ctx: Context): String = runCatching {
        JSONObject(ctx.assets.open(ASSET).bufferedReader().use { it.readText() })
            .optString("_default")
    }.getOrDefault("")

    fun byId(ctx: Context, id: String): Entry? = entriesFor(ctx).firstOrNull { it.id == id }

    /** Host shown in the consent screen, so the user sees where the bytes come from. */
    fun host(entry: Entry): String =
        runCatching { java.net.URI(entry.url).host ?: entry.url }.getOrDefault(entry.url)

    // ---------------------------------------------------------------------------------

    private fun parse(ctx: Context): List<Entry> {
        val text = runCatching {
            ctx.assets.open(ASSET).bufferedReader().use { it.readText() }
        }.getOrElse {
            Log.e(TAG, "[CATALOG] cannot read assets/$ASSET: ${it.message}")
            return emptyList()
        }

        val root = runCatching { JSONObject(text) }.getOrElse {
            Log.e(TAG, "[CATALOG] assets/$ASSET is not valid JSON: ${it.message}")
            return emptyList()
        }

        val out = mutableListOf<Entry>()
        for (abi in Build.SUPPORTED_ABIS) {
            val images = root.optJSONObject(abi) ?: continue
            for (label in images.keys()) {
                val o = images.optJSONObject(label) ?: continue
                val entry = toEntry(abi, label, o)
                if (entry == null) {
                    Log.w(TAG, "[CATALOG] skipping '$label' for $abi: incomplete or invalid")
                    continue
                }
                out += entry
            }
        }
        Log.i(TAG, "[CATALOG] ${out.size} image(s) available for ${Build.SUPPORTED_ABIS.joinToString()}")
        return out
    }

    /**
     * `note` is either a plain string, or an object keyed by language:
     *
     *     "note": { "id": "...", "en": "..." }
     *
     * The app displays Indonesian by default and English on request, and the note is
     * the one piece of the picker that comes from JSON rather than from strings.xml --
     * so without this it stayed Indonesian while everything around it turned English.
     * Falls back to Indonesian, then to whatever the object holds, so a partially
     * translated entry degrades to text rather than to nothing.
     */
    internal fun readNote(raw: Any?): String = when (raw) {
        is String -> raw
        is JSONObject -> {
            val lang = java.util.Locale.getDefault().language.lowercase()
            raw.optString(lang).ifBlank {
                raw.optString("id").ifBlank {
                    raw.keys().asSequence().firstOrNull()?.let { raw.optString(it) } ?: ""
                }
            }
        }
        else -> ""
    }

    internal fun toEntry(abi: String, label: String, o: JSONObject): Entry? {
        val url = o.optString("url").takeIf { it.startsWith("https://") } ?: return null
        // HTTPS only, and not merely by convention: RootfsDownloader rejects a non-HTTPS
        // hop mid-redirect, so an http:// entry here would fail late instead of never
        // being offered.
        val sha = o.optString("sha256").lowercase()
        if (sha.length != 64 || !sha.all { it in "0123456789abcdef" }) return null

        val fileName = url.substringAfterLast('/').takeIf { it.isNotBlank() } ?: return null
        val download = o.optLong("downloadBytes", 0L)
        if (download <= 0L) return null

        return Entry(
            // Derived from the label so the JSON does not have to carry an id as well as
            // a name that must stay in sync with it.
            id = label.lowercase().replace(Regex("[^a-z0-9]+"), "-").trim('-') + "-" + abi,
            label = label,
            fileName = fileName,
            url = url,
            sha256 = sha,
            approxBytes = download,
            // Fall back to a conservative multiple rather than zero: a zero here would
            // disable the free-space check instead of making it strict.
            approxExtractedBytes = o.optLong("installBytes", 0L).takeIf { it > 0L }
                ?: (download * 5),
            arch = abi,
            note = readNote(o.opt("note"))
        )
    }
}
