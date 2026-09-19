package com.dracxterm.ollama

import android.util.Log
import org.json.JSONObject
import java.io.File

/**
 * Persistent, crash-safe installation state.
 *
 * Two independent records, deliberately:
 *
 *  A. PROGRESS  , `guest-home/.ollama/install.state`, a single line the guest launcher can `cat`
 *     to tell the user what is happening. Volatile; safe to lose.
 *
 *  B. READINESS, `<prefix>/.install.json`, written LAST, INSIDE the versioned install directory.
 *     Readiness is therefore never a stale flag: it is re-derived from the filesystem every time
 *     ([isReady] re-checks the marker AND the real binary AND the library directory). If the rootfs
 *     is re-provisioned (RootfsExtractor deletes and recreates filesDir/rootfs), the marker goes
 *     with it and the runtime correctly reports NOT_INSTALLED. No SharedPreferences flag can
 *     survive to lie about an installation that no longer exists.
 *
 * A partially extracted tree can never be mistaken for an installation: extraction happens in a
 * `.stage-*` sibling directory and the versioned directory only ever comes into existence through
 * one atomic File.renameTo AFTER validation ([OllamaInstaller.promote]).
 */
object OllamaState {

    enum class Phase { NOT_INSTALLED, DOWNLOADING, VERIFYING, EXTRACTING, INSTALLING, READY, FAILED }

    /** Write the human-readable progress line the guest launcher tails. Never throws. */
    fun publish(stateFile: File, phase: Phase, detail: String = "", percent: Int = -1) {
        runCatching {
            stateFile.parentFile?.mkdirs()
            val pct = if (percent in 0..100) " $percent%" else ""
            stateFile.writeText("${phase.name}$pct ${detail}\n")
        }
    }

    fun readPhase(stateFile: File): Phase = runCatching {
        Phase.valueOf(stateFile.readText().trim().substringBefore(' ').substringBefore('\n'))
    }.getOrDefault(Phase.NOT_INSTALLED)

    /**
     * Readiness = marker present AND parseable AND version matches AND the real files exist.
     * Deliberately re-derived from disk on every call.
     */
    fun isReady(rootfs: File): Boolean {
        val marker = OllamaConfig.hostMarker(rootfs)
        if (!marker.isFile) return false
        val ok = runCatching {
            val o = JSONObject(marker.readText())
            o.optString("version") == OllamaConfig.TAG && o.optBoolean("ready", false)
        }.getOrDefault(false)
        if (!ok) return false
        val bin = OllamaConfig.hostBinary(rootfs)
        if (!bin.isFile || bin.length() <= 0L) return false
        val lib = OllamaConfig.hostLibDir(rootfs)
        return lib.isDirectory && (lib.list()?.isNotEmpty() == true)
    }

    /** Write the readiness marker. Called only after every validation has passed. */
    fun markReady(
        rootfs: File,
        archiveSha256: String,
        installedBytes: Long,
        keptEntries: Int,
        droppedEntries: Int,
        droppedBytes: Long,
        droppedRunners: Collection<String>,
        installedAtEpochMs: Long
    ) {
        val o = JSONObject()
            .put("ready", true)
            .put("version", OllamaConfig.TAG)
            .put("artifact", OllamaConfig.ARTIFACT)
            .put("artifact_sha256", archiveSha256)
            .put("guest_prefix", OllamaConfig.GUEST_PREFIX)
            .put("guest_binary", OllamaConfig.GUEST_BIN)
            .put("guest_models", OllamaConfig.GUEST_MODELS)
            .put("installed_bytes", installedBytes)
            .put("kept_entries", keptEntries)
            .put("dropped_entries", droppedEntries)
            .put("dropped_bytes", droppedBytes)
            .put("dropped_runners", droppedRunners.sorted().joinToString(","))
            .put("installed_at_ms", installedAtEpochMs)
        runCatching { OllamaConfig.hostMarker(rootfs).writeText(o.toString(2)) }
            .onFailure { Log.w(OllamaConfig.TAG_LOG, "[OLLAMA] marker write failed: ${it.message}") }
    }

    /** Remove abandoned `.stage-*` directories from a previous crashed attempt. Never fatal. */
    fun sweepStaging(rootfs: File) {
        runCatching {
            OllamaConfig.hostRoot(rootfs).listFiles()
                ?.filter { it.isDirectory && it.name.startsWith(".stage-") }
                ?.forEach {
                    Log.i(OllamaConfig.TAG_LOG, "[OLLAMA] sweeping stale staging dir ${it.name}")
                    it.deleteRecursively()
                }
        }
    }
}
