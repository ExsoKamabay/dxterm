package com.dracxterm.xset

import com.dracxterm.vhdp.Vhdp
import org.json.JSONObject

/**
 * Built-in configuration modules + the single place that declares defaults and registers modules.
 * A brand-new `xset <name>` menu is added by writing an [XsetModule] and adding one line to
 * [XsetBootstrap.install], the controller/renderer/store are never touched. Guest-coupled modules
 * (prompt/shell/plugins) are intentionally not registered here yet; see ROADMAP in the audit report.
 *
 * Localization: user-facing labels/titles/options/actions resolve through [XsetI18n.t] at build time,
 * so when the `ui.lang` setting changes the whole dashboard re-renders in the chosen language (see
 * [XsetLang]). Technical literals (paths, versions, model names, colour hex, brand/preset names,
 * units, "ON/OFF") are deliberately left untranslated.
 */

// Curated ARGB colours (opaque). Stored as the decimal string of the Int.
/** Theme-preset palette. FACADE over [XsetDesign]; names preserved so [XsetThemes] is untouched. */
private object C {
    const val WHITE  = XsetDesign.Text.PRIMARY;   const val GREEN = XsetDesign.State.SUCCESS; const val CYAN = XsetDesign.State.INFO
    const val AMBER  = XsetDesign.State.WARNING;  const val PURPLE = XsetDesign.ACCENT_SOFT; const val RED = XsetDesign.State.ERROR
    const val BLUE   = XsetDesign.Ansi.BLUE;      const val MONO = XsetDesign.Ansi.WHITE
    const val INK    = XsetDesign.Surface.BG;     const val SLATE = XsetDesign.Preset.SLATE; const val NAVY = XsetDesign.Preset.NAVY
    const val BLACK  = XsetDesign.Ansi.BLACK;     const val DEEP = XsetDesign.Preset.DEEP;   const val PLUM = XsetDesign.Preset.PLUM
    const val ACCENT = XsetDesign.ACCENT
}

private fun opt(label: String, argb: Int) = Opt(label, argb.toString())

/** name, fg, bg, cursor. */
data class ThemePreset(val id: String, val label: String, val fg: Int, val bg: Int, val cursor: Int)

object XsetThemes {
    val presets = listOf(
        ThemePreset("cyber", "Cyber Neon", 0xFFE6E6E6.toInt(), 0xFF0A0A0C.toInt(), 0xFF8A5CF6.toInt()),
        ThemePreset("synth", "Synthwave", 0xFFFF7EDB.toInt(), 0xFF1A1033.toInt(), 0xFF00E5FF.toInt()),
        ThemePreset("matrix", "Matrix", 0xFF3DDC84.toInt(), 0xFF000800.toInt(), 0xFF3DDC84.toInt()),
        ThemePreset("dracula", "Dracula", 0xFFF8F8F2.toInt(), 0xFF282A36.toInt(), 0xFFFF79C6.toInt()),
        ThemePreset("nord", "Nord", 0xFFD8DEE9.toInt(), 0xFF2E3440.toInt(), 0xFF88C0D0.toInt()),
        ThemePreset("tokyo", "Tokyo Night", 0xFFC0CAF5.toInt(), 0xFF1A1B26.toInt(), 0xFF7AA2F7.toInt()),
        ThemePreset("gruvbox", "Gruvbox Dark", 0xFFEBDBB2.toInt(), 0xFF282828.toInt(), 0xFFFE8019.toInt()),
        ThemePreset("solar", "Solarized Dark", 0xFF93A1A1.toInt(), 0xFF002B36.toInt(), 0xFFB58900.toInt()),
        ThemePreset("amber", "Amber CRT", 0xFFFFB000.toInt(), 0xFF1A0F00.toInt(), 0xFFFFB000.toInt()),
        ThemePreset("mono", "Mono Slate", 0xFFC7C7CC.toInt(), 0xFF15151A.toInt(), 0xFFC7C7CC.toInt()),
    )
    fun byId(id: String) = presets.firstOrNull { it.id == id } ?: presets[0]
}

