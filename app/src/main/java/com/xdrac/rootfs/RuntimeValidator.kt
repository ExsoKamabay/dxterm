package com.xdrac.rootfs

import android.content.Context
import android.util.Log
import java.io.File

/**
 * Whether the extracted rootfs can actually be launched.
 *
 * Shell detection goes through ShellLocator rather than File.exists(), which follows the
 * link and so misses a symlinked shell such as Alpine's /bin/sh -> /bin/busybox.
 */
class RuntimeValidator(private val ctx: Context) {

    data class Report(val ok: Boolean, val detail: String)

    private val tag = ShellLocator.TAG
    private val rootfs = File(ctx.filesDir, "rootfs")
    private val nativeLib = ctx.applicationInfo.nativeLibraryDir

    /** Quick check used every launch to decide skip-vs-provision. */
    fun isRootfsReady(): Boolean =
        rootfs.isDirectory && ShellLocator.find(rootfs) != null

    fun isBusyboxReady(): Boolean = File(nativeLib, "libbusybox.so").exists()

    /** Full validation before launching a Linux (PRoot) session; logs everything it inspects. */
    fun validate(): Report {
        Log.i(tag, "[RUNTIME] Checking rootfs: ${rootfs.absolutePath}")
        if (!File(nativeLib, "libbusybox.so").exists())        return fail("busybox binary missing")
        if (!File(nativeLib, "libproot.so").exists())          return fail("proot binary missing")
        if (!File(nativeLib, "libproot-loader.so").exists())   return fail("proot loader missing")
        if (!File(nativeLib, "libtalloc.so").exists())         return fail("talloc library missing")

        if (!rootfs.isDirectory) return fail("rootfs directory missing")

        Log.i(tag, "[RUNTIME] Checking shell")
        val shell = ShellLocator.find(rootfs, verbose = true)   // logs each candidate
        if (shell == null) {
            dumpRootfs()                                        // dir + bin/ + usr/bin listings
            return fail("rootfs shell missing")
        }
        Log.i(tag, "[RUNTIME] Shell found: /$shell")
        if (!ShellLocator.hasExecBinary(rootfs)) {
            dumpRootfs()
            return fail("shell present but no exec-able interpreter (busybox/bash) extracted")
        }
        return Report(true, "linux environment ready (shell=/$shell)")
    }

    private fun fail(reason: String): Report {
        Log.w(tag, "[RUNTIME] Shell NOT FOUND / validation failed: $reason")
        return Report(false, reason)
    }

    /** Print the rootfs top level plus bin/ and usr/bin so the real layout is visible in logcat. */
    private fun dumpRootfs() {
        Log.w(tag, "[RUNTIME] rootfs dir = ${rootfs.absolutePath}")
        Log.w(tag, "[RUNTIME] rootfs/     = ${listing(rootfs)}")
        Log.w(tag, "[RUNTIME] rootfs/bin  = ${listing(File(rootfs, "bin"))}")
        Log.w(tag, "[RUNTIME] rootfs/usr/bin = ${listing(File(rootfs, "usr/bin"))}")
    }

    private fun listing(dir: File): String =
        if (!dir.isDirectory) "(absent)" else (dir.list()?.sorted()?.take(60)?.joinToString(", ") ?: "(empty)")
}
