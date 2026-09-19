package com.dracxterm.ollama

import android.content.Context
import android.os.Build
import java.io.File

/**
 * Every constant the Ollama integration depends on, in one place.
 *
 * The pinned release is v0.32.14-rc0. [ARTIFACT_SHA256] comes from that release's own
 * sha256sum.txt and matches the GitHub asset digest, so two independent sources agree on
 * it. The archive layout comes from the release workflow's tar file list.
 *
 * The install prefix shape is not a preference. `ml/path.go:LibOllamaPath` resolves the
 * runtime library root on Linux as `dirname(EvalSymlinks(os.Executable()))/../lib/ollama`,
 * so `<prefix>/bin/ollama` must sit next to `<prefix>/lib/ollama` or the binary cannot
 * find libggml and libllama. EvalSymlinks is why a symlink to the binary is still safe.
 *
 * Two variables are deliberately absent. OLLAMA_TMPDIR is not read by envconfig/config.go
 * at this tag, so setting it would be inventing configuration. LD_LIBRARY_PATH is left
 * alone because llama-server carries RUNPATH=$ORIGIN and ollama builds the variable for
 * its own children in llm/llama_server.go; overriding it fights the runtime.
 */
object OllamaConfig {

    const val TAG_LOG = "dracXterm"

    // ---- Pinned official release (never silently substituted) ----------------------------------
    const val TAG = "v0.32.14-rc0"
    const val ARTIFACT = "ollama-linux-arm64.tar.zst"
    const val ARTIFACT_BYTES = 1_542_906_646L
    /** From the release's official sha256sum.txt (line "./ollama-linux-arm64.tar.zst"), which
     *  matches the GitHub asset `digest` field byte-for-byte. */
    const val ARTIFACT_SHA256 = "45b01ab0ed4b1079a80a9287d0346951cff15935de61d00663c4afc83e57bd43"

    const val DOWNLOAD_URL =
        "https://github.com/ollama/ollama/releases/download/$TAG/$ARTIFACT"

    /** The only device ABI this artifact can serve. Matches the module's `abiFilters`. */
    const val REQUIRED_ABI = "arm64-v8a"
    /** ELF e_machine for AArch64 (EM_AARCH64). Enforced by [OllamaInstaller] on the real bytes. */
    const val ELF_MACHINE_AARCH64 = 0xB7

    // ---- Guest (post-PRoot-reroot) paths --------------------------------------------------------
    /** Install root INSIDE the Linux rootfs. Chosen over an app-private dir + PRoot bind because it
     *  requires ZERO change to the spawn-fixed bind list in Bootstrap.prootArgv, see the design
     *  note in OllamaLauncher. Storage accounting is identical: the rootfs is already app-private. */
    const val GUEST_ROOT = "/opt/ollama"
    const val GUEST_PREFIX = "$GUEST_ROOT/$TAG"
    const val GUEST_BIN = "$GUEST_PREFIX/bin/ollama"
    /** First directory on the guest PATH (Bootstrap.kt PATH=/usr/local/sbin:/usr/local/bin:...). */
    const val GUEST_LAUNCHER = "/usr/local/bin/ollama"

    /** Persistent per-user Ollama dir. Backed by host `filesDir/guest-home`, which PRoot binds onto
     *  /home/dracos. It therefore survives a rootfs re-provision and is shared by every workspace ,
     *  which is what makes a single shared server correct across workspaces. */
    const val GUEST_OLLAMA_HOME = "/home/dracos/.ollama"
    const val GUEST_MODELS = "$GUEST_OLLAMA_HOME/models"

    // ---- Environment (only variables proven to be read by envconfig/config.go) -------------------
    const val ENV_HOST = "127.0.0.1:11434"
    const val ENV_MODELS = GUEST_MODELS

    // ---- Rootfs-relative host paths -------------------------------------------------------------
    fun rootfsDir(ctx: Context): File = File(ctx.filesDir, "rootfs")
    fun hostRoot(rootfs: File): File = File(rootfs, "opt/ollama")
    fun hostPrefix(rootfs: File): File = File(hostRoot(rootfs), TAG)
    fun hostBinary(rootfs: File): File = File(hostPrefix(rootfs), "bin/ollama")
    fun hostLibDir(rootfs: File): File = File(hostPrefix(rootfs), "lib/ollama")
    fun hostMarker(rootfs: File): File = File(hostPrefix(rootfs), ".install.json")

    /** Host-side view of the guest's ~/.ollama (the bind source, not a path inside the rootfs). */
    fun hostOllamaHome(ctx: Context): File = File(ctx.filesDir, "guest-home/.ollama")
    fun hostInstallRequest(ctx: Context): File = File(hostOllamaHome(ctx), "install.request")
    fun hostInstallLog(ctx: Context): File = File(hostOllamaHome(ctx), "install.log")
    fun hostInstallState(ctx: Context): File = File(hostOllamaHome(ctx), "install.state")

    /** True when this device can run the pinned artifact at all. Checked before any download. */
    fun abiSupported(): Boolean =
        (Build.SUPPORTED_ABIS ?: emptyArray()).any { it == REQUIRED_ABI }

    /** Free bytes required before starting: artifact + installed payload + 10% headroom. */
    const val REQUIRED_FREE_BYTES = 1_800_000_000L
}
