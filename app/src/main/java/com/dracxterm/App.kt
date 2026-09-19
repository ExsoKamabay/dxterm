package com.dracxterm

import android.app.Application
import android.content.Context
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.os.LocaleListCompat

/**
 * Sets the app's display language before any component exists.
 *
 * Indonesian is this app's default rather than the device's, so an English phone still gets
 * values/ and not values-en/. [LocaleSupport] owns the choice; this class only makes sure the
 * process starts in it, by wrapping the base context before anything is inflated.
 *
 * The app used to drive the language through [AppCompatDelegate.setApplicationLocales], which
 * recreates every started activity. [onCreate] now clears anything that mechanism left behind,
 * once, at a point where no activity exists to be recreated.
 */
class App : Application() {

    override fun attachBaseContext(base: Context) {
        super.attachBaseContext(LocaleSupport.wrap(base))
    }

    override fun onCreate() {
        super.onCreate()
        // Migration off the old mechanism. A locale stored by AppCompat would otherwise be
        // re-applied on top of the wrapped context and pull the app back to the previous
        // language. Safe here and nowhere else: no activity exists yet, so clearing it
        // recreates nothing.
        if (!AppCompatDelegate.getApplicationLocales().isEmpty) {
            AppCompatDelegate.setApplicationLocales(LocaleListCompat.getEmptyLocaleList())
        }
    }

    companion object {
        const val PREFS = "dracxterm"
        const val KEY_LANG_CHOSEN = "language_chosen"
        const val LANG_DEFAULT = "id"
    }
}
