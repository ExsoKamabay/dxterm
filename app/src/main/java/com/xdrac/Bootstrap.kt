package com.xdrac

import android.content.Context
import android.net.NetworkCapabilities
import android.net.ConnectivityManager
import android.os.Build
import android.os.Environment
import android.system.Os
import android.util.Log
import com.xdrac.ollama.OllamaLauncher
import com.xdrac.rootfs.RootfsConfigurator
import com.xdrac.rootfs.ShellLocator
import java.io.File

/**
 * Prepares the runtime environment for the shell.
 *
 * Three constraints shape everything here. The prebuilt binaries are arm64-v8a only and
 * live in nativeLibraryDir as lib*.so, which is the only naming Android extracts as real
 * files and therefore the only way they can be exec()'d from API 29. libproot.so has
 * DT_NEEDED "libtalloc.so.2" while the packager can only ship libtalloc.so, so the
 * libtalloc.so.2 symlink is made at every launch: an app update renames the install
 * directory, and a link made before the update points at a path that is gone. And a
 * rootfs may or may not exist: the default command is `busybox ash`, and prepare()
 * switches to proot on its own once one appears at filesDir/rootfs.
 */
object Bootstrap {

    private const val TAG = "Bootstrap"
    private const val MARKER = ".bootstrap_v1"

    data class Spec(val argv: Array<String>, val env: Array<String>, val cwd: String)

    fun prepare(ctx: Context): Spec {
        val nativeLib = ctx.applicationInfo.nativeLibraryDir
        val files = ctx.filesDir.absolutePath
        val home = "$files/home"
        val tmp = "$files/tmp"
        val usrLib = "$files/usr/lib"
        val usrBin = "$files/usr/bin"

        listOf(home, tmp, usrLib, usrBin).forEach { File(it).mkdirs() }

        val busybox = "$nativeLib/libbusybox.so"

        // Recreate libtalloc.so.2 -> nativeLib/libtalloc.so (proot's DT_NEEDED).
        symlink("$nativeLib/libtalloc.so", "$usrLib/libtalloc.so.2")

        // The applet links in usr/bin point at libbusybox.so inside nativeLibraryDir, and
        // an app update renames that directory. A marker that only answers "has this ever
        // run" is asking the wrong question: it says yes while every link it created
        // dangles. The marker records where the links point, so an update invalidates it.
        //
        // Not simply rerun every launch: busybox --list forks a process and there are
        // several hundred links to make, and this is a terminal, which should open at once.
        val marker = File(files, MARKER)
        val stamp = nativeLib
        // exists() follows the link, so a dangling one reads as absent, which is the
        // answer wanted here: if the shell this session is about to run is not reachable,
        // rebuild the links whatever the marker claims.
        if (runCatching { marker.readText() }.getOrNull() != stamp ||
            !File(usrBin, "ash").exists()
        ) {
            installBusyboxApplets(busybox, usrBin)
            runCatching { marker.writeText(stamp) }
        }
        // Gated on the file, not the marker: recreating a shell rc the user deleted is
        // right, and rewriting one they edited is not.
        if (!File(home, ".ashrc").exists()) writeAshrc(home)
        // `xset` guest command (refreshed every launch so upgrades land) for the no-rootfs busybox path.
        installXsetCommand(ctx, usrBin)

        // Runtime selection:
        //  - If the user has installed a Linux rootfs at filesDir/rootfs, launch it
        //    through proot (which dlopens libtalloc + libandroid-shmem via LD_LIBRARY_PATH).
        //  - Otherwise fall back to busybox ash, which works with no rootfs.
        val rootfs = "$files/rootfs"
        val hasGuest = hasRootfs(rootfs)

        // `ollama` guest command. Isolated add-on: OllamaLauncher installs the launcher onto
        // whichever PATH this session will actually use (rootfs /usr/local/bin, or the busybox
        // usr/bin stub that explains the Linux-runtime requirement) and arms the on-demand
        // provisioning watcher. Refreshed every launch, exactly like installXsetCommand above.
        // Never fatal: OllamaLauncher.attach swallows and logs every failure, so a problem here
        // can never delay or block the terminal.
        OllamaLauncher.attach(ctx, if (hasGuest) File(rootfs) else null, usrBin)

        // Proot's own runtime lookups (read on the HOST side, before re-root).
        val prootEnv = arrayOf(
            "LD_LIBRARY_PATH=$nativeLib:$usrLib",
            "PROOT_LOADER=$nativeLib/libproot-loader.so",
            "PROOT_TMP_DIR=$tmp"
        )

        // Guest login shell (real interpreter inside the rootfs), used for SHELL so the sudo/su
        // shims' `${SHELL:-…}` and `su -c` paths resolve to a shell the image actually ships.
        val guestShell = if (hasGuest) (ShellLocator.guestPath(File(rootfs)) ?: "/bin/sh") else "/bin/sh"

        // Guest environment. The login identity is the ordinary user 'dracos' with a '$' prompt,
        // but the guest runs as fake-root, so the (emulated) euid is 0 whatever the shell says.
        // The dracos/root distinction is presentation, driven by $DRAC_SU: the prompt in
        // ~/.bashrc and the whoami/id/logname shims on PATH both read it. DRAC_HOME pins the
        // themed rc so a super-user shell loads it even with HOME=/root.
        val guestEnv = arrayOf(
            "HOME=/home/dracos",
            "PWD=/home/dracos",
            "USER=dracos",
            "LOGNAME=dracos",
            "DRAC_HOME=/home/dracos",
            "SHELL=$guestShell",
            "TMPDIR=/tmp",
            "TERM=xterm-256color",
            "LANG=C.UTF-8",
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
        )
        val backend = if (hasGuest) com.xdrac.guest.GuestBackend.chosenBackend(ctx, File(rootfs)) else null

        val env = when {
            // phdp is an Android-side process: it gets a host environment, and hands the guest
            // exactly guestEnv (--clear-env + -e), so nothing of the app's environment leaks in.
            backend == com.xdrac.guest.GuestBackend.Backend.VHDP -> arrayOf(
                "HOME=$home",
                "PWD=$home",
                "TMPDIR=$tmp",
                "TERM=xterm-256color",
                "LANG=C.UTF-8",
                "PATH=$nativeLib:/system/bin"
            )
            // proot passes its own environment through to the guest.
            hasGuest -> guestEnv + prootEnv
            // BusyBox-over-Android environment (no rootfs): Android paths are correct. There is no
            // real /home/dracos on a non-rooted device, so HOME stays app-private; the identity is
            // still presented as 'dracos' (USER + a whoami function seeded into .ashrc) and the
            // prompt ends with '$' to match the normal-user contract of the Linux path.
            else -> arrayOf(
                "HOME=$home",
                "PWD=$home",
                "USER=dracos",
                "LOGNAME=dracos",
                "TMPDIR=$tmp",
                "TERM=xterm-256color",
                "LANG=C.UTF-8",
                "PATH=$usrBin:$nativeLib:/system/bin",
                "ENV=$home/.ashrc"
            ) + prootEnv
        }

        // Fresh every session start, so `ifconfig` never shows a network the device left behind.
        writeNetSnapshot(ctx)

        val argv = if (hasGuest) {
            Log.i(TAG, "rootfs found at $rootfs -> launching via guest backend $backend")
            // Through the guest front door, which picks VHDP or proot for this rootfs.
            com.xdrac.guest.GuestBackend.launchArgv(ctx, rootfs, guestEnv)
        } else {
            // Through the link, not the binary: argv[0] has to read "ash" for BusyBox to
            // dispatch to the shell. Handing it the packaged path gets "applet not found",
            // the shell exits at once, and the terminal closes itself on launch.
            arrayOf("$usrBin/ash")
        }
        // The host-side cwd handed to execve is always a real host directory ($HOME);
        // the backend re-roots the guest itself (vhdp --cwd, proot), so this stays valid
        // in both modes.
        return Spec(argv, env, home)
    }

