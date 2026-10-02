package com.dracxterm.claw

import android.content.Context
import android.util.Log
import com.dracxterm.Bootstrap
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Gets Scrapling ready inside the Linux guest so claw's browsing (the web_fetch tool and
 * `claw browse`) works the moment the user wants it, with no manual `claw browse --setup`.
 *
 * WHY THIS RUNS AT ALL, AND WHEN
 * ------------------------------------------------------------------------------------------------
 * Browsing needs python3 + a Scrapling virtualenv at /opt/claw/scrapling/.venv (~350 MB, and apt
 * for python3 needs root). None of that fits in the APK, so it is provisioned in the guest the
 * same way apt/dpkg recovery is: a POSIX script run as fake-root through the backend that runs the
 * rootfs (VHDP, or proot as the fallback). [attach] is called once per session spawn from
 * Bootstrap, right after the rootfs is prepared, so provisioning starts together with the Linux
 * setup and finishes in the background while the terminal stays fully usable.
 *
 * SAFE AND UNSURPRISING
 * ------------------------------------------------------------------------------------------------
 *  - It only downloads when browsing is NOT turned off in the claw policy (xset ▸ AI (claw)). If the
 *    user set Browsing = Mati, nothing is fetched. Default (ask/allow) means "prepare it".
 *  - Idempotent and marker-gated: once /opt/claw/scrapling/.ready exists (or a venv is already
 *    there), it does nothing. A failed or interrupted run leaves no marker, so it simply retries on
 *    the next launch when the network is back.
 *  - Best-effort and non-blocking: one background thread at minimum priority, a process-singleton,
 *    and every failure is swallowed and logged. It can never delay or block the terminal.
 *
 * The script it runs is a real, inspectable file in the guest
 * (/opt/claw/data/scripts/scrapling-setup.sh), installed with the rest of claw's data, and the user
 * can read it, run it by hand, or watch it: `tail -f /opt/claw/scrapling/setup.log`.
 */
object ScraplingProvisioner {

    private const val TAG = "claw-scrapling"
    private const val SCRIPT = "/opt/claw/data/scripts/scrapling-setup.sh"

    private val started = AtomicBoolean(false)
    private val worker = Executors.newSingleThreadExecutor { r ->
        Thread(r, "claw-scrapling-provisioner").apply { isDaemon = true; priority = Thread.MIN_PRIORITY }
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
            if (browsingDisabled(rootfs)) {
                Log.i(TAG, "[CLAW] browsing off in policy; skipping Scrapling setup")
                return
            }
            if (!File(rootfs, SCRIPT.removePrefix("/")).exists()) return  // claw data not installed yet
            if (!started.compareAndSet(false, true)) return
            worker.execute { provision(app, rootfs) }
        }.onFailure { Log.w(TAG, "[CLAW] Scrapling attach skipped: ${it.message}") }
    }

    /** Whether browsing has a working Scrapling to use. */
    fun isReady(rootfs: File): Boolean =
        File(rootfs, "opt/claw/scrapling/.ready").exists() ||
            File(rootfs, "opt/claw/scrapling/.venv/bin/python").exists()

    /** True only when the policy file explicitly turns browsing off; a missing/broken file means the
     *  safe default (ask), which does prepare Scrapling. */
    private fun browsingDisabled(rootfs: File): Boolean = runCatching {
        val f = File(rootfs, "opt/claw/data/policy.json")
        if (!f.isFile) return false
        val browse = org.json.JSONObject(f.readText()).optString("browse", "ask")
        browse.equals("off", ignoreCase = true)
    }.getOrDefault(false)

    private fun provision(ctx: Context, rootfs: File) {
        val hostLog = File(ctx.filesDir, "scrapling-setup.log")
        try {
            // Let the session settle (dpkg recovery at boot releases the apt lock first).
            try { Thread.sleep(3000) } catch (_: InterruptedException) { return }
            if (isReady(rootfs)) return

            Log.i(TAG, "[CLAW] preparing Scrapling for browsing (fake-root, background)")
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
                Log.i(TAG, "[CLAW] Scrapling ready; browsing is now usable")
            } else {
                Log.w(TAG, "[CLAW] Scrapling setup did not finish (exit=$code); will retry next launch. " +
                    "See ${hostLog.absolutePath} and /opt/claw/scrapling/setup.log")
            }
        } catch (t: Throwable) {
            Log.w(TAG, "[CLAW] Scrapling setup error: ${t.message}")
        } finally {
            // Allow another attempt on a later spawn if this one did not reach ready.
            if (!isReady(rootfs)) started.set(false)
        }
    }
}