/** Registers every default key exactly once. Called by both the app and host tests for parity. */
object XsetDefaults {
    fun register(store: XsetStore) {
        store.def("ui.lang", "indonesia")   // settings-display language (xset UI); default Indonesian
            .def("theme.preset", "cyber")
            .def("theme.fg", C.WHITE.toString()).def("theme.bg", C.INK.toString()).def("theme.cursor", C.ACCENT.toString())
            .def("font.size", "12").def("font.family", "jetbrains")
            .def("font.linespacing", "100").def("font.letterspacing", "0").def("font.bold_bright", "off")
            .def("cursor.style", "block").def("cursor.blink", "off").def("cursor.color", C.ACCENT.toString())
            .def("layout.padding", "100")
            .def("perf.scrollback", "5000")
            .def("storage.enabled", "off")
            // claw AI: default aman. browse=tanya dulu, humanizer mati, jaringan lokal diblokir.
            .def("claw.browse", "ask")
            .def("claw.private_network", "off")
            .def("claw.skills", "on")
            .def("claw.auto_update", "on")
            .def("claw.update_hours", "24")
            .def("claw.proxy_token", "on")
            .def("claw.save_conversations", "off")
            .def("claw.humanizer", "off")
            .def("claw.chat_model", "auto")
            .def("claw.humanizer_model", "auto")
            .def("claw.humanizer_agents", "3")
            .def("claw.activity_log", "on")
            .def("claw.fs_access", "off")
    }
}

// -------- helpers to build live-applying settings --------
// Shorthand for a translated UI string.
private fun t(key: String) = XsetI18n.t(key)

// Every theme preset except Cyber Neon sets at least one colour outside the curated option lists,
// and the row then showed the stored ARGB Int ("-33061"). Such a value is shown as #RRGGBB instead.
private fun enumColor(store: XsetStore, key: String, label: String, opts: List<Opt>, apply: (Int) -> Unit) =
    Setting(key, label, SettingKind.ENUM, options = opts,
        read = { store.get(key) },
        write = { v -> store.set(key, v); apply(v.toIntOrNull() ?: C.WHITE) },
        format = { v -> v.toIntOrNull()?.let { "#%06X".format(it and 0xFFFFFF) } ?: v })

private fun intSetting(store: XsetStore, key: String, label: String, mn: Int, mx: Int, st: Int, hint: String, apply: (Int) -> Unit) =
    Setting(key, label, SettingKind.INT, min = mn, max = mx, step = st, hint = hint,
        read = { store.get(key) },
        write = { v -> store.set(key, v); apply(v.toIntOrNull() ?: mn) })

private fun toggle(store: XsetStore, key: String, label: String, apply: (Boolean) -> Unit) =
    Setting(key, label, SettingKind.TOGGLE,
        read = { store.get(key) },
        write = { v -> store.set(key, v); apply(v.equals("on", true)) })

private fun enumSetting(store: XsetStore, key: String, label: String, opts: List<Opt>, apply: (String) -> Unit) =
    Setting(key, label, SettingKind.ENUM, options = opts,
        read = { store.get(key) },
        write = { v -> store.set(key, v); apply(v) })

/** [labelKey] is a stable i18n key: it yields both the language-independent Setting key and the
 *  translated label, so an action's identity never shifts when the UI language changes. */
private fun action(labelKey: String, run: () -> String?) =
    Setting("action.$labelKey", t(labelKey), SettingKind.ACTION, run = run, read = { "" })

private fun info(key: String, label: String, value: () -> String) =
    Setting(key, label, SettingKind.INFO, read = value)

// Colour/preset option labels are proper/brand names, shown the same in every language.
private val FG_OPTS = listOf(opt("White", C.WHITE), opt("Green", C.GREEN), opt("Cyan", C.CYAN),
    opt("Amber", C.AMBER), opt("Purple", C.PURPLE), opt("Red", C.RED), opt("Blue", C.BLUE), opt("Mono", C.MONO))
private val BG_OPTS = listOf(opt("Ink", C.INK), opt("Slate", C.SLATE), opt("Navy", C.NAVY),
    opt("Black", C.BLACK), opt("Deep", C.DEEP), opt("Plum", C.PLUM))
private val CUR_OPTS = listOf(opt("Purple", C.ACCENT), opt("Cyan", C.CYAN), opt("Green", C.GREEN),
    opt("Amber", C.AMBER), opt("White", C.WHITE))

