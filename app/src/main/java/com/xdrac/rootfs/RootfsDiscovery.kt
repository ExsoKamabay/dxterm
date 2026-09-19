package com.xdrac.rootfs

import android.content.Context
import android.os.Build
import android.util.Log
import java.io.File

/**
 * Finds an image the user already has on the device, and picks between them.
 *
 * Whether the APK itself carries an image is NOT decided here -- that is
 * [RootfsSourceResolver]'s single job. This class owns only [imageDir], where
 * [RootfsDownloader] puts what the user asked for, so a failed or interrupted install can
 * be retried without downloading again.
 *
 * Never throws: a missing or empty location contributes nothing.
 */
class RootfsDiscovery(private val ctx: Context) {

    companion object {
        const val ASSET_DIR = "rootfs"
        /** On-device location for images obtained at the user's request. */
        const val IMAGE_DIR = "rootfs-image"

        /**
         * The device's primary CPU arch in [RootfsArchive.classify]'s vocabulary
         * ("arm64"/"armhf"/"x86_64"/"x86"), so a rootfs' `arch` can be compared to it directly.
         * Shared by [selectLocal] and [RootfsSourceResolver] so bundled and downloaded images are
         * matched to the device the same way.
         */
        fun deviceArch(): String {
            val abis = Build.SUPPORTED_ABIS ?: emptyArray()
            return when {
                abis.any { it == "arm64-v8a" } -> "arm64"
                abis.any { it == "armeabi-v7a" } -> "armhf"
                abis.any { it == "x86_64" } -> "x86_64"
                abis.any { it == "x86" } -> "x86"
                else -> "unknown"
            }
        }
    }

    /** Directory holding downloaded/imported images. Created lazily by the downloader. */
    fun imageDir(): File = File(ctx.filesDir, IMAGE_DIR)

    fun listLocalFiles(): List<RootfsArchive> {
        val dir = imageDir()
        if (!dir.isDirectory) return emptyList()
        val files = dir.listFiles()?.filter { it.isFile && it.length() > 0L } ?: emptyList()
        return files.mapNotNull {
            RootfsArchive.classify(it.name, RootfsArchive.Source.LocalFile(it))
        }
    }

    /**
     * An already-downloaded archive matching the device ABI, else the first available, else null.
     *
     * Only the device directory is scanned. A bundled image never reaches this path; the
     * caller asks [RootfsSourceResolver] for that and provisions it directly.
     */
    fun selectLocal(): RootfsArchive? {
        Log.i(ShellLocator.TAG, "[DISCOVERY] Scanning ${imageDir().absolutePath}")
        val all = listLocalFiles()
        Log.i(ShellLocator.TAG, "[DISCOVERY] Archives found: " +
            if (all.isEmpty()) "(none)" else all.joinToString(", ") { "${it.fileName} @ ${it.describe()}" })
        if (all.isEmpty()) return null
        val abi = deviceArch()
        Log.i(ShellLocator.TAG, "[DISCOVERY] Device ABI: ${Build.SUPPORTED_ABIS?.joinToString(",")} -> arch=$abi")
        val chosen = all.firstOrNull { it.arch == abi } ?: all.first()
        Log.i(ShellLocator.TAG, "[DISCOVERY] Archive selected: ${chosen.fileName} " +
            "(arch=${chosen.arch}, ${chosen.compression}, from ${chosen.describe()})")
        return chosen
    }

    /** Deletes on-device images. Called after a successful extraction to reclaim space. */
    fun discardLocalImages(): Long {
        var freed = 0L
        (imageDir().listFiles() ?: emptyArray()).forEach { f ->
            val n = f.length()
            if (f.delete()) freed += n
        }
        if (freed > 0L) Log.i(ShellLocator.TAG, "[DISCOVERY] Reclaimed $freed bytes of image cache")
        return freed
    }
}
