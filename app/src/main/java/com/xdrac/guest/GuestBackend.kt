package com.xdrac.guest

import android.content.Context
import android.util.Log
import com.xdrac.Bootstrap
import com.xdrac.rootfs.DpkgRecoveryEngine
import com.xdrac.rootfs.RootfsConfigurator
import com.xdrac.rootfs.ShellLocator
import com.xdrac.vhdp.Vhdp
import org.json.JSONObject
import java.io.File

/**
 * The single front door for running the Linux guest.
 *
 * VHDP owns the Linux environment: it configures the rootfs, decides the dpkg repair plan and
 * the system projection, and -- through its rootless engine and userland ELF loader -- runs the
 * guest itself. An Android app may not execve() files from its writable storage, where every
 * guest program lives; VHDP's engine therefore starts guest programs by exec'ing
 * libvhdp-loader.so from nativeLibraryDir and mapping the guest ELF from inside that process.
 *
 * proot stays available as the fallback execution backend. It is chosen when VHDP's self-test on
 * this device and rootfs fails ([selectBackend]), so a device where VHDP cannot run still gets a
 * working terminal. proot is a SEPARATELY EXEC'd binary (in jniLibs), never linked into libvhdp:
 * proot is GPL-2 and VHDP is Apache-2.0, so keeping it an invoked tool keeps the licences clean.
 */
object GuestBackend {

    /**
     * Configure a freshly extracted rootfs for first boot.
     *
     * Primary path: VHDP native ([Vhdp.configureRootfs] -> libvhdp). Falls back to the legacy
     * in-app Kotlin configurator only when VHDP is unavailable or does not report success, so
     * provisioning can never regress while ownership moves into the library. Returns the same
     * [RootfsConfigurator.Result] the boot pipeline already understands.
     */
    fun configureRootfs(rootfs: File): RootfsConfigurator.Result {
        val r = runCatching { Vhdp.configureRootfs(rootfs.absolutePath) }.getOrNull()
        val ok = r != null && r.ok &&
            runCatching { JSONObject(r.json).optString("status") == "ok" }.getOrDefault(false)
        if (ok) {
            Log.i(ShellLocator.TAG, "[GUEST] rootfs configured by VHDP: ${r!!.json}")
            return RootfsConfigurator.Result.Ok
        }
        val why = if (r == null) "native call threw" else "${Vhdp.statusName(r.status)} json=${r.json}"
        Log.w(ShellLocator.TAG, "[GUEST] VHDP configure did not succeed ($why); using in-app fallback")
        return RootfsConfigurator().configure(rootfs)
    }

    /**
     * Repair dpkg/apt state after a fake-root install (feature 2), orchestrated by VHDP.
     *
     * VHDP decides the plan natively ([Vhdp.dpkgPlan]: inspects the package-manager state and
     * supplies the recovery script); the script is then executed as a fake-root pass through the
     * backend this rootfs runs on ([DpkgRecoveryEngine.runScript] -> [Bootstrap.fakerootShell]: VHDP,
     * or proot as the fallback). Marker-gated (runs once per install) and never fatal. Falls back to the self-contained in-app engine when VHDP's plan is unavailable.
     */
    fun recoverDpkg(ctx: Context, rootfs: File): Boolean {
        if (!File(rootfs, "var/lib/dpkg").isDirectory) return true       // not a dpkg distro
        if (DpkgRecoveryEngine.alreadyRecovered(ctx)) return true        // already repaired once

        val plan = runCatching { Vhdp.dpkgPlan(rootfs.absolutePath) }.getOrNull()
        val json = plan?.takeIf { it.ok }?.let { runCatching { JSONObject(it.json) }.getOrNull() }
        if (json == null) {
            Log.w(ShellLocator.TAG, "[GUEST] VHDP dpkg plan unavailable; using in-app fallback")
            return DpkgRecoveryEngine.run(ctx, rootfs)
        }
        if (!json.optBoolean("needs_recovery", false)) {
            Log.i(ShellLocator.TAG, "[GUEST] VHDP: no dpkg recovery needed")
            DpkgRecoveryEngine.markRecovered(ctx)
            return true
        }
        val script = json.optString("script")
        if (script.isBlank()) return DpkgRecoveryEngine.run(ctx, rootfs)

        Log.i(ShellLocator.TAG, "[GUEST] dpkg recovery: VHDP plan -> ${chosenBackend(ctx, rootfs)} backend (locks=${json.optJSONArray("locks")})")
        val ok = DpkgRecoveryEngine.runScript(ctx, rootfs, script)       // exec via the chosen backend
        if (ok) DpkgRecoveryEngine.markRecovered(ctx)
        return ok
    }