// Cursor-shape options carry translated labels (value is the canonical id).
private fun cursorStyleOpts() = listOf(
    Opt(t("opt.block"), "block"), Opt(t("opt.bar"), "bar"),
    Opt(t("opt.underline"), "underline"), Opt(t("opt.hollow"), "hollow"))

// -------- modules --------

/** Settings-display language. Switching re-renders the whole dashboard in-place (no new window). */
class LanguageModule : XsetModule {
    override val id = "language"
    override val title: String get() = t("mod.language")
    override val icon = XsetDesign.Icon.LANGUAGE
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        return listOf(
            // Native names in the option list, so you can always find your own language. The apply
            // updates the live language immediately; the controller also re-syncs each render.
            enumSetting(s, "ui.lang", t("lbl.ui_lang"), XsetLang.options()) { XsetI18n.current = XsetLang.from(it) },
        )
    }
}

class ThemeModule : XsetModule {
    override val id = "theme"; override val title: String get() = t("mod.theme"); override val icon = XsetDesign.Icon.THEME
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        val presetOpts = XsetThemes.presets.map { Opt(it.label, it.id) }
        return listOf(
            enumSetting(s, "theme.preset", t("lbl.preset"), presetOpts) { id ->
                val p = XsetThemes.byId(id)
                s.set("theme.fg", p.fg.toString()); s.set("theme.bg", p.bg.toString()); s.set("theme.cursor", p.cursor.toString())
                s.set("cursor.color", p.cursor.toString())
                ctx.applyTheme(p.fg, p.bg, p.cursor)
            },
            enumColor(s, "theme.fg", t("lbl.foreground"), FG_OPTS) { ctx.applyForeground(it) },
            enumColor(s, "theme.bg", t("lbl.background"), BG_OPTS) { ctx.applyBackground(it) },
            enumColor(s, "theme.cursor", t("lbl.cursor_color"), CUR_OPTS) { ctx.applyCursorColor(it); s.set("cursor.color", it.toString()) },
            action("act.reset_theme") {
                val p = XsetThemes.presets[0]
                s.set("theme.preset", p.id); s.set("theme.fg", p.fg.toString()); s.set("theme.bg", p.bg.toString())
                s.set("theme.cursor", p.cursor.toString()); s.set("cursor.color", p.cursor.toString())
                ctx.applyTheme(p.fg, p.bg, p.cursor); "Theme reset to ${p.label}"
            },
            info("theme.info", t("lbl.active"), { XsetThemes.byId(s.get("theme.preset")).label }),
        )
    }
}

class FontModule : XsetModule {
    override val id = "font"; override val title: String get() = t("mod.font"); override val icon = XsetDesign.Icon.FONT
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        return listOf(
            intSetting(s, "font.size", t("lbl.size_dp"), 7, 28, 1, "cell height") { ctx.applyFontSizeDp(it) },
            enumSetting(s, "font.family", t("lbl.family"), listOf(Opt("JetBrains Mono", "jetbrains"), Opt("System Mono", "system"))) { ctx.applyFontFamily(it) },
            intSetting(s, "font.linespacing", t("lbl.line_spacing"), 90, 160, 5, "row height") { ctx.applyLineSpacing(it) },
            intSetting(s, "font.letterspacing", t("lbl.letter_spacing"), 0, 120, 5, "tracking") { ctx.applyLetterSpacing(it) },
            toggle(s, "font.bold_bright", t("lbl.bold_bright")) { ctx.applyBoldBright(it) },
            intSetting(s, "layout.padding", t("lbl.padding"), 50, 200, 10, "content inset") { ctx.applyPaddingScale(it) },
            action("act.reset_font") {
                s.set("font.size", "12"); s.set("font.family", "jetbrains"); s.set("font.linespacing", "100")
                s.set("font.letterspacing", "0"); s.set("font.bold_bright", "off"); s.set("layout.padding", "100")
                ctx.reapplyAll(); "Font reset"
            },
            info("font.note", t("lbl.renderer"), { "damage-driven · dp-based" }),
        )
    }
}