    /**
     * Build a FAKE-ROOT one-shot invocation that runs `/bin/sh -c <script>` inside the rootfs, for
     * bootstrap-time maintenance such as the dpkg recovery engine, through whichever backend runs
     * this rootfs (VHDP with --uid 0, or proot -0). Fake-root is the only context in which
     * chown/permission repair inside the guest succeeds without Android root.
     * Returns (argv, env) ready for ProcessBuilder.
     */
    fun fakerootShell(ctx: Context, rootfs: String, script: String): Pair<Array<String>, Array<String>> {
        val nativeLib = ctx.applicationInfo.nativeLibraryDir
        val files = ctx.filesDir.absolutePath
        val tmp = "$files/tmp"; val usrLib = "$files/usr/lib"
        listOf(tmp, usrLib).forEach { File(it).mkdirs() }
        val guestPath = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

        if (com.xdrac.guest.GuestBackend.chosenBackend(ctx, File(rootfs)) == com.xdrac.guest.GuestBackend.Backend.VHDP) {
            ensureGuestDev(File(rootfs))
            val env = arrayOf("HOME=$files/home", "TMPDIR=$tmp", "LANG=C.UTF-8", "PATH=$nativeLib:/system/bin")
            val argv = arrayOf(
                *phdpEventsToLog(ctx), "$nativeLib/libphdp.so", "run", "--event-fd", "3",
                "--engine", "rootless",
                "--uid", "0", "--gid", "0", "--proc", "host", "--dev", "minimal",
                "--bind", "/sys:/sys:rw", "--cwd", "/", "--clear-env",
                "-e", "HOME=/root", "-e", "PWD=/root", "-e", "TMPDIR=/tmp",
                "-e", "TERM=xterm-256color", "-e", "LANG=C.UTF-8", "-e", guestPath,
                rootfs, "--", "/bin/sh", "-c", script
            )
            return argv to env
        }

        symlink("$nativeLib/libtalloc.so", "$usrLib/libtalloc.so.2")   // proot DT_NEEDED
        val env = arrayOf(
            "HOME=/root", "PWD=/root", "TMPDIR=/tmp",
            "TERM=xterm-256color", "LANG=C.UTF-8",
            guestPath,
            "LD_LIBRARY_PATH=$nativeLib:$usrLib",
            "PROOT_LOADER=$nativeLib/libproot-loader.so",
            "PROOT_TMP_DIR=$tmp"
        )
        val argv = arrayOf(
            "$nativeLib/libproot.so", "-0", "--link2symlink", "-r", rootfs,
            "-b", "/dev", "-b", "/proc", "-b", "/sys",
            "-w", "/", "/bin/sh", "-c", script
        )
        return argv to env
    }