    /** The device/system trees always projected into the guest, if VHDP has nothing better to say. */
    private val DEFAULT_SYSTEM_BINDS = listOf("/dev" to "/dev", "/proc" to "/proc", "/sys" to "/sys")

    /**
     * The guest's system/device/arch projection (feature 4), decided by VHDP.
     *
     * VHDP inspects the host and returns which real trees (/dev, /proc, /sys) to graft into the
     * guest plus the host arch/kernel/page-size it read from Android; the backend then applies them
     * ([Bootstrap.vhdpArgv] projects /dev and /proc natively and binds the rest; [Bootstrap.prootArgv]
     * uses -b binds). This is the seam where VHDP can later drop or
     * substitute a tree Android restricts. Falls back to the default trio when VHDP is unavailable,
     * and never returns empty, so the terminal always has its device/proc/sys mountpoints.
     */
    fun systemBinds(): List<Pair<String, String>> {
        val plan = runCatching { Vhdp.projectionPlan() }.getOrNull()
        val json = plan?.takeIf { it.ok }?.let { runCatching { JSONObject(it.json) }.getOrNull() }
        if (json == null) {
            Log.w(ShellLocator.TAG, "[GUEST] VHDP projection plan unavailable; default system binds")
            return DEFAULT_SYSTEM_BINDS
        }
        val arr = json.optJSONArray("system_binds")
        val out = ArrayList<Pair<String, String>>()
        val dropped = ArrayList<String>()
        if (arr != null) for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            if (o.optBoolean("available", true)) {
                val h = o.optString("host"); val g = o.optString("guest")
                if (h.isNotBlank() && g.isNotBlank()) out += h to g
            } else {
                dropped += "${o.optString("host")} (${o.optString("unavailable_because", "no reason given")})"
            }
        }
        // Logged loudly: a tree the guest does not get is the kind of thing that surfaces much
        // later as an unrelated-looking failure -- no /dev means no /dev/null, /dev/urandom or
        // /dev/tty, and writes to /dev/null land in a real file inside the rootfs instead.
        if (dropped.isNotEmpty()) {
            Log.w(ShellLocator.TAG, "[GUEST] system projection DROPPED: ${dropped.joinToString("; ")}")
        }
        if (out.isEmpty()) return DEFAULT_SYSTEM_BINDS   // never launch without device/proc/sys
        val host = json.optJSONObject("host")
        Log.i(ShellLocator.TAG, "[GUEST] system projection (VHDP): ${out.joinToString { it.second }}" +
            (host?.let { " host{arch=${it.optString("arch")} kernel=${it.optString("kernel")} page=${it.optLong("page_size")}}" } ?: ""))
        return out
    }

    enum class Backend { VHDP, PROOT }

    private const val BACKEND_MARKER = ".guest-backend"
    private const val SELFTEST_TOKEN = "vhdp-selftest-ok"
    private const val SELFTEST_TIMEOUT_MS = 20_000L

    /** Result of the last backend decision: what runs the guest, and why. */
    data class Decision(val backend: Backend, val detail: String)

    /**
     * Decides which backend runs [rootfs], by running VHDP for real: a fake-root shell through
     * libphdp.so and the userland loader, checking the emulated identity and a projected device.
     * The answer is cached against nativeLibraryDir (which an app update renames) and the rootfs
     * path, so the self-test runs once per install or update, not per session.
     *
     * Blocking (spawns a short-lived process): call it from a background thread. BootManager does,
     * on every boot, before the terminal opens; the session path only reads the cached answer
     * through [chosenBackend].
     */
    fun selectBackend(ctx: Context, rootfs: File): Decision {
        cached(ctx, rootfs)?.let { return it }
        val d = runCatching { selfTest(ctx, rootfs) }
            .getOrElse { Decision(Backend.PROOT, "self-test threw: ${it.message}") }
        runCatching {
            File(ctx.filesDir, BACKEND_MARKER).writeText("${stamp(ctx, rootfs)}\n${d.backend}\n${d.detail}\n")
        }
        Log.i(ShellLocator.TAG, "[GUEST] backend for ${rootfs.absolutePath}: ${d.backend} (${d.detail})")
        return d
    }

    /** The backend [selectBackend] chose, or PROOT when nothing was decided yet. Never spawns. */
    fun chosenBackend(ctx: Context, rootfs: File): Backend = cached(ctx, rootfs)?.backend ?: Backend.PROOT

    /** The cached decision including its reason, for diagnostics; null when not decided yet. */
    fun decision(ctx: Context, rootfs: File): Decision? = cached(ctx, rootfs)

    private fun stamp(ctx: Context, rootfs: File) = "${ctx.applicationInfo.nativeLibraryDir}|${rootfs.absolutePath}"

    private fun cached(ctx: Context, rootfs: File): Decision? {
        val lines = runCatching { File(ctx.filesDir, BACKEND_MARKER).readLines() }.getOrNull() ?: return null
        if (lines.size < 2 || lines[0] != stamp(ctx, rootfs)) return null
        val backend = runCatching { Backend.valueOf(lines[1]) }.getOrNull() ?: return null
        return Decision(backend, lines.getOrElse(2) { "" })
    }

    private fun selfTest(ctx: Context, rootfs: File): Decision {
        val nativeLib = ctx.applicationInfo.nativeLibraryDir
        val phdp = File(nativeLib, "libphdp.so")
        val loader = File(nativeLib, "libvhdp-loader.so")
        if (!phdp.isFile) return Decision(Backend.PROOT, "libphdp.so is not installed")
        if (!loader.isFile) return Decision(Backend.PROOT, "libvhdp-loader.so is not installed")
        if (ShellLocator.find(rootfs) == null) return Decision(Backend.PROOT, "rootfs has no shell")
        Bootstrap.ensureGuestDev(rootfs)

        val tmp = File(ctx.filesDir, "tmp").apply { mkdirs() }
        // /bin/sh is a dynamic ELF in every supported distro, so this exercises the loader, the
        // guest's own ld.so, fake-root identity and the minimal /dev projection together.
        val probe = "test \"$(id -u)\" = 0 && test -c /dev/null && echo x > /dev/null && echo $SELFTEST_TOKEN"
        val pb = ProcessBuilder(
            phdp.absolutePath, "run", "--engine", "rootless", "--json", "--verbose",
            "--uid", "0", "--gid", "0", "--proc", "host", "--dev", "minimal",
            "--cwd", "/", "--clear-env", "-e", "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
            rootfs.absolutePath, "--", "/bin/sh", "-c", probe
        ).redirectErrorStream(true)
        pb.environment().apply {
            clear()
            put("TMPDIR", tmp.absolutePath); put("LANG", "C.UTF-8"); put("PATH", "$nativeLib:/system/bin")
        }
        val proc = pb.start()
        val out = StringBuilder()
        val reader = Thread { runCatching { proc.inputStream.bufferedReader().forEachLine { out.appendLine(it) } } }
        reader.start()
        val finished = proc.waitFor(SELFTEST_TIMEOUT_MS, java.util.concurrent.TimeUnit.MILLISECONDS)
        if (!finished) {
            proc.destroyForcibly()
            return Decision(Backend.PROOT, "VHDP self-test timed out after ${SELFTEST_TIMEOUT_MS} ms")
        }
        reader.join(2_000)
        val code = proc.exitValue()
        val text = out.toString().trim()
        return if (code == 0 && text.lines().any { it.trim() == SELFTEST_TOKEN }) {
            Decision(Backend.VHDP, "self-test passed (rootless engine + userland loader)")
        } else {
            // The whole transcript (phdp's JSON events included) goes to logcat, so a device where
            // VHDP cannot run says why; the marker keeps a one-line summary.
            text.lines().forEach { Log.w(ShellLocator.TAG, "[GUEST] vhdp self-test: $it") }
            val why = text.lines().lastOrNull { it.contains("\"severity\":\"error\"") || it.startsWith("phdp:") }
                ?: text.lines().lastOrNull().orEmpty()
            Decision(Backend.PROOT, "VHDP self-test failed, exit=$code: ${why.take(300)}")
        }
    }

    /**
     * The argv that launches the guest shell: VHDP's rootless engine when [selectBackend] found it
     * working for this rootfs, otherwise proot. The terminal and session code above never needs
     * to know which one runs the guest.
     */
    fun launchArgv(ctx: Context, rootfs: String, guestEnv: Array<String>): Array<String> =
        when (chosenBackend(ctx, File(rootfs))) {
            Backend.VHDP -> Bootstrap.vhdpArgv(ctx, rootfs, guestEnv)
            Backend.PROOT -> Bootstrap.prootArgv(ctx, rootfs)
        }
}