class CursorModule : XsetModule {
    override val id = "cursor"; override val title: String get() = t("mod.cursor"); override val icon = XsetDesign.Icon.CURSOR
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        return listOf(
            enumSetting(s, "cursor.style", t("lbl.style"), cursorStyleOpts()) { ctx.applyCursorStyle(CursorStyle.from(it)) },
            toggle(s, "cursor.blink", t("lbl.blink")) { ctx.applyCursorBlink(it) },
            enumColor(s, "cursor.color", t("lbl.color"), CUR_OPTS) { ctx.applyCursorColor(it) },
            action("act.reset_cursor") {
                s.set("cursor.style", "block"); s.set("cursor.blink", "off"); s.set("cursor.color", C.ACCENT.toString())
                ctx.applyCursorStyle(CursorStyle.BLOCK); ctx.applyCursorBlink(false); ctx.applyCursorColor(C.ACCENT); "Cursor reset"
            },
            info("cursor.note", t("lbl.shapes"), { "block · bar · underline · hollow" }),
        )
    }
}

class AppearanceModule : XsetModule {
    override val id = "appearance"; override val title: String get() = t("mod.appearance"); override val icon = XsetDesign.Icon.APPEARANCE
    override fun build(ctx: XsetContext): List<Setting> {
        // Curated overview pulling the highest-value knobs from the dedicated modules (same keys →
        // changing here changes everywhere, no duplicated state).
        val s = ctx.store
        val presetOpts = XsetThemes.presets.map { Opt(it.label, it.id) }
        return listOf(
            enumSetting(s, "theme.preset", t("lbl.theme"), presetOpts) { id ->
                val p = XsetThemes.byId(id)
                s.set("theme.fg", p.fg.toString()); s.set("theme.bg", p.bg.toString()); s.set("theme.cursor", p.cursor.toString())
                s.set("cursor.color", p.cursor.toString()); ctx.applyTheme(p.fg, p.bg, p.cursor)
            },
            intSetting(s, "font.size", t("lbl.font_size"), 7, 28, 1, "") { ctx.applyFontSizeDp(it) },
            enumSetting(s, "cursor.style", t("lbl.cursor"), cursorStyleOpts()) { ctx.applyCursorStyle(CursorStyle.from(it)) },
            toggle(s, "cursor.blink", t("lbl.cursor_blink")) { ctx.applyCursorBlink(it) },
            enumColor(s, "theme.fg", t("lbl.foreground"), FG_OPTS) { ctx.applyForeground(it) },
            enumColor(s, "theme.bg", t("lbl.background"), BG_OPTS) { ctx.applyBackground(it) },
            intSetting(s, "font.linespacing", t("lbl.line_spacing"), 90, 160, 5, "") { ctx.applyLineSpacing(it) },
            intSetting(s, "layout.padding", t("lbl.padding"), 50, 200, 10, "") { ctx.applyPaddingScale(it) },
            enumColor(s, "cursor.color", t("lbl.cursor_color"), CUR_OPTS) { ctx.applyCursorColor(it) },
            toggle(s, "font.bold_bright", t("lbl.bold_bright")) { ctx.applyBoldBright(it) },
        )
    }
}

class BackgroundModule : XsetModule {
    override val id = "background"; override val title: String get() = t("mod.background"); override val icon = XsetDesign.Icon.BACKGROUND
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        return listOf(
            enumColor(s, "theme.bg", t("lbl.color"), BG_OPTS) { ctx.applyBackground(it) },
            enumColor(s, "theme.fg", t("lbl.contrast_fg"), FG_OPTS) { ctx.applyForeground(it) },
            action("act.reset_background") { s.set("theme.bg", C.INK.toString()); ctx.applyBackground(C.INK); "Background reset" },
            info("bg.note", t("lbl.note"), { "true window transparency: device-verify (roadmap)" }),
        )
    }
}

class PerformanceModule : XsetModule {
    override val id = "performance"; override val title: String get() = t("mod.performance"); override val icon = XsetDesign.Icon.PERFORMANCE
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        return listOf(
            intSetting(s, "perf.scrollback", t("lbl.scrollback"), 200, 20000, 200, "history depth") { ctx.applyScrollback(it) },
            info("perf.render", t("lbl.rendering"), { "damage-driven (generation counter)" }),
            info("perf.frame", t("lbl.frame_pacing"), { "Choreographer vsync" }),
            info("perf.mem", t("lbl.buffer"), { "deque scrollback · O(1) both ends" }),
        )
    }
}

