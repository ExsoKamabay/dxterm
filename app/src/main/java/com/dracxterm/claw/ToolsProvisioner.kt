package com.dracxterm.claw

import android.content.Context
import android.util.Log
import com.dracxterm.Bootstrap
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Extracts the BUNDLED Python modules (scapy + PyRIT) shipped inside the APK so the `networking`
 * (scapy) and `ai-red-team` (PyRIT) skills work — with NO network, apt, or pip at boot.
 *
 * WHY THIS CHANGED (root cause of the apt-lock bug)
 * ------------------------------------------------------------------------------------------------
 * The previous version ran `apt-get install` (python3, and for PyRIT a whole Rust toolchain) plus
 * `pip install` in the background. That held the global dpkg lock for a long time and, because the
 * heavy PyRIT build rarely finished, it re-ran every boot — so a user's `sudo apt install <pkg>`
 * would sit forever on "Could not get lock /var/lib/dpkg/lock-frontend … held by process N
 * (apt-get)". The modules are now bundled in the APK (assets/claw/data/pytools.tar) and this
 * provisioner only EXTRACTS them (tar) into /opt/claw/pytools. Tar extraction never touches the
 * dpkg lock, so it can never block the user's apt/dpkg again.
 *
 * WHEN & HOW
 * ------------------------------------------------------------------------------------------------
 * [attach] is called once per session spawn from Bootstrap, right after the rootfs is prepared, and
 * runs the guest script tools-setup.sh as fake-root through the backend (VHDP, or proot). scapy is
 * usable immediately from the bundle via PYTHONPATH=/opt/claw/pytools (pure Python, no deps). PyRIT
 * source is extracted too; its heavy runtime deps are installed on demand by the ai-red-team skill,
 * never at boot.
 *
 * SAFE AND UNSURPRISING
 * ------------------------------------------------------------------------------------------------
 *  - Only when skills are NOT turned off in the claw policy (xset ▸ AI (claw)).
 *  - Idempotent and version-gated: once /opt/claw/pytools/.ready exists AND .bundle-version matches
 *    the APK's BUNDLE_VERSION it does nothing; a bumped version re-extracts the refreshed modules.
 *  - Best-effort, one background thread at minimum priority, a process-singleton, failures swallowed
 *    and logged. It can never delay or block the terminal. Watch it: `tail -f /opt/claw/tools/setup.log`.
 */
object ToolsProvisioner {

    private const val TAG = "claw-tools"
    private const val SCRIPT = "/opt/claw/data/scripts/tools-setup.sh"

    /** Bundle content version — MUST match VERSION in tools-setup.sh. Bump on any pytools.tar change
     *  so installs with an older extracted tree re-extract the new modules instead of keeping the old
     *  ones. v2: scapy patched so `import scapy.all` survives a denied RTNETLINK socket in the guest. */
    private const val BUNDLE_VERSION = "2"

    private val started = AtomicBoolean(false)
    private val worker = Executors.newSingleThreadExecutor { r ->
        Thread(r, "claw-tools-provisioner").apply { isDaemon = true; priority = Thread.MIN_PRIORITY }
    }

    /**
     * Arm the background provisioner. [rootfs] is the extracted Linux tree, or null on the BusyBox
     * path (nothing to do without a guest). Called once per spawn; the process-singleton guard makes
     * repeat calls cheap no-ops.
     */
    fun attach(ctx: Context, rootfs: File?) {
        if (rootfs == null) return
        val app = ctx.applicationContext
        runCatching {
            if (isReady(rootfs)) return
            if (skillsDisabled(rootfs)) {
                Log.i(TAG, "[CLAW] skills off in policy; skipping tools setup")
                return
            }
            if (!File(rootfs, SCRIPT.removePrefix("/")).exists()) return  // claw data not installed yet
            if (!started.compareAndSet(false, true)) return
            worker.execute { provision(app, rootfs) }
        }.onFailure { Log.w(TAG, "[CLAW] tools attach skipped: ${it.message}") }
    }

    /** The bundled modules have been extracted to /opt/claw/pytools AND the extracted tree matches
     *  the version shipped in this APK. A missing/older .bundle-version means an older extraction, so
     *  the provisioner re-runs tools-setup.sh to refresh the modules. */
    fun isReady(rootfs: File): Boolean {
        if (!File(rootfs, "opt/claw/pytools/.ready").exists()) return false
        val ver = runCatching { File(rootfs, "opt/claw/pytools/.bundle-version").readText().trim() }
            .getOrDefault("")
        return ver == BUNDLE_VERSION
    }

    /** scapy is present in the extracted bundle — the `networking` skill is usable. */
    fun networkingReady(rootfs: File): Boolean =
        File(rootfs, "opt/claw/pytools/scapy/__init__.py").exists()

    /** True only when the policy file explicitly turns skills off; a missing/broken file means the
     *  safe default (skills on), which does prepare the tools. */
    private fun skillsDisabled(rootfs: File): Boolean = runCatching {
        val f = File(rootfs, "opt/claw/data/policy.json")
        if (!f.isFile) return false
        val o = org.json.JSONObject(f.readText())
        o.has("skills") && !o.optBoolean("skills", true)
    }.getOrDefault(false)

    private fun provision(ctx: Context, rootfs: File) {
        val hostLog = File(ctx.filesDir, "tools-setup.log")
        try {
            // Let the session settle; the extraction itself is quick and touches no package manager.
            try { Thread.sleep(3000) } catch (_: InterruptedException) { return }
            if (isReady(rootfs)) return

            Log.i(TAG, "[CLAW] extracting bundled scapy/PyRIT modules (fake-root, background)")
            val (argv, env) = Bootstrap.fakerootShell(ctx, rootfs.absolutePath, "sh $SCRIPT")
            val pb = ProcessBuilder(argv.toList()).redirectErrorStream(true)
            pb.environment().apply {
                clear()
                env.forEach { kv -> val i = kv.indexOf('='); if (i > 0) put(kv.substring(0, i), kv.substring(i + 1)) }
            }
            val proc = pb.start()
            proc.inputStream.use { input -> hostLog.outputStream().use { input.copyTo(it) } }
            val code = proc.waitFor()
            if (isReady(rootfs)) {
                Log.i(TAG, "[CLAW] bundled modules extracted; scapy ready (PYTHONPATH=/opt/claw/pytools), PyRIT source available")
            } else {
                Log.w(TAG, "[CLAW] module extraction did not finish (exit=$code); will retry next launch. " +
                    "See ${hostLog.absolutePath} and /opt/claw/tools/setup.log")
            }
        } catch (t: Throwable) {
            Log.w(TAG, "[CLAW] module extraction error: ${t.message}")
        } finally {
            // Allow another attempt on a later spawn until extraction succeeds.
            if (!isReady(rootfs)) started.set(false)
        }
    }
}
