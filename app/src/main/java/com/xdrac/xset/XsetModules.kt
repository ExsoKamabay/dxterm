package com.xdrac.xset

/**
 * Built-in configuration modules + the single place that declares defaults and registers modules.
 * A brand-new `xset <name>` menu is added by writing an [XsetModule] and adding one line to
 * [XsetBootstrap.install], the controller/renderer/store are never touched. Guest-coupled modules
 * (prompt/shell/plugins) are intentionally not registered here yet; see ROADMAP in the audit report.
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
        store.def("theme.preset", "cyber")
            .def("theme.fg", C.WHITE.toString()).def("theme.bg", C.INK.toString()).def("theme.cursor", C.ACCENT.toString())
            .def("font.size", "12").def("font.family", "jetbrains")
            .def("font.linespacing", "100").def("font.letterspacing", "0").def("font.bold_bright", "off")
            .def("cursor.style", "block").def("cursor.blink", "off").def("cursor.color", C.ACCENT.toString())
            .def("layout.padding", "100")
            .def("perf.scrollback", "5000")
            .def("storage.enabled", "off")
    }
}

// -------- helpers to build live-applying settings --------

private fun enumColor(store: XsetStore, key: String, label: String, opts: List<Opt>, apply: (Int) -> Unit) =
    Setting(key, label, SettingKind.ENUM, options = opts,
        read = { store.get(key) },
        write = { v -> store.set(key, v); apply(v.toIntOrNull() ?: C.WHITE) })

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

private fun action(label: String, run: () -> String?) =
    Setting("action.${label.lowercase().replace(' ', '_')}", label, SettingKind.ACTION, run = run, read = { "" })

private fun info(key: String, label: String, value: () -> String) =
    Setting(key, label, SettingKind.INFO, read = value)

private val FG_OPTS = listOf(opt("White", C.WHITE), opt("Green", C.GREEN), opt("Cyan", C.CYAN),
    opt("Amber", C.AMBER), opt("Purple", C.PURPLE), opt("Red", C.RED), opt("Blue", C.BLUE), opt("Mono", C.MONO))
private val BG_OPTS = listOf(opt("Ink", C.INK), opt("Slate", C.SLATE), opt("Navy", C.NAVY),
    opt("Black", C.BLACK), opt("Deep", C.DEEP), opt("Plum", C.PLUM))
private val CUR_OPTS = listOf(opt("Purple", C.ACCENT), opt("Cyan", C.CYAN), opt("Green", C.GREEN),
    opt("Amber", C.AMBER), opt("White", C.WHITE))

// -------- modules --------

class ThemeModule : XsetModule {
    override val id = "theme"; override val title = "Theme"; override val icon = XsetDesign.Icon.THEME
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        val presetOpts = XsetThemes.presets.map { Opt(it.label, it.id) }
        return listOf(
            enumSetting(s, "theme.preset", "Preset", presetOpts) { id ->
                val p = XsetThemes.byId(id)
                s.set("theme.fg", p.fg.toString()); s.set("theme.bg", p.bg.toString()); s.set("theme.cursor", p.cursor.toString())
                s.set("cursor.color", p.cursor.toString())
                ctx.applyTheme(p.fg, p.bg, p.cursor)
            },
            enumColor(s, "theme.fg", "Foreground", FG_OPTS) { ctx.applyForeground(it) },
            enumColor(s, "theme.bg", "Background", BG_OPTS) { ctx.applyBackground(it) },
            enumColor(s, "theme.cursor", "Cursor Color", CUR_OPTS) { ctx.applyCursorColor(it); s.set("cursor.color", it.toString()) },
            action("Reset Theme") {
                val p = XsetThemes.presets[0]
                s.set("theme.preset", p.id); s.set("theme.fg", p.fg.toString()); s.set("theme.bg", p.bg.toString())
                s.set("theme.cursor", p.cursor.toString()); s.set("cursor.color", p.cursor.toString())
                ctx.applyTheme(p.fg, p.bg, p.cursor); "Theme reset to ${p.label}"
            },
            info("theme.info", "Active", { XsetThemes.byId(s.get("theme.preset")).label }),
        )
    }
}

class FontModule : XsetModule {
    override val id = "font"; override val title = "Font"; override val icon = XsetDesign.Icon.FONT
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        return listOf(
            intSetting(s, "font.size", "Size (dp)", 7, 28, 1, "cell height") { ctx.applyFontSizeDp(it) },
            enumSetting(s, "font.family", "Family", listOf(Opt("JetBrains Mono", "jetbrains"), Opt("System Mono", "system"))) { ctx.applyFontFamily(it) },
            intSetting(s, "font.linespacing", "Line Spacing %", 90, 160, 5, "row height") { ctx.applyLineSpacing(it) },
            intSetting(s, "font.letterspacing", "Letter Spacing", 0, 120, 5, "tracking") { ctx.applyLetterSpacing(it) },
            toggle(s, "font.bold_bright", "Bold = Bright") { ctx.applyBoldBright(it) },
            intSetting(s, "layout.padding", "Padding %", 50, 200, 10, "content inset") { ctx.applyPaddingScale(it) },
            action("Reset Font") {
                s.set("font.size", "12"); s.set("font.family", "jetbrains"); s.set("font.linespacing", "100")
                s.set("font.letterspacing", "0"); s.set("font.bold_bright", "off"); s.set("layout.padding", "100")
                ctx.reapplyAll(); "Font reset"
            },
            info("font.note", "Renderer", { "damage-driven · dp-based" }),
        )
    }
}

class CursorModule : XsetModule {
    override val id = "cursor"; override val title = "Cursor"; override val icon = XsetDesign.Icon.CURSOR
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        return listOf(
            enumSetting(s, "cursor.style", "Style", listOf(Opt("Block", "block"), Opt("Bar", "bar"), Opt("Underline", "underline"), Opt("Hollow", "hollow"))) { ctx.applyCursorStyle(CursorStyle.from(it)) },
            toggle(s, "cursor.blink", "Blink") { ctx.applyCursorBlink(it) },
            enumColor(s, "cursor.color", "Color", CUR_OPTS) { ctx.applyCursorColor(it) },
            action("Reset Cursor") {
                s.set("cursor.style", "block"); s.set("cursor.blink", "off"); s.set("cursor.color", C.ACCENT.toString())
                ctx.applyCursorStyle(CursorStyle.BLOCK); ctx.applyCursorBlink(false); ctx.applyCursorColor(C.ACCENT); "Cursor reset"
            },
            info("cursor.note", "Shapes", { "block · bar · underline · hollow" }),
        )
    }
}

class AppearanceModule : XsetModule {
    override val id = "appearance"; override val title = "Appearance"; override val icon = XsetDesign.Icon.APPEARANCE
    override fun build(ctx: XsetContext): List<Setting> {
        // Curated overview pulling the highest-value knobs from the dedicated modules (same keys →
        // changing here changes everywhere, no duplicated state).
        val s = ctx.store
        val presetOpts = XsetThemes.presets.map { Opt(it.label, it.id) }
        return listOf(
            enumSetting(s, "theme.preset", "Theme", presetOpts) { id ->
                val p = XsetThemes.byId(id)
                s.set("theme.fg", p.fg.toString()); s.set("theme.bg", p.bg.toString()); s.set("theme.cursor", p.cursor.toString())
                s.set("cursor.color", p.cursor.toString()); ctx.applyTheme(p.fg, p.bg, p.cursor)
            },
            intSetting(s, "font.size", "Font Size", 7, 28, 1, "") { ctx.applyFontSizeDp(it) },
            enumSetting(s, "cursor.style", "Cursor", listOf(Opt("Block", "block"), Opt("Bar", "bar"), Opt("Underline", "underline"), Opt("Hollow", "hollow"))) { ctx.applyCursorStyle(CursorStyle.from(it)) },
            toggle(s, "cursor.blink", "Cursor Blink") { ctx.applyCursorBlink(it) },
            enumColor(s, "theme.fg", "Foreground", FG_OPTS) { ctx.applyForeground(it) },
            enumColor(s, "theme.bg", "Background", BG_OPTS) { ctx.applyBackground(it) },
            intSetting(s, "font.linespacing", "Line Spacing %", 90, 160, 5, "") { ctx.applyLineSpacing(it) },
            intSetting(s, "layout.padding", "Padding %", 50, 200, 10, "") { ctx.applyPaddingScale(it) },
            enumColor(s, "cursor.color", "Cursor Color", CUR_OPTS) { ctx.applyCursorColor(it) },
            toggle(s, "font.bold_bright", "Bold = Bright") { ctx.applyBoldBright(it) },
        )
    }
}

class BackgroundModule : XsetModule {
    override val id = "background"; override val title = "Background"; override val icon = XsetDesign.Icon.BACKGROUND
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        return listOf(
            enumColor(s, "theme.bg", "Color", BG_OPTS) { ctx.applyBackground(it) },
            enumColor(s, "theme.fg", "Contrast (fg)", FG_OPTS) { ctx.applyForeground(it) },
            action("Reset Background") { s.set("theme.bg", C.INK.toString()); ctx.applyBackground(C.INK); "Background reset" },
            info("bg.note", "Note", { "true window transparency: device-verify (roadmap)" }),
        )
    }
}

class PerformanceModule : XsetModule {
    override val id = "performance"; override val title = "Performance"; override val icon = XsetDesign.Icon.PERFORMANCE
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        return listOf(
            intSetting(s, "perf.scrollback", "Scrollback lines", 200, 20000, 200, "history depth") { ctx.applyScrollback(it) },
            info("perf.render", "Rendering", { "damage-driven (generation counter)" }),
            info("perf.frame", "Frame pacing", { "Choreographer vsync" }),
            info("perf.mem", "Buffer", { "deque scrollback · O(1) both ends" }),
        )
    }
}

class BackupModule : XsetModule {
    override val id = "backup"; override val title = "Backup"; override val icon = XsetDesign.Icon.BACKUP
    override fun build(ctx: XsetContext): List<Setting> {
        val s = ctx.store
        return listOf(
            action("Save Now") { ctx.status("saved"); "Configuration saved ✓" },
            action("Export to File") { val p = ctx.exportConfig(); if (p != null) "Exported → $p" else "Export failed" },
            action("Import from File") { if (ctx.importConfig()) { ctx.reapplyAll(); "Imported & applied ✓" } else "No import file found" },
            action("Reset ALL to defaults") { s.resetAll(); ctx.reapplyAll(); "All settings reset to defaults ✓" },
            info("backup.auto", "Auto-save", { "on (every change persists)" }),
            info("backup.count", "Keys", { s.snapshot().size.toString() }),
            info("backup.fmt", "Format", { "canonical JSON (portable)" }),
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
    override val id = "storage"; override val title = "Storage Access"; override val icon = XsetDesign.Icon.BACKUP
    override fun build(ctx: XsetContext): List<Setting> {
        return listOf(
            Setting(
                "storage.enabled", "Storage Access", SettingKind.TOGGLE,
                hint = "mount device storage at ~/sdcard",
                read = { if (ctx.storageEnabled()) "on" else "off" },
                write = { v -> if (v.equals("on", true)) ctx.enableStorage() else ctx.disableStorage() }
            ),
            info("storage.perm", "Permission", { ctx.storageStatus() }),
            info("storage.mount", "Mount point", { "~/sdcard (+ ~/sdcard-1 for a removable volume)" }),
            action("Grant / Rebuild") { ctx.enableStorage() },
            info("storage.dirs", "Contents", { "real volume root (DCIM · Download · Android · …)" }),
            info("storage.apply", "Applies", { "immediately on the active workspace (no restart)" }),
            info("storage.safe", "If denied", { "terminal keeps running (no crash)" }),
        )
    }
}

class AboutModule : XsetModule {
    override val id = "about"; override val title = "About"; override val icon = XsetDesign.Icon.ABOUT
    override fun build(ctx: XsetContext): List<Setting> {
        // System Information: read-only, device-derived (MainActivity.deviceInfo via appInfo()).
        val rows = ctx.appInfo().map { (k, v) -> info("about.$k", k, { v }) }.toMutableList()
        // Developer contact: the address plus what to send. Read-only, copyable info rows, no
        // No mail-client action here: the "Contact Us" action that sat between "Also"
        // and "Close" is gone). The address stays visible as an accurate, copyable value.
        rows.add(info("about.dev", "Contact information", { DEVELOPER_EMAIL }))
        rows.add(info("about.dev.use1", "Contact for", { "feedback · bug reports · features" }))
        rows.add(info("about.dev.use2", "Also", { "fixes · compatibility issues" }))
        rows.add(action("Close") { ctx.requestClose(); null })
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
        safeReg { AppearanceModule() }
        safeReg { ThemeModule() }
        safeReg { FontModule() }
        safeReg { CursorModule() }
        safeReg { BackgroundModule() }
        safeReg { PerformanceModule() }
        safeReg { StorageModule() }
        safeReg { BackupModule() }
        safeReg { AboutModule() }
        // Roadmap (guest-coupled; not shipped as placeholders):
        //   XsetRegistry.register(PromptModule())   // writes PS1 into the rootfs, device-verify
        //   XsetRegistry.register(ShellModule())    // selects login shell, device-verify
        //   XsetRegistry.register(PluginsModule())  // manages guest plugins, device-verify
    }
}
