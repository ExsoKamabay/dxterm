package com.xdrac.ollama

import com.github.luben.zstd.ZstdInputStream
import java.io.InputStream

/**
 * The one place that names a Zstandard implementation.
 *
 * Zstd is the only format the pinned Ollama artifact ships in, and it is the only thing
 * this file knows: tar entries belong to the layer above. Keeping the dependency behind
 * one call means swapping decoders touches this file and nothing else.
 *
 * The version pin lives in app/build.gradle.kts, where the reason for it is written down.
 */
object ZstdDecoder {

    /** Named in logs and in the install marker, so a marker says what decoded it. */
    const val IMPLEMENTATION = "zstd-jni 1.5.7-12 (native, streaming)"

    /**
     * Wraps [compressed] in a streaming decoder with a bounded internal buffer, so the
     * 1.44 GiB artifact is never read into a byte[] and never fully written back to disk.
     *
     * A corrupt header, a bad frame, a checksum failure or a truncated stream surfaces as
     * an IOException from a later read(), which [OllamaInstaller] turns into a failed
     * install with its staging tree deleted.
     */
    fun decode(compressed: InputStream): InputStream = ZstdInputStream(compressed)
}