class BackupModule : XsetModule {
    override val id = "backup"; override val title: String get() = t("mod.backup"); override val icon = XsetDesign.Icon.BACKUP
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        return listOf(
            action("act.save_now") { ctx.status("saved"); t("st.saved") },
            action("act.export") { val p = ctx.exportConfig(); if (p != null) "Exported → $p" else "Export failed" },
            action("act.import") { if (ctx.importConfig()) { ctx.reapplyAll(); "Imported & applied ✓" } else "No import file found" },
            action("act.reset_all") { s.resetAll(); ctx.reapplyAll(); "All settings reset to defaults ✓" },
            info("backup.auto", t("lbl.autosave"), { t("val.autosave_on") }),
            info("backup.count", t("lbl.keys"), { s.snapshot().size.toString() }),
            info("backup.fmt", t("lbl.format"), { t("val.fmt_json") }),
        )
    }
}

/**
 * Storage Access, adapts termux-setup-storage to the DRAC-XTERM (PRoot) architecture. Enabling it
 * requests the correct OS permission for the Android version and exposes the real device volumes at
 * ~/sdcard (internal shared storage) and ~/sdcard-1 … (a removable SD/USB volume, only when one truly
 * exists). Storage is grafted once at session spawn to a stable top-level mountpoint (/mnt/<name>) and
 * toggled live via home symlinks, so turning it ON/OFF affects the ALREADY-RUNNING shell immediately ,
 * the session is never respawned. A denied permission is safe, the terminal keeps running.
 */
class StorageModule : XsetModule {
    override val id = "storage"; override val title: String get() = t("mod.storage"); override val icon = XsetDesign.Icon.BACKUP
    override fun build(ctx: XsetContext): List<Setting> {
        return listOf(
            Setting(
                "storage.enabled", t("lbl.storage_access"), SettingKind.TOGGLE,
                hint = "mount device storage at ~/sdcard",
                read = { if (ctx.storageEnabled()) "on" else "off" },
                write = { v -> if (v.equals("on", true)) ctx.enableStorage() else ctx.disableStorage() }
            ),
            info("storage.perm", t("lbl.permission"), { ctx.storageStatus() }),
            info("storage.mount", t("lbl.mount_point"), { t("val.storage_mount") }),
            action("act.grant_rebuild") { ctx.enableStorage() },
            info("storage.dirs", t("lbl.contents"), { t("val.storage_contents") }),
            info("storage.apply", t("lbl.applies"), { t("val.storage_applies") }),
            info("storage.safe", t("lbl.if_denied"), { t("val.storage_safe") }),
        )
    }
}

/**
 * claw, the in-guest AI assistant (app/src/main/assets/claw). This module governs what the model is
 * allowed to do and how it behaves; every change is written to /opt/claw/data/policy.json in the
 * rootfs, which `claw` reads on its next run. Nothing here grants the model access on its own, it only
 * narrows or widens what claw will attempt, so the safe defaults (browse=ask, humanizer off, private
 * network blocked, proxy token required) hold until the user decides otherwise.
 */
