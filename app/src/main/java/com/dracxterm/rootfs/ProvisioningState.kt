package com.dracxterm.rootfs

import android.content.Context
import android.util.Log
import java.io.File

/**
 * How far provisioning got, so it runs on the first launch and after an invalidation and
 * not otherwise. Kept in SharedPreferences and in a marker file inside the rootfs, because
 * either alone can be true while the other is not: app data can be cleared, and a rootfs
 * can be deleted.
 */
class ProvisioningState(private val ctx: Context) {

    enum class Stage { NONE, VALIDATED, EXTRACTING, CONFIGURING, VERIFYING, DONE, FAILED }

    private val prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val marker = File(ctx.filesDir, "rootfs/.provisioned")

    var stage: Stage
        get() = runCatching { Stage.valueOf(prefs.getString(KEY_STAGE, Stage.NONE.name)!!) }
            .getOrDefault(Stage.NONE)
        set(v) { prefs.edit().putString(KEY_STAGE, v.name).apply() }

    /**
     * True once the user has said no to fetching a Linux image.
     *
     * Consent is asked for once. Re-prompting on every launch would turn an opt-in into
     * nagging, and BusyBox is a complete terminal on its own, someone who declined is
     * not in a degraded state that needs fixing. The offer stays reachable from the
     * settings dashboard (`xset`).
     */
    var imageOfferDeclined: Boolean
        get() = prefs.getBoolean(KEY_DECLINED, false)
        set(v) { prefs.edit().putBoolean(KEY_DECLINED, v).apply() }

    fun markDone(archiveName: String) {
        runCatching {
            marker.parentFile?.mkdirs()
            marker.writeText("provisioned:$archiveName:${System.currentTimeMillis()}")
        }
        stage = Stage.DONE
    }

    fun markFailed(reason: String) {
        Log.w(TAG, "provisioning failed: $reason")
        stage = Stage.FAILED
    }

    /** True only when a previous run fully completed provisioning. */
    fun isProvisioned(): Boolean = marker.exists() && stage == Stage.DONE

    private companion object {
        const val TAG = "Provisioning"
        const val PREFS = "provisioning"
        const val KEY_STAGE = "stage"
        const val KEY_DECLINED = "image_offer_declined"
    }
}
