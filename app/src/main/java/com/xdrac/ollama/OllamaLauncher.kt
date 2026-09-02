package com.xdrac.ollama

import android.content.Context
import android.system.Os
import android.util.Log
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The only entry point the rest of the app touches. Bootstrap calls [attach] once per session
 * spawn; everything else in this package is reached from here.
 *
 * ----------------------------------------------------------------------------------------------
 * WHY THE RUNTIME IS INSTALLED INSIDE THE ROOTFS RATHER THAN BIND-MOUNTED
 * ----------------------------------------------------------------------------------------------
 * Both options were evaluated against the existing source:
 *
 *   Option A, install app-private (filesDir/ollama) and add `-b <src>:/opt/ollama` to the PRoot
 *              invocation. This REQUIRES editing the bind list in Bootstrap.prootArgv. That list
 *              is spawn-fixed (a running PRoot can never be re-bound) and is the single most
 *              regression-prone area of this codebase: the long comment at Bootstrap.kt:188-207
 *              documents a real, already-fixed bug caused by bind/mountpoint subtleties
 *              (nested binds failing to stat under proot 5.1.0), plus a second bug caused by
 *              gating a bind on a start-time snapshot. Adding a bind re-enters that hazard.
 *
 *   Option B, install into the rootfs itself at /opt/ollama/<version>. Requires ZERO change to
 *              the PRoot argv, the bind list, the env array, or the launch path.
 *
 * Storage is identical either way: filesDir/rootfs is already app-private, so both land on the
 * same partition with the same accounting. Option B is therefore strictly lower regression risk
 * for equal benefit, and it is what this module does. The one consequence, a rootfs
 * re-provision wipes the runtime, is correct behaviour (a fresh Linux environment should be
 * freshly provisioned) and is handled: [OllamaState.isReady] re-derives readiness from the real
 * files every time, so a wiped install can never be reported as present.
 * ----------------------------------------------------------------------------------------------
 *
 * Everything here is best-effort and non-fatal. A failure in this module must never delay or
 * block the terminal, which is why every call site is wrapped and only logs on failure.
 */
object OllamaLauncher {

    private val tag = OllamaConfig.TAG_LOG
    private val watcherStarted = AtomicBoolean(false)
    private val installing = AtomicBoolean(false)
    private val worker = Executors.newSingleThreadExecutor { r ->
        Thread(r, "ollama-provisioner").apply { isDaemon = true; priority = Thread.MIN_PRIORITY }
    }

    /**
     * Install the `ollama` command onto the shell's PATH and arm the provisioning watcher.
     *
     * Called once per session spawn from Bootstrap (a single added line), mirroring exactly how
     * the existing `xset` command is installed, refreshed every launch so an already-provisioned
     * rootfs picks up changes without a re-provision.
     *
     * @param rootfs the extracted Linux rootfs, or null when running the BusyBox fallback.
     * @param busyboxBinDir filesDir/usr/bin, the first entry on the BusyBox PATH.
     */
    fun attach(ctx: Context, rootfs: File?, busyboxBinDir: String) {
        // The Activity is what reaches this call (MainActivity -> TerminalSession ->
        // Bootstrap.prepare), and [startWatcher] hands its argument to a static executor and a
        // thread that never returns, so passing the Activity through pinned the whole view
        // hierarchy for the life of the process. Nothing here needs anything but the app
        // context, so take that once, at the boundary.
        val app = ctx.applicationContext
        runCatching {
            if (rootfs != null) {
                installGuestLauncher(app, rootfs)
                startWatcher(app)
            } else {
                installBusyboxStub(app, busyboxBinDir)
            }
        }.onFailure { Log.w(tag, "[OLLAMA] attach skipped: ${it.message}") }
    }

    // ---------------------------------------------------------------------------------------------

