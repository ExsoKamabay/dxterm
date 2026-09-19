package com.dracxterm.rootfs

import android.content.Context
import android.os.StatFs
import android.util.Log
import java.io.File

/**
 * Refuses an archive before anything expensive reads it: unreadable, truncated, or with
 * magic bytes that disagree with the compression its name claims, and refuses to start an
 * extraction there is plainly not room for.
 *
 * Works the same for an archive bundled in assets and one downloaded on request.
 */
class RootfsValidator(private val ctx: Context) {

    sealed class Result {
        object Ok : Result()
        data class Invalid(val reason: String) : Result()
    }

    fun validate(archive: RootfsArchive, targetDir: File): Result {
        Log.i(ShellLocator.TAG, "[VALIDATOR] Archive validation started: ${archive.fileName} (${archive.describe()})")
        // 1. readable + at least a full header
        val magic = ByteArray(6)
        val read = runCatching {
            archive.open(ctx).use { ins ->
                var off = 0
                while (off < 6) { val n = ins.read(magic, off, 6 - off); if (n < 0) break; off += n }
                off
            }
        }.getOrElse { return Result.Invalid("archive not readable: ${it.message}") }
        if (read < 6) return Result.Invalid("archive is empty or truncated")

        // 2. magic matches declared compression
        val magicOk = when (archive.compression) {
            RootfsArchive.Compression.XZ ->
                magic[0] == 0xFD.toByte() && magic[1] == 0x37.toByte() && magic[2] == 0x7A.toByte() &&
                magic[3] == 0x58.toByte() && magic[4] == 0x5A.toByte() && magic[5] == 0x00.toByte()
            RootfsArchive.Compression.GZIP ->
                magic[0] == 0x1F.toByte() && magic[1] == 0x8B.toByte()
            RootfsArchive.Compression.PLAIN -> true   // ustar magic is checked by the tar reader
        }
        if (!magicOk) return Result.Invalid("header does not match ${archive.compression} for ${archive.fileName}")
        Log.i(ShellLocator.TAG, "[VALIDATOR] Magic bytes OK (${archive.compression})")

        // 3. free-space heuristic (need ~4x the compressed size for a typical rootfs)
        val compressed = archive.sizeBytes(ctx)
        Log.i(ShellLocator.TAG, "[VALIDATOR] Archive size: $compressed bytes")
        if (compressed > 0L) {
            val base = targetDir.parentFile?.absolutePath ?: ctx.filesDir.absolutePath
            val free = runCatching { StatFs(base).availableBytes }.getOrDefault(Long.MAX_VALUE)
            if (free in 0 until compressed * 4)
                return Result.Invalid("insufficient storage: need ~${compressed * 4} bytes, free $free")
        }
        return Result.Ok
    }
}