    /**
     * True when [rootfs] provides a login shell. Uses ShellLocator (Os.lstat based, wide
     * candidate set) so symlinked shells such as Alpine /bin/sh -> /bin/busybox are detected
     * correctly instead of being missed by File.exists() (which follows the link to the host).
     */
    private fun hasRootfs(rootfs: String): Boolean =
        ShellLocator.find(File(rootfs)) != null

    /** What both guest backends bind or project, prepared once per session start. */
    class GuestTree(
        val rootfs: File,
        val guestHome: File,
        val shell: String,
        val storage: LinkedHashMap<String, File>,
    )

    /**
     * Prepares an installed rootfs for a session, whichever backend runs it: the writable guest
     * HOME, the login user, the guest-side commands (compat shims, `xset`, `vhdp`), and the
     * storage volumes to graft under /mnt. Idempotent and refreshed every launch, so a rootfs
     * provisioned by an older build is repaired in place.
     */
    fun prepareGuestTree(ctx: Context, rootfs: String): GuestTree {
        val rootfsDir = File(rootfs)

        // Writable guest HOME backed by a host directory, bound over /home/dracos. World-
        // writable so the session can always write it regardless of host-side ownership of the
        // extracted files (an in-rootfs home can otherwise be read-only under the backend).
        val guestHome = File(ctx.filesDir, "guest-home").apply { mkdirs() }
        runCatching { Os.chmod(guestHome.absolutePath, 0x1FF) }   // 0777
        seedUserHome(ctx, guestHome)
        runCatching { File(rootfsDir, "root").mkdirs() }          // /root exists (super-user HOME target)
        runCatching { File(rootfsDir, "home/dracos").mkdirs() }   // /home/dracos exists (normal-user HOME + bind target)
        // Every launch, not just at provisioning time, so a rootfs that predates this gains the
        // user without being unpacked again. Never fatal: a failure here still boots the shell.
        runCatching { RootfsConfigurator().ensureUser(rootfsDir) }
            .onFailure { Log.w(ShellLocator.TAG, "[BOOTSTRAP] ensureUser skipped: ${it.message}") }
        installCompatShims(ctx, rootfsDir)                        // sudo/su/fakeroot + whoami/id/logname (idempotent)
        installXsetCommand(ctx, File(rootfsDir, "usr/local/bin").absolutePath)  // `xset` dashboard command
        installVhdpCommand(ctx, rootfsDir)                        // `vhdp` CLI (doctor/inspect/capabilities/run)

        // Storage, as the xset Storage Access row exposes it.
        //
        // Binds are fixed at spawn and a running backend can never be re-bound, so every real
        // volume is grafted whether or not storage permission is held yet. That is safe because
        // a bind is only a path-translation rule: establishing one needs no read permission and
        // invents nothing, reads stay gated by the OS, so an unpermitted source shows up as an
        // empty or EACCES directory that is nonetheless real. Gating the bind on permission
        // instead would strand the common case, where the user grants access from xset after the
        // shell has already spawned and there is nothing left to bind into.
        //
        //   /mnt/sdcard      the internal shared-storage root
        //   /mnt/sdcard-1 …  a removable volume root, only when one exists and is readable
        //
        // These sit at the top level, not under the /home/dracos bind. A bind mountpoint nested
        // inside another bind can fail to stat on proot 5.1.0 even while its contents stay
        // reachable, so HOME reaches them through a plain symlink instead.
        runCatching { File(rootfsDir, "mnt").mkdirs() }          // ensure the guest /mnt bind-parent exists
        val storage = LinkedHashMap<String, File>()
        for ((name, src) in storageVolumes(ctx)) {
            runCatching { File(rootfsDir, "mnt/$name").mkdirs() }
            storage[name] = src
        }
        boundStorageNames = LinkedHashSet(storage.keys)
        applyStorageVisibility(ctx, storageAccessEnabled(ctx))   // reflect the persisted ON/OFF at spawn
        logStorageTrace(ctx, "spawn")

        val shell = ShellLocator.guestPath(rootfsDir) ?: "/bin/sh"
        return GuestTree(rootfsDir, guestHome, shell, storage)
    }

