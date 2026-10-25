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
        // Tiga tema baru bergaya modern.
        // CberX — cyberpunk elegan: dasar plum-tinta, teks lavender-putih yang nyaman dibaca lama,
        // satu aksen magenta panas (ciri genre) hanya di kursor.
        ThemePreset("cberx", "CberX Cyberpunk", 0xFFECE4F5.toInt(), 0xFF0B0613.toInt(), 0xFFFF2EA6.toInt()),
        // CberY — Sci-Fi HUD: dasar navy panel instrumen, teks sian berpendar seperti layar bacaan,
        // kursor amber (pasangan klasik sian+amber kokpit HUD).
        ThemePreset("cbery", "CberY Sci-Fi HUD", 0xFF9FE8F7.toInt(), 0xFF041018.toInt(), 0xFFFFB01F.toInt()),
        // CberZ — Terminal Noir: dasar hitam-noir, teks abu kertas hangat (noir itu monokrom hangat),
        // satu kilau kuningan menahan diri di kursor.
        ThemePreset("cberz", "CberZ Noir", 0xFFD7CFC2.toInt(), 0xFF07070A.toInt(), 0xFFC8A15A.toInt()),
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
            // claw AI: kemampuan penuh jadi default khusus model dracxterm (tanpa toggle di xset).
            // browse, fs_access (berkas + terminal), jaringan lokal, skill, humanizer, auto-update
            // semuanya aktif begitu aplikasi dipasang. proxy_token tetap wajib (mengunci proxy dari
            // aplikasi lain di perangkat) dan activity_log tetap jalan untuk jejak audit.
            .def("claw.browse", "allow")
            .def("claw.private_network", "on")
            .def("claw.skills", "on")
            .def("claw.auto_update", "on")
            .def("claw.update_hours", "24")
            .def("claw.proxy_token", "on")
            .def("claw.save_conversations", "off")
            .def("claw.humanizer", "on")
            .def("claw.chat_model", "auto")
            .def("claw.humanizer_model", "auto")
            .def("claw.humanizer_agents", "3")
            .def("claw.activity_log", "on")
            .def("claw.fs_access", "allow")
            // App self-update (Settings ▸ Update): check the GitHub releases of the project on open.
            // last_check throttles the background check; notified_tag stops the same version from
            // nagging more than once. These are housekeeping values, not user-facing knobs.
            .def("update.auto_check", "on")
            .def("update.last_check_ms", "0")
            .def("update.notified_tag", "")
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
            enumSetting(s, "font.family", t("lbl.family"), listOf(
                Opt("JetBrains Mono", "jetbrains"),
                Opt("Fira Mono", "firamono"),
                Opt("Hack", "hack"),
                Opt("IBM Plex Mono", "ibmplex"),
                Opt("Inconsolata", "inconsolata"),
                Opt("Noto Sans Mono", "noto"),
                Opt("Source Code Pro", "sourcecode"),
                Opt("Space Mono", "spacemono"),
                Opt("Ubuntu Mono", "ubuntumono"),
                Opt("Anonymous Pro", "anonymous"),
                Opt("System Mono", "system"),
            )) { ctx.applyFontFamily(it) },
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
 * claw, the in-guest AI assistant (app/src/main/assets/claw). In dracxterm the model runs with full
 * capability by default: browsing, file + terminal access, local network, skills, humanizer and
 * auto-update are all on from first launch. Those values are written to /opt/claw/data/policy.json in
 * the rootfs, which `claw` reads on its next run. The per-feature permission rows were removed from
 * this page on purpose, so what remains is the model picker, an apply action, and read-only status.
 * proxy_token (locks the `claw serve` proxy to this app) and the activity log stay on as a baseline
 * and have no UI switch; the defaults themselves live in [XsetDefaults].
 */
class ClawModule : XsetModule {
    override val id = "claw"; override val title: String get() = t("mod.claw"); override val icon = XsetDesign.Icon.PERFORMANCE
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        // Model tiap fitur dipilih dari pool claw (models.json di rootfs); "auto" memilih sesuai tugas.
        val models = listOf(Opt(t("opt.auto"), "auto")) + ctx.clawModels().map { Opt(it, it) }
        fun model(key: String, label: String, hint: String) = Setting(
            key, label, SettingKind.ENUM, options = models, hint = hint,
            read = { s.get(key) }, write = { v -> s.set(key, v); ctx.applyClawPolicy() })
        // Bagian AI (claw) sengaja hanya memuat pemilih model; semua baris lain disembunyikan.
        return listOf(
            model("claw.chat_model", t("lbl.chat_model"), "model untuk claw chat & a-work"),
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

/**
 * App self-update. Reads the project's latest GitHub release, compares it with the installed
 * build, and shows the release notes so the user can see what a newer version changes before
 * updating. The network call lives on the host (off the UI thread) behind [XsetContext]; this
 * module only renders the result and triggers actions, so it stays pure and host-testable.
 */
class UpdateModule : XsetModule {
    override val id = "update"; override val title: String get() = t("mod.update"); override val icon = XsetDesign.Icon.UPDATE
    override fun build(ctx: XsetContext): List<Setting> {
        val rows = mutableListOf(
            info("update.installed", t("lbl.installed_version"), { "${ctx.appVersion()} · vc${ctx.appVersionCode()}" }),
            info("update.repo", t("lbl.repository"), { ctx.updateRepo() }),
            toggle(ctx.store, "update.auto_check", t("lbl.auto_check")) { /* persisted only; read on next open */ },
            action("act.check_update") { ctx.checkForUpdate() },
            info("update.latest", t("lbl.latest_version"), {
                when (ctx.updateSnapshot().state) {
                    UpdateState.CHECKING -> t("val.checking")
                    UpdateState.UNKNOWN -> t("val.not_checked")
                    else -> ctx.updateSnapshot().latest.ifBlank { t("val.not_checked") }
                }
            }),
            info("update.status", t("lbl.update_status"), {
                when (ctx.updateSnapshot().state) {
                    UpdateState.CURRENT -> t("val.up_to_date")
                    UpdateState.AVAILABLE -> t("val.update_available")
                    UpdateState.CHECKING -> t("val.checking")
                    UpdateState.ERROR -> t("val.check_error")
                    else -> t("val.not_checked")
                }
            }),
        )
        // Both actions are always present so the row count never shifts when a check lands between
        // repaints. With no notes yet, showReleaseNotes() returns a "not checked" line instead.
        rows.add(action("act.view_notes") { ctx.showReleaseNotes() })
        rows.add(action("act.open_release") { ctx.openReleasePage() })
        return rows
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
        safeReg { UpdateModule() }        // app self-update: checks the project's GitHub releases
        safeReg { AboutModule() }
    }
}
