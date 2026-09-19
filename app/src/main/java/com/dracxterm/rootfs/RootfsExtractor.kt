package com.dracxterm.rootfs

import android.content.Context
import android.system.Os
import android.util.Log
import com.dracxterm.archive.ArchivePaths
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.apache.commons.compress.compressors.gzip.GzipCompressorInputStream
import org.apache.commons.compress.compressors.xz.XZCompressorInputStream
import java.io.BufferedInputStream
import java.io.File
import java.io.InputStream

/**
 * Streams an archive into the rootfs directory: xz, gzip or plain tar, into a ".tmp"
 * staging tree, then renamed into place.
 *
 * A single wrapping top-level directory is stripped, because some images are packed as
 * `alpine-minirootfs-3.19/...` and some are packed at the root. Entry names and link
 * targets both go through [ArchivePaths]. Device and FIFO nodes are skipped: proot
 * virtualises /dev, and an app has no business creating them anyway.
 */
class RootfsExtractor(private val ctx: Context) {

    private val tag = ShellLocator.TAG

    fun interface Progress { fun onProgress(entries: Long, bytes: Long, currentPath: String) }

    sealed class Result {
        data class Ok(val entries: Long) : Result()
        data class Failed(val reason: String) : Result()
    }

    fun extract(archive: RootfsArchive, finalDir: File, progress: Progress?): Result {
        val tmpDir = File(finalDir.parentFile, finalDir.name + ".tmp")
        runCatching { if (tmpDir.exists()) tmpDir.deleteRecursively() }
        if (!tmpDir.mkdirs()) return Result.Failed("cannot create staging dir ${tmpDir.absolutePath}")
        val tmpCanonical = tmpDir.canonicalPath
        Log.i(tag, "[EXTRACTOR] Extraction started: ${archive.fileName} (${archive.compression}) from ${archive.describe()}")
        Log.i(tag, "[EXTRACTOR] Destination directory: ${finalDir.absolutePath}")

        var count = 0L
        var total = 0L
        try {
            tarStream(archive).use { tar ->
                val buf = ByteArray(64 * 1024)
                var e: TarArchiveEntry? = tar.nextEntry
                while (e != null) {
                    val entry = e
                    val name = ArchivePaths.normalise(entry.name)
                    // The archive's own root directory (`./`, the first member of every
                    // `tar -c .`) names the staging tree itself, so there is nothing to unpack
                    // and its normalised name is empty. Skipped by name before the empty-name
                    // refusal below, which is what aborted Alpine on its very first entry.
                    if (ArchivePaths.isArchiveRoot(name)) { e = tar.nextEntry; continue }
                    // Link targets are checked too, not only entry names. Os.symlink and Os.link
                    // take the target verbatim, so `../../..` reaches out of the staging tree
                    // while the entry name itself looks ordinary; the canonical-path test below
                    // never sees it because it only ever looks at the entry.
                    //
                    // Which kind of link it is travels with the target, because an absolute
                    // target is ordinary on one and never legitimate on the other.
                    val link = when {
                        entry.isSymbolicLink -> ArchivePaths.Link.Sym(entry.linkName)
                        entry.isLink -> ArchivePaths.Link.Hard(entry.linkName)
                        else -> null
                    }
                    ArchivePaths.refuse(name, link)?.let { return Result.Failed(it) }

                    val outFile = File(tmpDir, name)
                    val canon = outFile.canonicalPath
                    if (canon != tmpCanonical && !canon.startsWith(tmpCanonical + File.separator)) {
                        return Result.Failed("unsafe path in archive: ${entry.name}")
                    }
                    when {
                        entry.isDirectory -> outFile.mkdirs()
                        entry.isSymbolicLink -> {
                            outFile.parentFile?.mkdirs()
                            if (ShellLocator.entryExists(outFile.absolutePath)) outFile.delete()
                            runCatching { Os.symlink(entry.linkName, outFile.absolutePath) }
                        }
                        entry.isLink -> {
                            outFile.parentFile?.mkdirs()
                            val src = File(tmpDir, ArchivePaths.normalise(entry.linkName))
                            runCatching { Os.link(src.absolutePath, outFile.absolutePath) }
                                .onFailure { runCatching { src.copyTo(outFile, overwrite = true) } }
                        }
                        entry.isCharacterDevice || entry.isBlockDevice || entry.isFIFO -> { /* skip */ }
                        else -> {
                            outFile.parentFile?.mkdirs()
                            outFile.outputStream().use { os ->
                                var n = tar.read(buf)
                                while (n > 0) { os.write(buf, 0, n); total += n; n = tar.read(buf) }
                            }
                        }
                    }
                    if (!entry.isSymbolicLink) runCatching { Os.chmod(outFile.absolutePath, entry.mode and 0xFFF) }
                    count++
                    if (count % 250L == 0L) progress?.onProgress(count, total, entry.name)
                    e = tar.nextEntry
                }
            }
        } catch (t: Throwable) {
            runCatching { tmpDir.deleteRecursively() }
            return Result.Failed("extraction error: ${t.message}")
        }

        // Strip a single wrapping top-level directory (e.g. archives made with `tar czf x.tgz alpine/`).
        val stripped = stripSingleTopLevelDir(tmpDir)
        if (stripped != null) Log.i(tag, "[EXTRACTOR] Stripped wrapping top-level directory: $stripped")

        Log.i(tag, "[EXTRACTOR] Files extracted: $count (bytes=$total)")

        // The old tree is moved aside, not deleted, until the new one is in place. Deleting
        // first leaves a window in which a kill between the delete and the rename takes the
        // user's working rootfs with it and leaves nothing behind.
        val previous = File(finalDir.parentFile, finalDir.name + ".old")
        runCatching { if (previous.exists()) previous.deleteRecursively() }
        val hadPrevious = finalDir.exists() && finalDir.renameTo(previous)
        if (finalDir.exists() && !hadPrevious) {
            return Result.Failed("cannot move the existing rootfs aside; it was left untouched")
        }
        if (!tmpDir.renameTo(finalDir)) {
            val ok = runCatching {
                tmpDir.copyRecursively(finalDir, overwrite = true); tmpDir.deleteRecursively()
            }.isSuccess
            if (!ok) {
                // The copy failed part-way, so finalDir now holds a partial tree. Putting the
                // previous rootfs back has to clear that first: rename(2) onto a non-empty
                // directory fails with ENOTEMPTY, so the old restore silently did nothing and
                // left a broken rootfs in place with the user's working one stranded in .old --
                // which the next provisioning run then deleted.
                runCatching { finalDir.deleteRecursively() }
                val restored = hadPrevious && previous.renameTo(finalDir)
                runCatching { tmpDir.deleteRecursively() }
                return Result.Failed(
                    if (hadPrevious && !restored)
                        "cannot finalise rootfs directory, and the previous one could not be " +
                            "restored; it is still at ${previous.name}"
                    else "cannot finalise rootfs directory"
                )
            }
        }
        runCatching { previous.deleteRecursively() }

        // Audit the real post-extraction layout.
        Log.i(tag, "[EXTRACTOR] rootfs/     = ${listing(finalDir)}")
        Log.i(tag, "[EXTRACTOR] rootfs/bin  = ${listing(File(finalDir, "bin"))}")
        Log.i(tag, "[EXTRACTOR] rootfs/usr/bin = ${listing(File(finalDir, "usr/bin"))}")

        progress?.onProgress(count, total, "done")
        return Result.Ok(count)
    }