    /**
     * The proot command line for an installed rootfs, used when VHDP cannot run it (see
     * [com.xdrac.guest.GuestBackend.selectBackend]).
     */
    fun prootArgv(ctx: Context, rootfs: String): Array<String> {
        val nativeLib = ctx.applicationInfo.nativeLibraryDir
        val tree = prepareGuestTree(ctx, rootfs)

        val binds = ArrayList<String>()
        fun bind(src: String?, dst: String) { if (src != null) { binds += "-b"; binds += "$src:$dst" } }
        // The device/system projection (/dev, /proc, /sys) is decided by VHDP; proot applies it.
        for ((h, g) in com.xdrac.guest.GuestBackend.systemBinds()) bind(h, g)
        bind(tree.guestHome.absolutePath, "/home/dracos")
        for ((name, src) in tree.storage) bind(src.absolutePath, "/mnt/$name")

        // -0 is fake-root, and it is not a choice: dpkg checks geteuid()==0 and sudo wants a
        // uid-0 setuid binary proot cannot forge, so without it apt and dpkg cannot write. The
        // kernel euid is therefore 0 in every state, which is why the dracos/root distinction
        // is presented at the shell rather than enforced by the kernel.
        //
        // --link2symlink emulates hardlinks with symlinks, because dpkg's link()-based backups
        // fail on Android's app-private filesystem without it.
        val head = arrayOf("$nativeLib/libproot.so", "-0", "--link2symlink", "-r", rootfs) + binds.toTypedArray()
        Log.i(ShellLocator.TAG, "[BOOTSTRAP] login user=dracos HOME=/home/dracos (proot fake-root euid 0, link2symlink), shell=${tree.shell}")
        val tail = arrayOf("-w", "/home/dracos", tree.shell, "-l")

        val argv = head + tail
        Log.i(ShellLocator.TAG, "[BOOTSTRAP] proot argv: " + argv.joinToString(" "))
        return argv
    }

    /**
     * The VHDP command line for an installed rootfs: phdp's rootless engine, started from
     * nativeLibraryDir, runs the guest itself -- guest programs start through the userland loader
     * (libvhdp-loader.so) instead of execve() from app storage, which Android forbids.
     *
     * It presents the same guest as [prootArgv]: fake-root identity (--uid 0 --gid 0; the
     * dracos/root distinction is still presentation via DRAC_SU), host /proc, /sys bound, the
     * writable HOME and every storage volume bound read-write, and the login shell in
     * /home/dracos. /dev is VHDP's minimal device projection over the rootfs's own /dev (see
     * [ensureGuestDev]), which, unlike a whole-/dev bind, can be listed.
     */
    fun vhdpArgv(ctx: Context, rootfs: String, guestEnv: Array<String>): Array<String> {
        val nativeLib = ctx.applicationInfo.nativeLibraryDir
        val tree = prepareGuestTree(ctx, rootfs)
        ensureGuestDev(tree.rootfs)

        val args = arrayListOf(*phdpEventsToLog(ctx), "$nativeLib/libphdp.so", "run",
            "--event-fd", "3",
            "--engine", "rootless",
            "--uid", "0", "--gid", "0",
            "--proc", "host",
            "--dev", "minimal",
            "--cwd", "/home/dracos",
            "--clear-env"
        )
        for (kv in guestEnv) { args += "-e"; args += kv }
        // /dev and /proc are projected natively above; any other system tree VHDP decided on is bound.
        for ((h, g) in com.xdrac.guest.GuestBackend.systemBinds()) {
            if (g != "/dev" && g != "/proc") { args += "--bind"; args += "$h:$g:rw" }
        }
        args += "--bind"; args += "${tree.guestHome.absolutePath}:/home/dracos:rw"
        for ((name, src) in tree.storage) { args += "--bind"; args += "${src.absolutePath}:/mnt/$name:rw" }
        args += rootfs; args += "--"; args += tree.shell; args += "-l"

        Log.i(ShellLocator.TAG, "[BOOTSTRAP] login user=dracos HOME=/home/dracos (VHDP fake-root uid 0, userland loader), shell=${tree.shell}")
        Log.i(ShellLocator.TAG, "[BOOTSTRAP] vhdp argv: " + args.joinToString(" "))
        return args.toTypedArray()
    }

    /**
     * Launch prefix giving phdp an event descriptor 3 appended to cache/vhdp-events.log. phdp
     * reports its diagnostics (refused syscalls, paths outside the session) on that stream,
     * which is otherwise stderr: the terminal itself, where they would be interleaved with the
     * guest's output. Start failures are still printed on stderr. `exec` replaces the shell, so
     * the process the terminal holds is phdp itself. The redirection belongs to the exec'd
     * command: mksh marks a descriptor above 2 opened by a bare `exec 3>>file` close-on-exec.
     */
    private fun phdpEventsToLog(ctx: Context): Array<String> {
        val log = File(ctx.cacheDir, "vhdp-events.log")
        if (log.length() > 512L * 1024) log.delete()
        return arrayOf("/system/bin/sh", "-c", "exec \"\$@\" 3>>\"\$0\"", log.absolutePath)
    }

    /**
     * The guest /dev that VHDP's minimal projection expects, for a rootfs provisioned before
     * VHDP configure created it (configure.cpp does the same for new installs): /proc/self/fd
     * links for process substitution and /dev/std*, a sticky world-writable /dev/shm, a /dev/pts
     * directory, and placeholders so the projected nodes appear in a listing. Existing entries
     * are never replaced.
     */
    fun ensureGuestDev(rootfs: File) {
        val dev = File(rootfs, "dev").apply { mkdirs() }
        for ((name, target) in listOf("fd" to "/proc/self/fd", "stdin" to "/proc/self/fd/0",
                                      "stdout" to "/proc/self/fd/1", "stderr" to "/proc/self/fd/2")) {
            val link = File(dev, name)
            if (runCatching { Os.lstat(link.absolutePath) }.isFailure) {
                runCatching { Os.symlink(target, link.absolutePath) }
            }
        }
        File(dev, "shm").let { it.mkdirs(); runCatching { Os.chmod(it.absolutePath, 0x3FF) } }  // 01777
        File(dev, "pts").mkdirs()
        for (n in listOf("null", "zero", "full", "random", "urandom", "tty", "ptmx")) {
            val f = File(dev, n)
            if (runCatching { Os.lstat(f.absolutePath) }.isFailure) runCatching { f.createNewFile() }
        }
    }

