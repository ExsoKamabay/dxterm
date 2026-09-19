package com.dracxterm

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat

/**
 * Foreground service whose only job is to keep the app process alive while the terminal is open,
 * so the OS does not reclaim it, and kill the shell/PTY child processes, when the user switches
 * to another app.
 *
 * Design (audit-driven):
 *  - It owns NOTHING about the sessions. PTYs, shells, scrollback and the WorkspaceManager all
 *    stay in MainActivity exactly as before; this service only raises the process importance to
 *    "foreground" for the lifetime of the visible terminal. That is the single supported Android
 *    mechanism for surviving a background switch without an implementation-level leak.
 *  - START_NOT_STICKY + no WakeLock: if the process is genuinely gone the system will not
 *    resurrect the service, and nothing here holds the CPU awake. An idle shell blocked on read()
 *    consumes no CPU, so nothing here drains the battery. The persistent notification is the
 *    required cost of a foreground service, not an optional extra.
 *  - Started from MainActivity.onCreate (app is in the foreground, so the start is always allowed
 *    even under Android 12+ background-start restrictions) and stopped from MainActivity.onDestroy.
 */
class TerminalService : Service() {

    /** The foreground notification is user-visible text, so it follows the same language as
     *  the rest of the app rather than the device's. */
    override fun attachBaseContext(base: Context) {
        super.attachBaseContext(LocaleSupport.wrap(base))
    }


    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIFICATION_ID, buildNotification())
        return START_NOT_STICKY
    }

    private fun buildNotification(): Notification {
        ensureChannel()
        val open = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(getString(R.string.fgs_running))
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentIntent(open)
            .setOngoing(true)
            .setShowWhen(false)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .build()
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = getSystemService(NotificationManager::class.java) ?: return
            if (nm.getNotificationChannel(CHANNEL_ID) == null) {
                nm.createNotificationChannel(
                    NotificationChannel(
                        CHANNEL_ID,
                        "Terminal session",
                        NotificationManager.IMPORTANCE_LOW
                    ).apply {
                        description = "Keeps terminal sessions running while the app is in the background"
                        setShowBadge(false)
                    }
                )
            }
        }
    }

    companion object {
        private const val CHANNEL_ID = "dracxterm_session"
        private const val NOTIFICATION_ID = 1001

        /** Start (or re-assert) the foreground service. Safe to call repeatedly. */
        fun start(ctx: Context) {
            runCatching {
                ContextCompat.startForegroundService(ctx, Intent(ctx, TerminalService::class.java))
            }
        }

        /** Stop the foreground service and remove its notification. */
        fun stop(ctx: Context) {
            runCatching { ctx.stopService(Intent(ctx, TerminalService::class.java)) }
        }
    }
}
