package com.dracxterm.claw

import android.content.Context
import android.content.res.AssetManager
import android.os.Build
import android.util.Log
import java.io.File

/**
 * Puts the `claw` command into the terminal. The work is done natively by libclawsetup.so
 * (app/src/main/cpp/claw/ClawSetup.cpp); this object only picks the ABI and the stamp and keeps
 * any failure away from the caller.
 *
 * Called once per session spawn from Bootstrap, like OllamaLauncher.attach. With a rootfs it
 * installs /opt/claw and /usr/local/bin/claw; without one it puts a stub on the BusyBox PATH
 * that explains claw needs the Linux rootfs.
 */
object ClawSetup {

    private const val TAG = "claw-setup"

    private val loaded: Boolean by lazy {
        runCatching { System.loadLibrary("clawsetup") }
            .onFailure { Log.w(TAG, "libclawsetup.so did not load: ${it.message}") }
            .isSuccess
    }

    @JvmStatic
    private external fun nativeInstall(
        assets: AssetManager,
        rootfs: ByteArray,
        busyboxBin: ByteArray,
        abi: ByteArray,
        stamp: ByteArray,
    ): ByteArray

    fun attach(ctx: Context, rootfs: File?, busyboxBinDir: String) {
        runCatching {
            if (!loaded) return
            val app = ctx.applicationContext
            // Same ABI choice as Bootstrap.installVhdpCommand, so both guest binaries agree.
            val abi = (Build.SUPPORTED_ABIS ?: emptyArray())
                .firstOrNull { it == "arm64-v8a" || it == "x86_64" }
                ?: Build.SUPPORTED_ABIS?.firstOrNull() ?: "arm64-v8a"
            // A new APK (reinstall or upgrade) changes the stamp and triggers a full refresh.
            @Suppress("DEPRECATION")
            val info = app.packageManager.getPackageInfo(app.packageName, 0)
            @Suppress("DEPRECATION")
            val stamp = "${info.versionName}/${info.versionCode}/${info.lastUpdateTime}/$abi"
            val result = nativeInstall(
                app.assets,
                (rootfs?.absolutePath ?: "").toByteArray(),
                busyboxBinDir.toByteArray(),
                abi.toByteArray(),
                stamp.toByteArray(),
            ).toString(Charsets.UTF_8)
            Log.i(TAG, "[CLAW] $result")
        }.onFailure { Log.w(TAG, "[CLAW] setup skipped: ${it.message}") }
    }
}