    /** The persisted Storage-Access opt-in (`storage.enabled` in the xset store). Off by
     *  default: ~/sdcard appears only once the user turns it on. */
    private fun storageAccessEnabled(ctx: Context): Boolean =
        ctx.getSharedPreferences("xset", Context.MODE_PRIVATE)
            .getString("storage.enabled", "off")
            .let { it == "on" || it == "1" || it == "true" }

    // Volume names bound at spawn. proot binds are fixed for the life of the process, so a
    // volume the user inserts afterwards can never be bound; applyStorageVisibility only
    // links names from this set, and so never leaves a link pointing at nothing.
    @Volatile private var boundStorageNames: Set<String> = emptySet()

    /** The storage volumes to expose for THIS device: guest name -> real host source root. `sdcard`
     *  is the internal shared-storage root (/sdcard = Environment.getExternalStorageDirectory()).
     *  Removable volumes (SD card / USB-OTG) become `sdcard-1`, `sdcard-2`, …, each the volume ROOT,
     *  and only when that root actually exists and is readable. There is deliberately no app-specific
     *  fallback: a private app dir is not external storage, so an absent/unreadable removable volume
     *  is simply omitted (sdcard-1 is never invented). Order: internal first, then removables. */
    private fun storageVolumes(ctx: Context): LinkedHashMap<String, File> {
        val out = LinkedHashMap<String, File>()
        runCatching { Environment.getExternalStorageDirectory() }.getOrNull()
            ?.takeIf { it.exists() }?.let { out["sdcard"] = it }
        val volumes = runCatching { ctx.getExternalFilesDirs(null) }.getOrNull() ?: emptyArray()
        var ext = 0
        for (i in 1 until volumes.size) {
            val appDir = volumes[i] ?: continue
            val root = generateSequence(appDir as File?) { it.parentFile }
                .firstOrNull { it.name.equals("Android", ignoreCase = true) }?.parentFile
            // Real removable ROOT only, exists AND readable. No fabricated placeholder otherwise.
            if (root != null && root.exists() && root.canRead()) {
                ext++
                out[if (ext == 1) "sdcard-1" else "sdcard-$ext"] = root
            }
        }
        return out
    }

    /** Guest names this build may expose now (`sdcard*`) plus legacy names from earlier versions,
     *  used to purge stale visibility links so an old `~/storage`/`~/external` never lingers. */
    private val STORAGE_LINK_NAMES_TO_CLEAR =
        listOf("storage", "external", "external-2", "external-3", "sdcard", "sdcard-1", "sdcard-2", "sdcard-3")

    /**
     * Turns storage visibility on or off for the session that is already running.
     *
     * The binds exist from spawn, so all this does is create or remove the symlinks
     * `~/<name> -> /mnt/<name>` in the host-side guest-home that proot has bound onto
     * /home/dracos. That is a real filesystem edit rather than a display filter, so the
     * live shell sees it at once, with no restart and no new session.
     *
     * Permission is read here rather than remembered, because the grant usually arrives
     * after the shell started. Returns false when ON was asked for without permission, so
     * the caller can send the user to grant it instead of producing an empty ~/sdcard. */
    fun applyStorageVisibility(ctx: Context, enabled: Boolean): Boolean {
        val guestHome = File(ctx.filesDir, "guest-home").apply { runCatching { mkdirs() } }
        if (!enabled) {
            for (name in STORAGE_LINK_NAMES_TO_CLEAR) removeLink(File(guestHome, name))
            logStorageTrace(ctx, "toggle-off")
            return true
        }
        // Always clear legacy-scheme links even on ON, so an old `~/storage` never shadows `~/sdcard`.
        for (name in listOf("storage", "external", "external-2", "external-3")) removeLink(File(guestHome, name))
        val granted = runCatching { PermissionManager.storageGranted(ctx) }.getOrDefault(false)
        if (!granted) { logStorageTrace(ctx, "toggle-on-denied"); return false }
        // Create ~/<name> -> /mnt/<name> for each volume bound at spawn (never for one that appeared
        // later: a running proot cannot bind it, so the link would dangle).
        for (name in boundStorageNames) ensureSymlink(File(guestHome, name), "/mnt/$name")
        logStorageTrace(ctx, "toggle-on")
        return true
    }