class ClawModule : XsetModule {
    override val id = "claw"; override val title: String get() = t("mod.claw"); override val icon = XsetDesign.Icon.PERFORMANCE
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        // Setiap perubahan menulis ulang policy.json ke rootfs.
        fun apply() { ctx.applyClawPolicy() }
        val perms = listOf(Opt(t("opt.off"), "off"), Opt(t("opt.ask"), "ask"), Opt(t("opt.allow"), "allow"))
        val browse = Setting(
            "claw.browse", t("lbl.browsing_web"), SettingKind.ENUM, options = perms,
            hint = "akses internet lewat Scrapling",
            read = { s.get("claw.browse") }, write = { v -> s.set("claw.browse", v); apply() })
        val files = Setting(
            "claw.fs_access", t("lbl.fs_access"), SettingKind.ENUM, options = perms,
            hint = "model boleh baca/ubah berkas & jalankan terminal",
            read = { s.get("claw.fs_access") }, write = { v -> s.set("claw.fs_access", v); apply() })
        // Model tiap fitur dipilih dari pool claw (models.json di rootfs); "auto" memilih sesuai tugas.
        val models = listOf(Opt(t("opt.auto"), "auto")) + ctx.clawModels().map { Opt(it, it) }
        fun model(key: String, label: String, hint: String) = Setting(
            key, label, SettingKind.ENUM, options = models, hint = hint,
            read = { s.get(key) }, write = { v -> s.set(key, v); apply() })
        return listOf(
            browse,
            files,
            toggle(s, "claw.private_network", t("lbl.private_network")) { apply() },
            toggle(s, "claw.skills", t("lbl.use_skills")) { apply() },
            model("claw.chat_model", t("lbl.chat_model"), "ngobrol & perintah langsung"),
            toggle(s, "claw.humanizer", t("lbl.humanizer")) { apply() },
            model("claw.humanizer_model", t("lbl.humanizer_model"), "dipakai tim agen otonom"),
            intSetting(s, "claw.humanizer_agents", t("lbl.humanizer_agents"), 1, 6, 1, "agen per tujuan") { apply() },
            toggle(s, "claw.auto_update", t("lbl.auto_update")) { apply() },
            intSetting(s, "claw.update_hours", t("lbl.update_hours"), 1, 168, 1, "auto-update") { apply() },
            toggle(s, "claw.proxy_token", t("lbl.proxy_token")) { apply() },
            toggle(s, "claw.save_conversations", t("lbl.save_conversations")) { apply() },
            toggle(s, "claw.activity_log", t("lbl.activity_log")) { apply() },
            action("act.apply_now") { ctx.applyClawPolicy() },
            info("claw.status", t("lbl.status"), { if (ctx.clawInstalled()) t("val.claw_installed") else t("val.claw_need_rootfs") }),
            info("claw.browsing", t("lbl.browsing_scrapling"), { if (ctx.browsingReady()) t("val.ready") else t("val.browsing_auto") }),
            info("claw.file", t("lbl.perm_stored"), { "/opt/claw/data/policy.json" }),
            info("claw.when", t("lbl.in_effect"), { t("val.when_next") }),
            info("claw.chat", t("lbl.chat"), { "claw chat" }),
            info("claw.auto", t("lbl.auto_mode"), { "claw humanizer \"<tujuan>\"" }),
            info("claw.safe", t("lbl.sensitive"), { t("val.safe_default") }),
        )
    }
}

class AboutModule : XsetModule {
    override val id = "about"; override val title: String get() = t("mod.about"); override val icon = XsetDesign.Icon.ABOUT
    override fun build(ctx: XsetContext): List<Setting> {
        // System Information: read-only, device-derived (MainActivity.deviceInfo via appInfo()).
        val rows = ctx.appInfo().map { (k, v) -> info("about.$k", k, { v }) }.toMutableList()
        // Developer contact: the address plus what to send. Read-only, copyable info rows.
        rows.add(info("about.dev", t("lbl.contact_info"), { DEVELOPER_EMAIL }))
        rows.add(info("about.dev.use1", t("lbl.contact_for"), { t("val.contact_for") }))
        rows.add(info("about.dev.use2", t("lbl.also"), { t("val.also") }))
        rows.add(action("act.close") { ctx.requestClose(); null })
        return rows
    }
}

/**
 * VHDP diagnostics. The real in-app consumer of the embedded libvhdp (com.dracxterm.vhdp.Vhdp): it
 * surfaces the library version/ABI, the build arch, the detected runtime profile, the exec-storage
 * policy, and the android-app support status, plus an on-demand active `doctor` probe. All calls are
 * cheap and cached at build() time except the explicit active-probe action; everything is guarded so
 * a VHDP failure (e.g. libvhdpjni not loaded) degrades to a single info row and never breaks xset.
 */
class DiagnosticsModule : XsetModule {
    override val id = "vhdp"; override val title: String get() = t("mod.diagnostics"); override val icon = XsetDesign.Icon.PERFORMANCE

