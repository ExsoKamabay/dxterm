package com.xdrac.ollama

import android.content.Context
import android.system.Os
import android.util.Log
import com.xdrac.archive.ArchivePaths
import com.xdrac.net.HttpsOnly
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.io.InputStream
import java.security.DigestInputStream
import java.security.MessageDigest

/**
 * Provisions the pinned official Ollama ARM64 Linux runtime.
 *
 * The pipeline, and the order matters: nothing reads the archive's structure until its
 * SHA-256 has been checked.
 *
 *     ABI + capacity preflight
 *            v
 *     download official artifact -> <root>/.download.tmp   (SHA-256 computed on the fly)
 *            v
 *     VERIFY SHA-256 against the pinned official checksum   <-- nothing is parsed before this
 *            v
 *     zstd STREAM -> tar STREAM -> OllamaPayloadPolicy filter -> <root>/.stage-<n>
 *            v
 *     validate staged tree (layout + real ELF header + AArch64 e_machine)
 *            v
 *     ATOMIC promote: renameTo(<root>/<version>)            <-- no second full copy
 *            v
 *     write readiness marker LAST
 *
 * FAILURE SEMANTICS: every failure path deletes only the staging directory and the temp download.
 * An existing valid installation is never touched, modified in place, or removed on failure.
 *
 * MEMORY: bounded. ZstdInputStream is a FilterInputStream with a fixed internal source buffer
 * (ZSTD_DStreamInSize, ~128 KiB); the tar layer is streamed; the copy buffer here is 64 KiB. The
 * 1.44 GiB archive is never held in RAM, and is never fully decompressed to a .tar on disk.
 */
class OllamaInstaller(private val ctx: Context) {

    sealed class Result {
        data class Ok(
            val installedBytes: Long,
            val keptEntries: Int,
            val droppedEntries: Int,
            val droppedBytes: Long,
            val droppedRunners: Set<String>
        ) : Result()
        data class Failed(val reason: String) : Result()
    }

    fun interface Progress { fun onProgress(phase: OllamaState.Phase, detail: String, percent: Int) }

    private val tag = OllamaConfig.TAG_LOG
    private val stateFile = OllamaConfig.hostInstallState(ctx)