    /** Safe diagnostic trace: `adb logcat -s STORAGE_TRACE`. Logs only STRUCTURAL facts, never file
     *  contents, user file names, or secrets, so a single device run pinpoints which layer
     *  (permission / spawn bind / live symlink) is in play. */
    private fun logStorageTrace(ctx: Context, phase: String) {
        runCatching {
            val guestHome = File(ctx.filesDir, "guest-home")
            val enabled = storageAccessEnabled(ctx)
            val granted = runCatching { PermissionManager.storageGranted(ctx) }.getOrDefault(false)
            Log.i("STORAGE_TRACE", "[$phase] enabled=$enabled granted=$granted " +
                "guestHome=${guestHome.absolutePath} boundAtSpawn=$boundStorageNames")
            for ((name, src) in storageVolumes(ctx)) {
                val link = File(guestHome, name)
                val target = runCatching { Os.readlink(link.absolutePath) }.getOrNull()
                Log.i("STORAGE_TRACE", "[$phase]   vol=$name guestMount=/mnt/$name src=${src.absolutePath} " +
                    "srcExists=${src.exists()} srcReadable=${src.canRead()} boundAtSpawn=${name in boundStorageNames} " +
                    "link=~/$name linkExists=${isSymlink(link)} linkTarget=$target")
            }
        }
    }

    private fun ensureSymlink(link: File, target: String) {
        runCatching {
            val cur = runCatching { Os.readlink(link.absolutePath) }.getOrNull()
            if (cur == target) return
            removeLink(link)
            Os.symlink(target, link.absolutePath)
        }.onFailure { Log.w(TAG, "storage link ${link.name} -> $target failed: ${it.message}") }
    }

    /** Remove a visibility entry: unlink a symlink, or delete an empty legacy mountpoint dir. Never
     *  touches a non-empty directory, so real user data is never at risk. */
    private fun removeLink(link: File) {
        runCatching {
            if (isSymlink(link)) Os.remove(link.absolutePath)
            else if (link.isDirectory && (link.list()?.isEmpty() != false)) link.delete()
        }
    }

    private fun isSymlink(f: File): Boolean =
        runCatching { (Os.lstat(f.absolutePath).st_mode.toInt() and 0xF000) == 0xA000 }.getOrDefault(false)

    /** Seed a themed prompt + one-shot login banner into the (persistent) writable guest home,
     *  without clobbering edits the user makes across reboots. */
    private fun seedUserHome(ctx: Context, home: File) {
        // Reference-style two-line prompt as a Prompt State Machine (NORMAL vs SUPER_USER). Shipped
        // as an asset so there is no fragile in-Kotlin PS1 escaping; app-managed and refreshed every
        // launch (like .profile/.banner.sh) so prompt/state changes reach already-provisioned installs.
        // The base login shell has $DRAC_SU unset -> NORMAL user 'dracos' (trailing '$'); the
        // sudo/su/fakeroot shims export DRAC_SU=1 -> SUPER_USER 'root' (trailing '#'). Exiting that
        // shell returns to the base shell -> NORMAL again (a pure shell-nesting state transition).
        runCatching {
            ctx.assets.open("bashrc").use { i -> File(home, ".bashrc").outputStream().use { i.copyTo(it) } }
        }

        // Ship the ASCII banner + its centering displayer alongside the home. These are
        // app-managed identity files, refreshed every launch so version upgrades take effect.
        //
        // The version line is filled in here rather than typed into the art. It was typed
        // in once, and 1.0.1 shipped with a banner still reading 1.0.0: the art is a file
        // nobody opens when the version changes. Asking the package manager means the
        // banner cannot disagree with the APK it came from.
        runCatching {
            val version = runCatching {
                ctx.packageManager.getPackageInfo(ctx.packageName, 0).versionName
            }.getOrNull().orEmpty()
            val art = ctx.assets.open("banner.art").bufferedReader().use { it.readText() }
            File(home, ".banner.art").writeText(art.replace("@VERSION@", version))
        }
        runCatching {
            ctx.assets.open("banner.sh").use { i -> File(home, ".banner.sh").outputStream().use { i.copyTo(it) } }
        }
        // Suppress the Kali "Message from Kali developers" MOTD the documented way: pam_motd
        // skips it when ~/.hushlogin exists. Non-destructive, no system file is edited.
        runCatching { File(home, ".hushlogin").writeText("") }

        // Login profile (app-managed): source the interactive rc, then show the app banner ONCE
        // per login shell (not per subshell). Refreshed each launch so the flow stays current.
        runCatching {
            File(home, ".profile").writeText(
                "[ -f \"\$HOME/.bashrc\" ] && . \"\$HOME/.bashrc\"\n" +
                "[ -f \"\$HOME/.banner.sh\" ] && sh \"\$HOME/.banner.sh\"\n"
            )
        }
    }