    private data class Snap(
        val version: String, val abi: Int, val arch: String,
        val profile: String, val execPolicy: String, val androidApp: String
    )

    private fun snapshot(): Snap {
        val ver = Vhdp.version()
        val abi = Vhdp.abiVersion()
        var arch = "?"; var androidApp = "?"
        val cap = Vhdp.capabilities()
        if (cap.ok) {
            val o = JSONObject(cap.json)
            arch = o.optJSONObject("build")?.optString("arch", "?") ?: "?"
            o.optJSONArray("profiles")?.let { profs ->
                for (i in 0 until profs.length()) {
                    val p = profs.getJSONObject(i)
                    if (p.optString("id") == "android-app") androidApp = p.optString("status", "?")
                }
            }
        }
        var profile = "?"; var exec = "?"
        val doc = Vhdp.doctor(Vhdp.DOCTOR_NO_ACTIVE_PROBES)
        if (doc.ok) {
            val o = JSONObject(doc.json)
            profile = o.optString("detected_profile", "?")
            o.optJSONArray("checks")?.let { checks ->
                for (i in 0 until checks.length()) {
                    val c = checks.getJSONObject(i)
                    if (c.optString("id") == "exec.storage_policy") exec = c.optString("status", "?")
                }
            }
        }
        return Snap(ver, abi, arch, profile, exec, androidApp)
    }

    override fun build(ctx: XsetContext): List<Setting> {
        val snap = runCatching { snapshot() }.getOrNull()
            ?: return listOf(info("vhdp.err", "VHDP", { "unavailable (library not loaded)" }))
        return listOf(
            info("vhdp.ver", t("lbl.library"), { "libvhdp ${snap.version}" }),
            info("vhdp.abi", t("lbl.abi_version"), { snap.abi.toString() }),
            info("vhdp.arch", t("lbl.build_arch"), { snap.arch }),
            info("vhdp.profile", t("lbl.detected_profile"), { snap.profile }),
            info("vhdp.exec", t("lbl.exec_policy"), { snap.execPolicy }),
            info("vhdp.androidapp", t("lbl.androidapp"), { snap.androidApp }),
            info("vhdp.backend", t("lbl.guest_backend"), { ctx.guestBackend() }),
            action("act.run_probes") {
                runCatching {
                    val r = Vhdp.doctor(Vhdp.DOCTOR_REFRESH)
                    if (!r.ok) return@runCatching "doctor failed: ${Vhdp.statusName(r.status)}"
                    val checks = JSONObject(r.json).optJSONArray("checks")
                    fun status(id: String): String {
                        if (checks != null) for (i in 0 until checks.length()) {
                            val c = checks.getJSONObject(i); if (c.optString("id") == id) return c.optString("status", "?")
                        }
                        return "?"
                    }
                    "ptrace=${status("ptrace.child")} seccomp=${status("seccomp.filter")} openat2=${status("openat2")}"
                }.getOrElse { "doctor error: ${it.message}" }
            },
            info("vhdp.note", t("lbl.note"), { "the rootless engine runs guests in-app via the userland loader; proot is the fallback" }),
        )
    }
}

/** The one place modules are wired in. Adding a menu = add one register() line here. */
object XsetBootstrap {
    /** Register one module, isolating construction failures so a single bad module can't block the rest. */
    private inline fun safeReg(make: () -> XsetModule) {
        try { XsetRegistry.register(make()) }
        catch (t: Throwable) { android.util.Log.e("xset", "module init failed; skipping", t) }
    }

    fun install() {
        if (XsetRegistry.modules().isNotEmpty()) return
        safeReg { LanguageModule() }      // settings-display language (xset UI), first so it is easy to find
        safeReg { AppearanceModule() }
        safeReg { ThemeModule() }
        safeReg { FontModule() }
        safeReg { CursorModule() }
        safeReg { BackgroundModule() }
        safeReg { PerformanceModule() }
        safeReg { StorageModule() }
        safeReg { ClawModule() }          // in-guest AI (assets/claw): permissions + behaviour policy
        safeReg { DiagnosticsModule() }   // real consumer of the embedded libvhdp (com.dracxterm.vhdp.Vhdp)
        safeReg { BackupModule() }
        safeReg { AboutModule() }
    }
}