    /** Write the launcher into the rootfs's /usr/local/bin (first standard dir on the guest PATH),
     *  substituting the pinned configuration so OllamaConfig stays the single source of truth. */
    private fun installGuestLauncher(ctx: Context, rootfs: File) {
        val binDir = File(rootfs, "usr/local/bin").apply { runCatching { mkdirs() } }
        val target = File(binDir, "ollama")
        val body = ctx.assets.open("ollama/launcher.sh").bufferedReader().use { it.readText() }
            .replace("@PREFIX@", OllamaConfig.GUEST_PREFIX)
            .replace("@OLLAMA_HOME@", OllamaConfig.GUEST_OLLAMA_HOME)
            .replace("@HOST@", OllamaConfig.ENV_HOST)
            .replace("@MODELS@", OllamaConfig.ENV_MODELS)
            .replace("@VERSION@", OllamaConfig.TAG)
            .replace("@ARTIFACT_MB@", (OllamaConfig.ARTIFACT_BYTES / 1_000_000).toString())
            .replace("@INSTALL_MB@", "60")
        target.writeText(body)
        runCatching { Os.chmod(target.absolutePath, 0x1ED) }   // 0755
        // Guarantee the shared ~/.ollama exists so the launcher can always write its state files.
        runCatching { OllamaConfig.hostOllamaHome(ctx).mkdirs() }
        runCatching { File(rootfs, "opt/ollama").mkdirs() }
        Log.i(tag, "[OLLAMA] launcher installed at ${OllamaConfig.GUEST_LAUNCHER} -> " +
            OllamaConfig.GUEST_BIN)
    }

    /** No rootfs: put an explicit "requires the Linux runtime/rootfs" command on the BusyBox PATH
     *  so `ollama` explains itself instead of failing with "not found". */
    private fun installBusyboxStub(ctx: Context, binDir: String) {
        File(binDir).mkdirs()
        val target = File(binDir, "ollama")
        ctx.assets.open("ollama/unavailable.sh").use { i ->
            target.outputStream().use { i.copyTo(it) }
        }
        runCatching { Os.chmod(target.absolutePath, 0x1ED) }   // 0755
        Log.i(tag, "[OLLAMA] busybox stub installed at $binDir/ollama (no rootfs)")
    }

    /**
     * Poll for the guest's install request. The guest cannot provision Ollama itself (the Kali nano
     * image ships neither `zstd` nor a guaranteed `curl`), so the app performs the download,
     * verification and extraction; the launcher only asks for it and tails the progress file.
     *
     * Process-singleton, idempotent, daemon, minimum priority. It does nothing at all until the
     * user explicitly confirms the download inside the terminal, so it cannot cause background
     * network use on its own.
     */
    private fun startWatcher(ctx: Context) {
        if (!watcherStarted.compareAndSet(false, true)) return
        val req = OllamaConfig.hostInstallRequest(ctx)
        Thread({
            while (true) {
                runCatching {
                    if (req.isFile) {
                        req.delete()
                        if (installing.compareAndSet(false, true)) {
                            worker.execute {
                                try { runInstall(ctx) } finally { installing.set(false) }
                            }
                        }
                    }
                }
                try { Thread.sleep(1000L) } catch (_: InterruptedException) { return@Thread }
            }
        }, "ollama-request-watcher").apply { isDaemon = true; priority = Thread.MIN_PRIORITY }.start()
        Log.i(tag, "[OLLAMA] request watcher armed at ${req.absolutePath}")
    }

    private fun runInstall(ctx: Context) {
        val log = OllamaConfig.hostInstallLog(ctx)
        runCatching { log.parentFile?.mkdirs() }
        fun line(s: String) { runCatching { log.appendText(s + "\n") } }

        line("=== drac-Xterm Ollama provisioning ===")
        line("release   : ${OllamaConfig.TAG} (pinned, official pre-release)")
        line("artifact  : ${OllamaConfig.ARTIFACT} (${OllamaConfig.ARTIFACT_BYTES} bytes)")
        line("sha256    : ${OllamaConfig.ARTIFACT_SHA256}")
        line("install   : ${OllamaConfig.GUEST_PREFIX}")

        when (val r = OllamaInstaller(ctx).install { phase, detail, pct ->
            line("[$phase]${if (pct in 0..100) " $pct%" else ""} $detail")
        }) {
            is OllamaInstaller.Result.Ok -> {
                line("kept entries    : ${r.keptEntries}")
                line("dropped entries : ${r.droppedEntries} (${r.droppedBytes} bytes)")
                line("dropped runners : ${r.droppedRunners.sorted().joinToString(", ")}")
                line("installed bytes : ${r.installedBytes}")
                line("RESULT: READY")
            }
            is OllamaInstaller.Result.Failed -> line("RESULT: FAILED, ${r.reason}")
        }
    }
}
