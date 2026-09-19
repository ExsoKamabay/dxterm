package com.xdrac.rootfs

import android.content.Context
import android.util.Log
import com.xdrac.Bootstrap
import java.io.File

/**
 * Advanced Compatibility & Recovery Engine for apt / dpkg inside the guest, no Android root.
 *
 * Root cause it repairs: with a non-root interactive login (dracos), package-manager state under
 * /var/lib/dpkg ends up owned by dracos(1000) instead of root(0). In a fake-root guest (vhdp
 * --uid 0 or proot -0) a sudo/su-acquired root does not inherit the launch-time fake-root DAC
 * bypass, so dpkg, even via sudo, cannot write a dracos-owned directory ("error creating new
 * backup file '/var/lib/dpkg/status-old': Permission denied"), and the transaction is left
 * interrupted.
 *
 * Strategy (all inside a single fake-root pass, the only context where chown succeeds here):
 *   1. Normalise ownership of the SYSTEM tree to root:root (user homes are left untouched), so
 *      that apt/dpkg run through `sudo` (guest uid 0) own their files and can write them.
 *   2. Clear stale package-manager locks from the interrupted run.
 *   3. Verify the dpkg status DB; restore it from status-old / a backup when empty or corrupt.
 *   4. Take a rollback snapshot, finish the interrupted transaction (`dpkg --configure -a`), and
 *      roll the status DB back if that fails.
 *
 * It is idempotent, gated by a version marker so the (one-off, heavier) ownership pass runs once,
 * logs to filesDir/dpkg-recovery.log, and never throws, a recovery failure must not block boot.
 */
object DpkgRecoveryEngine {

    // v2: forces one clean re-run on existing installs. v1 could be written even when
    // `dpkg --configure -a` had actually failed (the script always exited 0), so a device that
    // hit the pre-link2symlink backup-link error is stuck "repaired" with an interrupted dpkg.
    // Bumping the marker + making the script exit truthfully (below) heals it once, now that
    // link2symlink lets the backup link() succeed.
    private const val MARKER = ".dpkg-recovery.v2"

    /** The one-off recovery has already run successfully on this install. */
    fun alreadyRecovered(ctx: Context): Boolean = File(ctx.filesDir, MARKER).exists()

    /** Record that recovery completed, so the (heavier) pass runs once per install. */
    fun markRecovered(ctx: Context) {
        runCatching { File(ctx.filesDir, MARKER).createNewFile() }
    }

    /**
     * Execute a recovery [script] inside the guest as fake-root, through the backend the rootfs runs
     * on ([Bootstrap.fakerootShell]: VHDP, or proot as the fallback). VHDP (via GuestBackend) decides
     * the script; this runs it. Returns true on exit 0. Never throws; a failure is logged and returned
     * so the caller withholds the success marker and retries next boot. Does NOT touch the marker.
     */
    fun runScript(ctx: Context, rootfs: File, script: String): Boolean {
        val log = File(ctx.filesDir, "dpkg-recovery.log")
        return try {
            val (argv, env) = Bootstrap.fakerootShell(ctx, rootfs.absolutePath, script)
            Log.i(ShellLocator.TAG, "[RECOVERY] starting dpkg/apt recovery (fake-root)")
            val pb = ProcessBuilder(argv.toList()).redirectErrorStream(true)
            pb.environment().apply {
                clear()
                env.forEach { kv -> val i = kv.indexOf('='); if (i > 0) put(kv.substring(0, i), kv.substring(i + 1)) }
            }
            val proc = pb.start()
            proc.inputStream.use { input -> log.outputStream().use { input.copyTo(it) } }
            val code = proc.waitFor()
            Log.i(ShellLocator.TAG, "[RECOVERY] finished, exit=$code (see ${log.absolutePath})")
            code == 0
        } catch (t: Throwable) {
            Log.w(ShellLocator.TAG, "[RECOVERY] skipped: ${t.message}")
            false
        }
    }

    /**
     * Self-contained fallback used when VHDP's plan is unavailable: marker-gated, runs the built-in
     * [RECOVERY_SH] through whichever backend runs the rootfs. The primary path is
     * [com.xdrac.guest.GuestBackend.recoverDpkg], which asks VHDP to decide the plan/script and
     * calls [runScript] here to execute it.
     */
    fun run(ctx: Context, rootfs: File): Boolean {
        if (!File(rootfs, "var/lib/dpkg").isDirectory) return true   // not a dpkg distro
        if (alreadyRecovered(ctx)) return true                       // already repaired once
        val ok = runScript(ctx, rootfs, RECOVERY_SH)
        if (ok) markRecovered(ctx)
        return ok
    }

    // POSIX sh, executed as fake-root (uid 0) inside the guest. Defensive: every step tolerates
    // absent files and never aborts the whole script on a single failure.
    private val RECOVERY_SH = """
        set -u
        log() { echo "[drac-recovery] ${'$'}*"; }
        log "start"

        # 1) Ownership: the system tree must be root-owned so apt/dpkg (root via sudo) can write it.
        #    User homes are intentionally skipped so the non-root session keeps owning its files.
        for d in /etc /usr /var /bin /sbin /lib /lib32 /lib64 /libx32 /opt /run /srv /boot; do
            [ -e "${'$'}d" ] && chown -R 0:0 "${'$'}d" 2>/dev/null || true
        done

        # 2) Clear stale package-manager locks left by the interrupted run.
        rm -f /var/lib/dpkg/lock /var/lib/dpkg/lock-frontend \
              /var/lib/apt/lists/lock /var/cache/apt/archives/lock 2>/dev/null || true

        # 3) dpkg status DB integrity: restore from a good copy when empty/corrupt.
        DB=/var/lib/dpkg
        S="${'$'}DB/status"
        if [ ! -s "${'$'}S" ]; then
            log "status missing/empty; attempting restore"
            for b in "${'$'}DB/status-old" /var/backups/dpkg.status.0 "${'$'}DB/status-new"; do
                if [ -s "${'$'}b" ]; then cp -a "${'$'}b" "${'$'}S" && log "restored from ${'$'}b" && break; fi
            done
        fi
        [ -s "${'$'}S" ] && cp -a "${'$'}S" "${'$'}DB/status.drac-bak" 2>/dev/null || true   # rollback point

        # 4) Finish the interrupted transaction; roll the status DB back if it fails.
        #    rc propagates the REAL outcome so the caller only writes its success marker when
        #    dpkg actually reconfigured cleanly (otherwise recovery retries on the next boot).
        rc=0
        if command -v dpkg >/dev/null 2>&1; then
            if dpkg --configure -a; then
                log "dpkg --configure -a OK"
            else
                log "dpkg --configure -a FAILED -> rolling back status"
                [ -s "${'$'}DB/status.drac-bak" ] && cp -a "${'$'}DB/status.drac-bak" "${'$'}S"
                rc=1
            fi
        fi
        log "done (rc=${'$'}rc)"
        exit "${'$'}rc"
    """.trimIndent()
}
