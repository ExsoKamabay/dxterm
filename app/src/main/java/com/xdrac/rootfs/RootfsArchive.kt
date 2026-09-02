package com.xdrac.rootfs

import android.content.Context
import java.io.File
import java.io.FileInputStream
import java.io.InputStream

/**
 * A rootfs archive drac-Xterm can provision from. Immutable descriptor only.
 *
 * An archive reaches the device by one of two routes, and nothing downstream cares which:
 *
 *  - [Source.Asset]     bundled into the APK at build time (`assets/rootfs/`). That is how
 *                       the offline installer works, and how a self-build can get an
 *                       offline first run.
 *  - [Source.LocalFile] a file in the app's own directory, put there by [RootfsDownloader]
 *                       after the user asked for it and its SHA-256 matched.
 */
data class RootfsArchive(
    val source: Source,
    val fileName: String,           // e.g. "debian-trixie-aarch64-pd-v4.37.0.tar.xz"
    val distro: String,             // best-effort label, e.g. "kali"
    val arch: String,               // arm64 | armhf | x86_64 | x86 | unknown
    val compression: Compression
) {
    enum class Compression { XZ, GZIP, PLAIN }

    sealed class Source {
        /** Path relative to the APK asset root, e.g. "rootfs/alpine-arm64.tar.xz". */
        data class Asset(val assetPath: String) : Source()
        /** An archive already present on the device filesystem. */
        data class LocalFile(val file: File) : Source()
    }

    /** Opens the archive bytes. The caller owns the stream. */
    fun open(ctx: Context): InputStream = when (source) {
        is Source.Asset -> ctx.assets.open(source.assetPath)
        is Source.LocalFile -> FileInputStream(source.file)
    }

    /**
     * Size in bytes, or 0 when it cannot be determined.
     *
     * For assets this relies on the archive being stored uncompressed, `androidResources
     * { noCompress += ... }` in app/build.gradle.kts guarantees that for the archive
     * extensions we accept. `openFd` throws on a deflated asset, hence the runCatching.
     */
    // `when (val s = source)` rather than `when (source)`: the Asset branch reads the
    // subject inside a lambda, where the compiler will not smart-cast a property. Binding
    // to a local carries the narrowed type into the closure.
    fun sizeBytes(ctx: Context): Long = when (val s = source) {
        is Source.Asset -> runCatching { ctx.assets.openFd(s.assetPath).use { it.length } }
            .getOrDefault(0L)
        is Source.LocalFile -> s.file.length()
    }

    /** Human-readable origin, for logs and for the provisioning UI. */
    fun describe(): String = when (source) {
        is Source.Asset -> "assets/${source.assetPath}"
        is Source.LocalFile -> source.file.absolutePath
    }

    companion object {
        /**
         * Builds a descriptor from a file name, or null when the name is not an archive
         * shape we can read. Shared by asset scanning and on-disk scanning so both routes
         * classify identically.
         */
        fun classify(name: String, source: Source): RootfsArchive? {
            val lower = name.lowercase()
            val comp = when {
                lower.endsWith(".tar.xz") || lower.endsWith(".txz") -> Compression.XZ
                lower.endsWith(".tar.gz") || lower.endsWith(".tgz") -> Compression.GZIP
                lower.endsWith(".tar") -> Compression.PLAIN
                else -> return null
            }
            val arch = when {
                lower.contains("arm64") || lower.contains("aarch64") -> "arm64"
                lower.contains("armhf") || lower.contains("armv7") -> "armhf"
                lower.contains("x86_64") || lower.contains("amd64") -> "x86_64"
                lower.contains("i386") || lower.contains("i686") -> "x86"
                else -> "unknown"
            }
            val distro = lower.substringBefore('-').ifEmpty { "linux" }
            return RootfsArchive(source, name, distro, arch, comp)
        }
    }
}