    fun install(progress: Progress?): Result {
        val rootfs = OllamaConfig.rootfsDir(ctx)
        val root = OllamaConfig.hostRoot(rootfs)

        fun emit(p: OllamaState.Phase, d: String, pct: Int = -1) {
            OllamaState.publish(stateFile, p, d, pct)
            progress?.onProgress(p, d, pct)
            Log.i(tag, "[OLLAMA] $p $d")
        }

        // ---------- 0. Preflight -----------------------------------------------------------------
        if (!rootfs.isDirectory) {
            return fail(emitFail("Linux rootfs is not provisioned ($rootfs)"))
        }
        if (!OllamaConfig.abiSupported()) {
            return fail(emitFail(
                "device ABI is not ${OllamaConfig.REQUIRED_ABI}; the pinned artifact " +
                    "${OllamaConfig.ARTIFACT} cannot run here"
            ))
        }
        if (OllamaState.isReady(rootfs)) {
            emit(OllamaState.Phase.READY, "already installed at ${OllamaConfig.GUEST_PREFIX}", 100)
            return Result.Ok(dirBytes(OllamaConfig.hostPrefix(rootfs)), 0, 0, 0L, emptySet())
        }
        if (!root.exists() && !root.mkdirs()) {
            return fail(emitFail("cannot create ${root.absolutePath}"))
        }
        OllamaState.sweepStaging(rootfs)

        // A failed usableSpace reads as unknown, not as zero: an unknown figure is no
        // evidence of a full disk, while a genuine zero is exactly the case to refuse.
        val free = runCatching { root.usableSpace }.getOrDefault(-1L)
        if (free in 0 until OllamaConfig.REQUIRED_FREE_BYTES) {
            return fail(emitFail(
                "insufficient free space: need ~${OllamaConfig.REQUIRED_FREE_BYTES / 1_000_000} MB " +
                    "(artifact + install + headroom), have ${free / 1_000_000} MB"
            ))
        }

        val tmp = File(root, ".download.tmp")
        val stage = File(root, ".stage-${OllamaConfig.TAG}")
        runCatching { tmp.delete() }
        runCatching { if (stage.exists()) stage.deleteRecursively() }

        try {
            // ---------- 1. Download (hash while writing; no extra read pass) ---------------------
            emit(OllamaState.Phase.DOWNLOADING, "${OllamaConfig.ARTIFACT} (${OllamaConfig.TAG})", 0)
            val digest = MessageDigest.getInstance("SHA-256")
            val got = download(OllamaConfig.DOWNLOAD_URL, tmp, digest) { read ->
                val pct = if (OllamaConfig.ARTIFACT_BYTES > 0)
                    ((read * 100L) / OllamaConfig.ARTIFACT_BYTES).toInt().coerceIn(0, 100) else -1
                emit(OllamaState.Phase.DOWNLOADING, "${read / 1_000_000} / " +
                    "${OllamaConfig.ARTIFACT_BYTES / 1_000_000} MB", pct)
            }

            // ---------- 2. VERIFY before anything reads the archive's structure -------------------
            emit(OllamaState.Phase.VERIFYING, "SHA-256 of the downloaded artifact", -1)
            if (got != OllamaConfig.ARTIFACT_BYTES) {
                return fail(emitFail(
                    "size mismatch: expected ${OllamaConfig.ARTIFACT_BYTES} bytes, got $got " +
                        "(truncated or corrupt download)"
                ))
            }
            val sha = digest.digest().joinToString("") { "%02x".format(it) }
            if (!sha.equals(OllamaConfig.ARTIFACT_SHA256, ignoreCase = true)) {
                return fail(emitFail(
                    "SHA-256 mismatch, REJECTED. expected=${OllamaConfig.ARTIFACT_SHA256} actual=$sha"
                ))
            }
            Log.i(tag, "[OLLAMA] integrity OK: sha256=$sha")

            // ---------- 3. Stream: zstd -> tar -> policy filter -> staging ------------------------
            emit(OllamaState.Phase.EXTRACTING,
                "streaming zstd -> tar via ${ZstdDecoder.IMPLEMENTATION} (dropping accelerator runners)", -1)
            if (!stage.mkdirs()) return fail(emitFail("cannot create staging dir"))
            val x = extract(tmp, stage) { kept, dropped ->
                emit(OllamaState.Phase.EXTRACTING, "kept $kept, dropped $dropped", -1)
            }
            if (x is Extraction.Failed) return fail(emitFail(x.reason))
            val ex = x as Extraction.Ok

            // ---------- 4. Validate the staged tree (real bytes, not assumptions) -----------------
            emit(OllamaState.Phase.INSTALLING, "validating staged runtime", -1)
            validateStaged(stage)?.let { return fail(emitFail(it)) }

            // ---------- 5. Atomic promotion (rename, never copy) ---------------------------------
            promote(stage, OllamaConfig.hostPrefix(rootfs))?.let { return fail(emitFail(it)) }

            val installed = dirBytes(OllamaConfig.hostPrefix(rootfs))
            OllamaState.markReady(
                rootfs, sha, installed, ex.kept, ex.dropped, ex.droppedBytes,
                ex.droppedRunners, System.currentTimeMillis()
            )
            emit(OllamaState.Phase.READY, "installed at ${OllamaConfig.GUEST_PREFIX} " +
                "(${installed / 1_000_000} MB)", 100)
            return Result.Ok(installed, ex.kept, ex.dropped, ex.droppedBytes, ex.droppedRunners)
        } catch (t: Throwable) {
            return fail(emitFail(t.message ?: t.javaClass.simpleName))
        } finally {
            runCatching { tmp.delete() }
            runCatching { if (stage.exists()) stage.deleteRecursively() }
        }
    }

    // ---------------------------------------------------------------------------------------------

    private fun emitFail(reason: String): String {
        OllamaState.publish(stateFile, OllamaState.Phase.FAILED, reason)
        Log.w(tag, "[OLLAMA] FAILED: $reason")
        return reason
    }

    private fun fail(reason: String) = Result.Failed(reason)

