package com.dracxterm

import android.content.Context
import android.content.res.Configuration
import java.util.Locale

/**
 * The app's display language, owned in one place and applied without restarting anything.
 *
 * The previous mechanism was [androidx.appcompat.app.AppCompatDelegate.setApplicationLocales],
 * which is defined to recreate every started activity. That recreate is the flash the language
 * button produced, and it is also why the toggle could not be offered while provisioning was
 * running: a torn-down ProvisioningActivity came back and started a second extraction into the
 * same staging directory.
 *
 * Here the language is a preference plus two operations:
 *
 *  - [wrap] builds the localised [Context] that `attachBaseContext` hands to a component, so a
 *    process that starts fresh is already in the right language before anything is inflated.
 *  - [applyInPlace] retargets a live component's resources, so `getString` returns the other
 *    language from the next call onwards, with no recreate and no lost state. The screen then
 *    re-reads the strings it is showing.
 *
 * [Locale.setDefault] is set on both paths because it is not only Android resources that are
 * language-dependent: RootfsCatalog picks the note for `Locale.getDefault().language`.
 */
object LocaleSupport {

    /** Language tag currently chosen, falling back to the app's own default. */
    fun tag(ctx: Context): String =
        ctx.getSharedPreferences(App.PREFS, Context.MODE_PRIVATE)
            .getString(KEY_LANG, null) ?: App.LANG_DEFAULT

    /** Record a choice. Survives the process; nothing is applied by this call alone. */
    fun save(ctx: Context, tag: String) {
        ctx.getSharedPreferences(App.PREFS, Context.MODE_PRIVATE).edit()
            .putString(KEY_LANG, tag)
            .putBoolean(App.KEY_LANG_CHOSEN, true)
            .apply()
    }

    /** The other language. The app ships exactly two, so this is the whole toggle. */
    fun other(tag: String): String = if (tag.startsWith("en")) App.LANG_DEFAULT else "en"

    /**
     * A context resolving resources in the saved language. For `attachBaseContext`, where the
     * returned context becomes the component's base and every later `getString` goes through it.
     */
    fun wrap(base: Context): Context {
        val locale = Locale.forLanguageTag(tag(base))
        Locale.setDefault(locale)
        val cfg = Configuration(base.resources.configuration)
        cfg.setLocale(locale)
        cfg.setLocales(android.os.LocaleList(locale))   // minSdk 24: always available
        return base.createConfigurationContext(cfg)
    }

    /**
     * Retarget an already-created component's resources at the saved language.
     *
     * `Resources.updateConfiguration` is deprecated in favour of recreating with a new
     * configuration -- which is precisely the recreate this exists to avoid. It is used
     * deliberately and narrowly: one locale field, on a component that is about to re-read its
     * own strings, so nothing is left showing a stale value.
     */
    @Suppress("DEPRECATION")
    fun applyInPlace(ctx: Context) {
        val locale = Locale.forLanguageTag(tag(ctx))
        Locale.setDefault(locale)
        val res = ctx.resources
        val cfg = Configuration(res.configuration)
        cfg.setLocale(locale)
        cfg.setLocales(android.os.LocaleList(locale))   // minSdk 24: always available
        res.updateConfiguration(cfg, res.displayMetrics)
    }

    private const val KEY_LANG = "language_tag"
}
