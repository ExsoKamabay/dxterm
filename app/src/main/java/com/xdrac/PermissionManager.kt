package com.xdrac

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.Settings
import androidx.core.content.ContextCompat

/**
 * What the app may ask the OS for, and when.
 *
 * INTERNET and ACCESS_NETWORK_STATE are normal permissions, granted at install, so they
 * never appear here. Shared storage is the only thing with a runtime story, and it is
 * always opt-in from the xset Storage screen: nothing is requested at startup, and every
 * feature keeps working when it is refused.
 *
 * minSdk is 24, so no branch below needs a pre-Marshmallow case.
 */
object PermissionManager {

    /**
     * Legacy read/write permissions, for API 24 to 29 where WRITE_EXTERNAL_STORAGE still
     * grants broad access by path. Empty from API 30, where the only route to a usable
     * ~/sdcard is all-files access.
     */
    fun legacyStoragePermissions(): Array<String> =
        if (Build.VERSION.SDK_INT <= Build.VERSION_CODES.Q)
            arrayOf(
                android.Manifest.permission.READ_EXTERNAL_STORAGE,
                android.Manifest.permission.WRITE_EXTERNAL_STORAGE
            )
        else emptyArray()

    /**
     * The system "All files access" screen for this app, or null below API 30 where the
     * legacy permissions above are the whole story.
     */
    fun allFilesAccessIntent(ctx: Context): Intent? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return null
        return Intent(
            Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            Uri.parse("package:${ctx.packageName}")
        )
    }

    /**
     * True when the app can reach shared storage by path, which is what ~/sdcard needs.
     * Read live at every toggle rather than cached: the grant arrives from a Settings
     * screen, after the shell has already spawned.
     */
    fun storageGranted(ctx: Context): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) Environment.isExternalStorageManager()
        else ContextCompat.checkSelfPermission(
            ctx, android.Manifest.permission.READ_EXTERNAL_STORAGE
        ) == PackageManager.PERMISSION_GRANTED

    /** One line for the Storage row of the xset dashboard. */
    fun storageStatusText(ctx: Context): String = when {
        storageGranted(ctx) && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R ->
            "granted (all files access)"
        storageGranted(ctx) -> "granted"
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.R -> "not granted, needs all-files access"
        else -> "not granted, needs storage permission"
    }
}