    /** Streaming download with on-the-fly digest. */
    private fun download(
        url: String,
        dest: File,
        digest: MessageDigest,
        onBytes: (Long) -> Unit
    ): Long {
        val c = HttpsOnly.open(url) { conn ->
            conn.connectTimeout = 30_000
            conn.readTimeout = 60_000
            conn.setRequestProperty("Accept", "application/octet-stream")
            conn.setRequestProperty("User-Agent", "drac-Xterm/1.0 (+ollama-provisioner)")
        }
        try {
            if (c.responseCode != 200) {
                throw IOException("HTTP ${c.responseCode} fetching the pinned release artifact")
            }
            DigestInputStream(BufferedInputStream(c.inputStream, 1 shl 16), digest).use { input ->
                dest.outputStream().use { out ->
                    val buf = ByteArray(64 * 1024)
                    var total = 0L
                    var lastTick = 0L
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        total += n
                        if (total - lastTick >= 25_000_000L) { lastTick = total; onBytes(total) }
                    }
                    out.flush()
                    return total
                }
            }
        } finally {
            runCatching { c.disconnect() }
        }
    }

    private sealed class Extraction {
        data class Ok(
            val kept: Int, val dropped: Int, val droppedBytes: Long, val droppedRunners: Set<String>
        ) : Extraction()
        data class Failed(val reason: String) : Extraction()
    }

    /**
     * zstd stream -> tar stream -> [OllamaPayloadPolicy] -> staging. Nothing is buffered whole.
     * The staging root canonical path is re-checked for every entry (belt-and-braces on top of the
     * policy's syntactic traversal guard) so no archive name can place bytes outside the stage.
     */
    private fun extract(archive: File, stage: File, tick: (Int, Int) -> Unit): Extraction {
        val stageCanonical = stage.canonicalPath
        var kept = 0; var dropped = 0; var droppedBytes = 0L
        val droppedRunners = LinkedHashSet<String>()
        val unknownKept = LinkedHashSet<String>()

        try {
            tarStream(archive).use { tar ->
                val buf = ByteArray(64 * 1024)
                var e: TarArchiveEntry? = tar.nextEntry
                while (e != null) {
                    val entry = e
                    val name = ArchivePaths.normalise(entry.name)
                    // Same archive-root skip as RootfsExtractor: `./` carries no payload and
                    // would otherwise be refused as an empty name.
                    if (ArchivePaths.isArchiveRoot(name.trimEnd('/'))) { e = tar.nextEntry; continue }
                    val link = when {
                        entry.isSymbolicLink -> ArchivePaths.Link.Sym(entry.linkName)
                        entry.isLink -> ArchivePaths.Link.Hard(entry.linkName)
                        else -> null
                    }

                    if (entry.isDirectory) {
                        // Directories carry no payload; create only those on the keep side.
                        when (OllamaPayloadPolicy.classify(name.trimEnd('/') , null)) {
                            is OllamaPayloadPolicy.Decision.Keep ->
                                File(stage, name).mkdirs()
                            is OllamaPayloadPolicy.Decision.Reject ->
                                return Extraction.Failed("unsafe directory entry: $name")
                            is OllamaPayloadPolicy.Decision.DropAccelerator -> { /* skip */ }
                        }
                        e = tar.nextEntry
                        continue
                    }

                    when (val d = OllamaPayloadPolicy.classify(name, link)) {
                        is OllamaPayloadPolicy.Decision.Reject ->
                            return Extraction.Failed("archive rejected: ${d.why}")

                        is OllamaPayloadPolicy.Decision.DropAccelerator -> {
                            dropped++
                            droppedBytes += entry.size
                            droppedRunners += d.runner
                            // Consume nothing explicitly: TarArchiveInputStream skips to the next
                            // header on nextEntry(), so the payload is never written to disk.
                        }

                        is OllamaPayloadPolicy.Decision.Keep -> {
                            if (d.why.startsWith("unrecognised")) unknownKept += name
                            val out = File(stage, name)
                            val canon = out.canonicalPath
                            if (canon != stageCanonical &&
                                !canon.startsWith(stageCanonical + File.separator)
                            ) {
                                return Extraction.Failed("path escapes staging dir: $name")
                            }
                            out.parentFile?.mkdirs()
                            when {
                                entry.isSymbolicLink -> {
                                    runCatching { if (exists(out)) Os.remove(out.absolutePath) }
                                    Os.symlink(entry.linkName, out.absolutePath)
                                }
                                entry.isLink -> {
                                    val src = File(stage, ArchivePaths.normalise(entry.linkName))
                                    runCatching { Os.link(src.absolutePath, out.absolutePath) }
                                        .onFailure { runCatching { src.copyTo(out, overwrite = true) } }
                                }
                                else -> {
                                    out.outputStream().use { os ->
                                        var n = tar.read(buf)
                                        while (n > 0) { os.write(buf, 0, n); n = tar.read(buf) }
                                    }
                                    // Preserve the archive mode so bin/ollama stays executable.
                                    runCatching { Os.chmod(out.absolutePath, entry.mode and 0xFFF) }
                                }
                            }
                            kept++
                        }
                    }
                    if ((kept + dropped) % 8 == 0) tick(kept, dropped)
                    e = tar.nextEntry
                }
            }
        } catch (t: Throwable) {
            return Extraction.Failed("decompression/extraction error: ${t.message ?: t.javaClass.simpleName}")
        }

        if (unknownKept.isNotEmpty()) {
            Log.w(tag, "[OLLAMA] kept ${unknownKept.size} unrecognised archive path(s) " +
                "(conservative policy): ${unknownKept.take(10).joinToString(", ")}")
        }
        Log.i(tag, "[OLLAMA] filter result: kept=$kept dropped=$dropped " +
            "droppedBytes=$droppedBytes runners=${droppedRunners.joinToString(",")}")
        return Extraction.Ok(kept, dropped, droppedBytes, droppedRunners)
    }

    /** [ZstdDecoder] decodes the compression frame; tar owns the entries. Naming the zstd
     *  implementation in one file keeps it swappable without touching this pipeline. */
    private fun tarStream(archive: File): TarArchiveInputStream {
        val raw: InputStream = BufferedInputStream(FileInputStream(archive), 1 shl 16)
        val zstd: InputStream = ZstdDecoder.decode(raw)
        return TarArchiveInputStream(zstd)
    }

    private fun exists(f: File): Boolean =
        runCatching { Os.lstat(f.absolutePath); true }.getOrDefault(false)

    /**
     * Validate the STAGED tree before it is allowed to become an installation.
     * Returns null on success, or a human-readable blocker. Reads the real ELF header rather
     * than trusting the archive's own layout, and refuses a binary that is not AArch64 instead
     * of installing something the device cannot execute.
     */
    private fun validateStaged(stage: File): String? {
        val bin = File(stage, "bin/ollama")
        if (!bin.isFile) return "staged tree has no bin/ollama"
        if (bin.length() < 1_000_000L) return "staged bin/ollama is implausibly small (${bin.length()} bytes)"

        val lib = File(stage, "lib/ollama")
        if (!lib.isDirectory) return "staged tree has no lib/ollama"
        val libFiles = lib.listFiles()?.filter { it.isFile } ?: emptyList()
        if (libFiles.isEmpty()) return "staged lib/ollama is empty, the CPU runtime was not extracted"
        if (!File(lib, "llama-server").exists()) return "staged lib/ollama has no llama-server"
        if (libFiles.none { it.name.startsWith("libggml-cpu-") })
            return "staged lib/ollama has no libggml-cpu-* backend, CPU inference would be impossible"

        // ELF header: magic, 64-bit, little-endian, e_machine == EM_AARCH64 (0xB7).
        val h = ByteArray(20)
        bin.inputStream().use { if (it.read(h) < 20) return "cannot read ELF header of bin/ollama" }
        if (h[0] != 0x7F.toByte() || h[1] != 'E'.code.toByte() ||
            h[2] != 'L'.code.toByte() || h[3] != 'F'.code.toByte()
        ) return "bin/ollama is not an ELF binary"
        if (h[4].toInt() != 2) return "bin/ollama is not ELF64"
        if (h[5].toInt() != 1) return "bin/ollama is not little-endian"
        val machine = (h[18].toInt() and 0xFF) or ((h[19].toInt() and 0xFF) shl 8)
        if (machine != OllamaConfig.ELF_MACHINE_AARCH64)
            return "bin/ollama e_machine=0x%02X, expected 0x%02X (AArch64), REJECTED"
                .format(machine, OllamaConfig.ELF_MACHINE_AARCH64)

        Log.i(tag, "[OLLAMA] ELF OK: ELF64 LE AArch64, ${bin.length()} bytes; " +
            "lib/ollama has ${libFiles.size} files")
        return null
    }

    /**
     * Atomic promotion. If a directory for this version already exists it is left alone (another
     * install won the race) and the staging tree is discarded, never merged, never overwritten.
     */
    private fun promote(stage: File, target: File): String? {
        if (target.exists()) {
            Log.i(tag, "[OLLAMA] ${target.name} already present; discarding staging tree")
            runCatching { stage.deleteRecursively() }
            return null
        }
        target.parentFile?.mkdirs()
        if (!stage.renameTo(target)) {
            return "atomic promotion failed (rename ${stage.name} -> ${target.name}); " +
                "existing installation left untouched"
        }
        Log.i(tag, "[OLLAMA] promoted -> ${target.absolutePath}")
        return null
    }

    private fun dirBytes(d: File): Long =
        runCatching { d.walkTopDown().filter { it.isFile }.sumOf { it.length() } }.getOrDefault(0L)
}