    /** Install PATH-based compatibility shims (sudo/su/fakeroot) into the guest's /usr/local/bin ,
     *  the first standard directory on PATH, so they shadow the distro binaries that cannot work
     *  under PRoot. The session already runs as fake-root (uid 0), so each shim strips privilege-
     *  escalation syntax and execs the target as root, letting habitual `sudo apt …`, `sudo -i`,
     *  `sudo su`, `su -c`, and existing scripts keep working. Shipped as assets (no fragile in-Kotlin
     *  shell string). Host-side + refreshed every launch, so it also repairs an ALREADY-provisioned
     *  rootfs without re-provisioning. Non-fatal by design, never blocks the terminal. */
    private fun installCompatShims(ctx: Context, rootfsDir: File) {
        val binDir = File(rootfsDir, "usr/local/bin").apply { runCatching { mkdirs() } }
        // sudo/su/fakeroot: escalation shims (present the SUPER_USER identity).
        // whoami/id/logname: identity shims that keep the presented user in sync with $DRAC_SU, so
        // the normal-user default reports 'dracos'/1000 and super-user reports 'root'/0. They sit in
        // /usr/local/bin (first on PATH) so they shadow the distro's coreutils versions.
        for (name in arrayOf("sudo", "su", "fakeroot", "whoami", "id", "logname", "ifconfig")) {
            val f = File(binDir, name)
            runCatching {
                ctx.assets.open("compat/$name").use { i -> f.outputStream().use { i.copyTo(it) } }
                Os.chmod(f.absolutePath, 0x1ED)   // 0755 = rwxr-xr-x
            }.onFailure { Log.w(ShellLocator.TAG, "[COMPAT] shim $name install failed: ${it.message}") }
        }
    }

