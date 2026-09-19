package com.dracxterm.rootfs

import android.system.Os
import android.util.Log
import java.io.File

/**
 * Locates a login shell inside an extracted rootfs.
 *
 * Uses Os.lstat, which does not follow symlinks, rather than File.exists(), which does.
 * Minimal images such as Alpine ship `/bin/sh` as an absolute symlink to
 * `/bin/busybox`; on the Android host File.exists() resolves that against the host root
 * (no /bin/busybox there) and wrongly reports the shell missing, even though PRoot
 * re-roots the guest and resolves it correctly. lstat treats the symlink entry itself as
 * present, which is the correct test for "does the rootfs provide this shell".
 */
object ShellLocator {

    const val TAG = "dracXterm"

    /**
     * Relative shell paths. bash is preferred so Debian/Kali/Ubuntu images boot into
     * their real login shell (reading /etc/bash.bashrc + ~/.bashrc, i.e. the themed
     * reference prompt) instead of dash's bare "#". sh/ash follow as the fallback for
     * minimal images (e.g. Alpine, whose /bin/sh is a busybox symlink and has no bash).
     */
    val CANDIDATES = listOf(
        "bin/bash", "usr/bin/bash",
        "bin/sh", "usr/bin/sh",
        "bin/ash", "usr/bin/ash"
    )

    /** Real ELF interpreters PRoot can exec (a symlinked sh must resolve to one of these). */
    private val EXEC_BINARIES = listOf(
        "bin/busybox", "usr/bin/busybox", "bin/bash", "usr/bin/bash",
        "bin/ash", "bin/dash", "bin/sh", "usr/bin/sh"
    )

    /** True if the path exists as a regular file, directory, OR (possibly dangling) symlink. */
    fun entryExists(path: String): Boolean =
        runCatching { Os.lstat(path); true }.getOrDefault(false)

    /** The first shell candidate that exists in [rootfs], or null. Logs every check. */
    fun find(rootfs: File, verbose: Boolean = false): String? {
        var found: String? = null
        for (c in CANDIDATES) {
            val abs = File(rootfs, c).absolutePath
            val ok = entryExists(abs)
            if (verbose) Log.i(TAG, "[RUNTIME] Searching: /$c -> ${if (ok) "present" else "absent"}")
            if (ok && found == null) { found = c; if (!verbose) break }
        }
        return found
    }

    /** Guest (post-reroot) absolute path for the located shell, e.g. "/bin/sh". */
    fun guestPath(rootfs: File): String? = find(rootfs)?.let { "/$it" }

    /** True if at least one real exec-able shell/interpreter binary is present. */
    fun hasExecBinary(rootfs: File): Boolean =
        EXEC_BINARIES.any { entryExists(File(rootfs, it).absolutePath) }
}