    /**
     * If [dir] contains exactly one child and that child is a directory holding bin/ or usr/,
     * move the child's contents up into [dir] and return the stripped prefix name.
     */
    private fun stripSingleTopLevelDir(dir: File): String? {
        val children = dir.listFiles() ?: return null
        if (children.size != 1 || !children[0].isDirectory) return null
        val sub = children[0]
        val looksLikeRoot = File(sub, "bin").isDirectory || File(sub, "usr").isDirectory ||
                            ShellLocator.entryExists(File(sub, "bin/sh").absolutePath)
        if (!looksLikeRoot) return null
        val staging = File(dir, ".__pull__")
        if (!sub.renameTo(staging)) return null            // rename to avoid name clashes while moving
        (staging.listFiles() ?: emptyArray()).forEach { child ->
            val dest = File(dir, child.name)
            if (!child.renameTo(dest)) child.copyRecursively(dest, overwrite = true)
        }
        staging.deleteRecursively()
        return sub.name
    }

    private fun tarStream(archive: RootfsArchive): TarArchiveInputStream {
        // Source-agnostic: assets/ for self-built APKs, a device file for images the user
        // asked drac-Xterm to fetch or supplied themselves.
        val raw: InputStream = BufferedInputStream(archive.open(ctx), 1 shl 16)
        val decomp: InputStream = when (archive.compression) {
            RootfsArchive.Compression.XZ   -> XZCompressorInputStream(raw)
            RootfsArchive.Compression.GZIP -> GzipCompressorInputStream(raw)
            RootfsArchive.Compression.PLAIN -> raw
        }
        return TarArchiveInputStream(decomp)
    }

    private fun listing(d: File): String =
        if (!d.isDirectory) "(absent)" else (d.list()?.sorted()?.take(60)?.joinToString(", ") ?: "(empty)")
}