    /**
     * Write what the app can see of the device's network into the guest, for the `ifconfig` shim.
     *
     * The guest cannot find this out for itself. Since Android 10 an app's uid is denied
     * /proc/net and /sys/class/net (verified on device: the `shell` user reads /proc/net/dev, this
     * app's uid gets EACCES), so a real net-tools ifconfig installed in the rootfs would come back
     * empty no matter what. What an app IS allowed is ConnectivityManager, which reports the
     * interface name, addresses, MTU and DNS for each network it can use. That is written here in
     * a flat format and rendered by the shim.
     *
     * Refreshed on every session start and on every connectivity change, so the file is not a
     * one-off snapshot taken at install time.
     */
    fun writeNetSnapshot(ctx: Context) {
        val lines = StringBuilder()
        runCatching {
            val cm = ctx.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
            val active = cm.activeNetwork
            for (n in cm.allNetworks) {
                val lp = cm.getLinkProperties(n) ?: continue
                val name = lp.interfaceName ?: continue
                val caps = cm.getNetworkCapabilities(n)
                val kind = when {
                    caps == null -> "unknown"
                    caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> "wifi"
                    caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> "cellular"
                    caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> "ethernet"
                    caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN) -> "vpn"
                    else -> "other"
                }
                // LinkProperties.getMtu() only exists from API 29; minSdk here is 24, so on an
                // older device it would throw NoSuchMethodError. Zero means "not reported" and the
                // shim prints MTU:unknown rather than a made-up number.
                val mtu = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) lp.mtu else 0
                lines.append("iface\t$name\t$mtu\t$kind\t${if (n == active) "active" else "-"}\n")
                for (la in lp.linkAddresses) {
                    val ip = la.address?.hostAddress ?: continue
                    val fam = if (ip.contains(':')) "inet6" else "inet"
                    lines.append("addr\t$name\t$fam\t$ip\t${la.prefixLength}\n")
                }
                for (d in lp.dnsServers) d.hostAddress?.let { lines.append("dns\t$name\t$it\n") }
            }
        }.onFailure { Log.w(TAG, "[NET] snapshot failed: ${it.message}") }

        // Both layouts: the proot guest sees /usr/local/share, the BusyBox fallback sees the
        // app-private usr/share. Writing both keeps one shim working in either mode.
        val targets = listOf(
            File(ctx.filesDir, "rootfs/usr/local/share/dracxterm"),
            File(ctx.filesDir, "usr/share/dracxterm")
        )
        for (dir in targets) runCatching {
            if (!dir.isDirectory && !dir.mkdirs()) return@runCatching
            File(dir, "net").writeText(lines.toString())
        }
    }

    /** Install the `xset` guest command (opens the in-terminal dashboard via OSC 5391) into [binDir].
     *  Shipped as an asset; refreshed every launch so it also lands on an already-provisioned rootfs.
     *  Non-fatal, a failure never blocks the terminal. */
    private fun installXsetCommand(ctx: Context, binDir: String) {
        runCatching {
            File(binDir).mkdirs()
            val f = File(binDir, "xset")
            ctx.assets.open("compat/xset").use { i -> f.outputStream().use { i.copyTo(it) } }
            Os.chmod(f.absolutePath, 0x1ED)   // 0755 = rwxr-xr-x
        }.onFailure { Log.w(TAG, "[XSET] command install failed: ${it.message}") }
    }

    /**
     * Install the `vhdp` guest command into /usr/local/bin from the prebuilt CLI shipped in
     * assets/vhdp/bin/<abi>/vhdp.
     *
     * This is the terminal half of the VHDP integration: users get `vhdp doctor` / `inspect` /
     * `capabilities` (and `run`, where the profile allows it) without downloading anything, because
     * the binary already ships in the APK. It is a fully STATIC build, so it runs inside the glibc
     * Debian guest under either backend without depending on the guest's or Android's libc/loader. Its full
     * source ships alongside in assets/vhdp/ for anyone who wants to rebuild it. The ABI is chosen
     * from Build.SUPPORTED_ABIS so the same code path serves an arm64 device and an x86_64 emulator.
     * Refreshed every launch like the compat/xset shims; non-fatal, never blocks the terminal.
     */
    private fun installVhdpCommand(ctx: Context, rootfsDir: File) {
        val abi = (Build.SUPPORTED_ABIS ?: emptyArray())
            .firstOrNull { it == "arm64-v8a" || it == "x86_64" }
            ?: Build.SUPPORTED_ABIS?.firstOrNull() ?: "arm64-v8a"
        runCatching {
            val binDir = File(rootfsDir, "usr/local/bin").apply { mkdirs() }
            val f = File(binDir, "vhdp")
            ctx.assets.open("vhdp/bin/$abi/vhdp").use { i -> f.outputStream().use { i.copyTo(it) } }
            Os.chmod(f.absolutePath, 0x1ED)   // 0755 = rwxr-xr-x
        }.onFailure { Log.w(ShellLocator.TAG, "[VHDP] command install failed ($abi): ${it.message}") }
    }

    /**
     * Link every BusyBox applet into [usrBin].
     *
     * BusyBox chooses what to run from argv[0], and Java cannot set argv[0] apart from
     * the program path. The binary is packaged as libbusybox.so, because Android only
     * extracts entries matching lib*.so, and "libbusybox.so" is not an applet name, so
     * running it by its own path answers "applet not found" and exits 127, for --list and
     * for every applet alike. This build has the `busybox` multiplexer applet disabled as
     * well, so there is no argv[1] fallback either. The only way in is a link whose NAME
     * is the applet, which is why one is made before asking for the list.
     *
     * The exit status is checked because the failure mode is silent otherwise: BusyBox
     * writes "libbusybox.so: applet not found" as a single non-blank line, which reads as a
     * one-entry applet list, and a symlink then gets named after the error message. Hence
     * also the sanity check on the list length; a real BusyBox lists hundreds.
     */
    private fun installBusyboxApplets(busybox: String, usrBin: String) {
        symlink(busybox, "$usrBin/busybox")

        val applets = runCatching {
            val p = ProcessBuilder("$usrBin/busybox", "--list")
                .redirectErrorStream(true).start()
            val out = p.inputStream.bufferedReader().readLines()
            val code = p.waitFor()
            if (code != 0) error("busybox --list exited $code: ${out.firstOrNull().orEmpty()}")
            val list = out.filter { it.isNotBlank() && !it.contains('/') }
            // A real BusyBox lists hundreds. Anything tiny is an error message wearing
            // the shape of a list, and must not become a filename.
            if (list.size < 16) error("busybox --list returned ${list.size} entries")
            list
        }.getOrElse {
            Log.w(TAG, "busybox --list failed, using core applet set", it)
            listOf("sh", "ash", "ls", "cat", "echo", "env", "pwd", "cd", "mkdir",
                   "rm", "cp", "mv", "ln", "chmod", "grep", "sed", "awk", "vi",
                   "ps", "kill", "uname", "id", "whoami", "clear", "date", "head", "tail")
        }
        for (a in applets) symlink(busybox, "$usrBin/$a")
        Log.i(TAG, "installed ${applets.size} busybox applet links")
    }

    private fun writeAshrc(home: String) {
        // BusyBox fallback (no rootfs): coloured '$' prompt + a whoami function presenting 'dracos',
        // so the normal-user contract (whoami=dracos, trailing '$') also holds without a Linux image.
        // No real /home/dracos exists on a non-rooted device, so HOME stays app-private here.
        //
        // The colour escapes are wrapped in \[ \]. Without those markers BusyBox ash counts the
        // escape BYTES as visible prompt width -- measured on BusyBox 1.36: it read this 8-column
        // prompt as 17 columns wide, so it placed the cursor nine columns right of the text and
        // wrapped long lines early. ash honours \[ \] exactly as bash does.
        val esc = "\u001B"
        val ashrc = buildString {
            append("export PS1=\"\\[${esc}[35m\\]dracos\\[${esc}[0m\\]:\\[${esc}[36m\\]\\w\\[${esc}[0m\\]\\\$ \"\n")
            append("whoami() { echo dracos; }\n")
        }
        runCatching { File(home, ".ashrc").writeText(ashrc) }
    }

    /**
     * Replace a symlink, including one that no longer resolves.
     *
     * The delete is unconditional because File.exists() follows the link: a symlink whose
     * target is gone reports false, so guarding the delete with it leaves the stale link
     * in place and Os.symlink then fails with EEXIST. File.delete() unlinks a dangling
     * symlink perfectly well; exists() is the call that cannot see one.
     *
     * An app update produces exactly that state. The install directory is renamed on every
     * update, so nativeLibraryDir moves while filesDir survives, and libtalloc.so.2 is left
     * pointing into the old install. proot cannot resolve its DT_NEEDED, the shell dies as
     * it starts, and the terminal closes itself with nothing on screen to explain why.
     */
    private fun symlink(target: String, linkPath: String) {
        File(linkPath).delete()
        runCatching { Os.symlink(target, linkPath) }
            .onFailure { Log.w(TAG, "symlink $linkPath -> $target failed: ${it.message}") }

        // Verify rather than assume. A symlink that does not resolve costs the whole
        // terminal, and it is a one-syscall check.
        if (!File(linkPath).exists()) {
            Log.e(TAG, "symlink $linkPath -> $target does not resolve; proot will not start")
        }
    }
}
