package com.dracxterm.xset

/** Developer contact address surfaced by the About screen and the mail intent. Single source of
 *  truth so the displayed address and the intent target can never drift. */
const val DEVELOPER_EMAIL = "lexyong66@gmail.com"

/**
 * drac-Xterm, internal Terminal Configuration Framework (`xset`).
 *
 * Design goals (see /truth mission):
 *  - Registry/module architecture: a new configuration menu is added by REGISTERING an
 *    [XsetModule]; the engine (controller/renderer/store) is never edited to add a menu.
 *  - Dependency inversion: modules depend only on [XsetContext] (an app-provided bridge),
 *    never on TerminalView/MainActivity directly -> modules are unit-testable in isolation.
 *  - Pure, portable value model (String-canonical) -> host-verifiable round-trip + navigation.
 *
 * Nothing here touches the stable subsystems (Scroll/Zoom/Shortcut/Compatibility Engine); the
 * controller only reads/writes render knobs and session config through the context.
 */

/** Cursor shapes the renderer can draw. BLOCK is the pre-existing default (zero behaviour change). */
enum class CursorStyle { BLOCK, BAR, UNDERLINE, HOLLOW;
    companion object { fun from(s: String) = values().firstOrNull { it.name.equals(s, true) } ?: BLOCK }
}

/** The kinds of setting rows the controller knows how to render and adjust. */
enum class SettingKind { ENUM, TOGGLE, INT, ACTION, INFO }

/** One selectable option for an ENUM setting (display label + canonical value). */
data class Opt(val label: String, val value: String)

/**
 * A single configuration row. Flat by design (not a sealed tree) so it is trivially mirrored and
 * host-verified. [read] returns the current canonical value; [write] applies+persists a new value;
 * [run] executes an ACTION and returns an optional status line.
 */
class Setting(
    val key: String,
    val label: String,
    val kind: SettingKind,
    val options: List<Opt> = emptyList(),
    val min: Int = 0,
    val max: Int = 0,
    val step: Int = 1,
    val hint: String = "",
    val read: () -> String,
    val write: ((String) -> Unit)? = null,
    val run: (() -> String?)? = null,
    /** How an ENUM value that matches none of [options] is shown, e.g. a colour set by a preset. */
    val format: ((String) -> String)? = null,
) {
    /** Display string for the current value (right column). */
    fun display(): String = when (kind) {
        SettingKind.TOGGLE -> if (read().equals("on", true) || read() == "1" || read().equals("true", true)) "ON" else "OFF"
        SettingKind.ENUM   -> options.firstOrNull { it.value == read() }?.label ?: format?.invoke(read()) ?: read()
        SettingKind.ACTION -> "▶"
        else               -> read()
    }

    /** ENUM index of the current value, or -1. */
    private fun enumIndex(): Int = options.indexOfFirst { it.value == read() }

    /** Advance the value by [dir] (+1/-1). Returns an optional status line (from ACTION). */
    fun adjust(dir: Int): String? {
        when (kind) {
            SettingKind.TOGGLE -> write?.invoke(if (display() == "ON") "off" else "on")
            SettingKind.ENUM -> if (options.isNotEmpty()) {
                val n = options.size
                val i = enumIndex().let { if (it < 0) 0 else it }
                val ni = ((i + dir) % n + n) % n
                write?.invoke(options[ni].value)
            }
            SettingKind.INT -> {
                val cur = read().toIntOrNull() ?: min
                val nv = (cur + dir * step).coerceIn(min, max)
                write?.invoke(nv.toString())
            }
            SettingKind.ACTION -> return run?.invoke()
            SettingKind.INFO -> {}
        }
        return null
    }

    /** Enter/activate: ENUM/INT advance forward, TOGGLE flips, ACTION runs. */
    fun activate(): String? = when (kind) {
        SettingKind.ACTION -> run?.invoke()
        SettingKind.INFO -> null
        else -> adjust(1)
    }

    val adjustable: Boolean get() = kind == SettingKind.ENUM || kind == SettingKind.TOGGLE || kind == SettingKind.INT
}

/** A configuration menu. Implementations only need an id/title/icon and to build rows from context. */
interface XsetModule {
    val id: String
    val title: String
    val icon: String
    fun build(ctx: XsetContext): List<Setting>
}

/**
 * The module registry. Adding a new `xset <name>` menu is one call: `XsetRegistry.register(MyModule())`.
 * Insertion order is preserved (LinkedHashMap) so the left panel order is deterministic.
 */
object XsetRegistry {
    private val mods = LinkedHashMap<String, XsetModule>()
    fun register(m: XsetModule) { mods[m.id] = m }
    fun unregister(id: String) { mods.remove(id) }
    fun modules(): List<XsetModule> = mods.values.toList()
    fun byId(id: String): XsetModule? = mods[id]
    fun index(id: String): Int = modules().indexOfFirst { it.id == id }
    fun clear() { mods.clear() }
}

/**
 * A full-screen input+render surface that TerminalView can host as an overlay (the PTY is idle while
 * one is [active]). The settings dashboard ([XsetController]) is the first implementation; future
 * surfaces (Command Palette, AI panel, Session/Remote manager) implement the same contract so they
 * plug into the existing input-interception + shared `drawGrid` path with no engine change.
 *
 * Migration note: `TerminalView.dashboard` is still typed `XsetController?` today. To host multiple
 * surfaces, retype it to `XsetSurface?` and add a small router that picks the active one, a purely
 * additive change enabled by this interface. Deferred until a second surface actually ships (YAGNI).
 */
interface XsetSurface {
    /** True while this surface owns input and the screen. */
    val active: Boolean

    /**
     * True while the surface has a text field taking input (the search box).
     *
     * Paste asks before delivering. Typed text arrives one key at a time and the user sees the
     * result of each; a paste is a single gesture carrying arbitrary content, and feeding it
     * through [onChar] would fire one hotkey per character -- an 'l' in the clipboard adjusts
     * whatever row is selected. So paste is delivered only where it means "text", and dropped
     * everywhere else.
     */
    val acceptsText: Boolean get() = false
    /** Handle a named special key (e.g. "UP", "ENTER", "ESC"). Returns true if consumed. */
    fun onSpecial(name: String): Boolean
    /** Handle a character key with modifier flags. Returns true if consumed. */
    fun onChar(c: Char, ctrl: Boolean, alt: Boolean): Boolean
    /** Produce the styled grid for the given viewport, painted by the shared renderer. */
    fun render(cols: Int, rows: Int): TuiCanvas
    /** Dismiss the surface (return control to the terminal). */
    fun close()
}

/**
 * Typed, persisted key→value store (canonical String values). Backed by any load/save pair
 * (SharedPreferences on device; an in-memory map in host tests). Serialises to a CANONICAL JSON
 * object (keys sorted, string values, minimal escaping) so export/import round-trips byte-for-byte
 * across the Kotlin app and the host reference implementation.
 */
class XsetStore(
    private val loader: () -> Map<String, String>,
    private val saver: (Map<String, String>) -> Unit,
) {
    private val defaults = LinkedHashMap<String, String>()
    private val values = LinkedHashMap<String, String>()

    init { values.putAll(loader()) }

    /** Declare a default; does not overwrite a persisted value. */
    fun def(key: String, value: String): XsetStore {
        defaults[key] = value
        if (!values.containsKey(key)) values[key] = value
        return this
    }

    fun get(key: String): String = values[key] ?: defaults[key] ?: ""
    fun getInt(key: String, fallback: Int = 0): Int = get(key).toIntOrNull() ?: fallback
    fun getBool(key: String): Boolean = get(key).let { it.equals("on", true) || it == "1" || it.equals("true", true) }

    fun set(key: String, value: String) { values[key] = value; persist() }

    /** Reset every known default key to its default (does not drop unknown keys). */
    fun resetAll() { for ((k, v) in defaults) values[k] = v; persist() }

    private fun persist() = saver(LinkedHashMap(values))

    /** Canonical JSON of all known-default keys (stable ordering) for export. */
    fun toJson(): String {
        val keys = defaults.keys.sorted()
        val sb = StringBuilder("{")
        keys.forEachIndexed { i, k ->
            if (i > 0) sb.append(',')
            sb.append('"').append(esc(k)).append("\":\"").append(esc(get(k))).append('"')
        }
        return sb.append('}').toString()
    }

    /** Merge values from a canonical JSON export. Unknown keys are ignored (forward-compatible). */
    fun fromJson(json: String): Boolean {
        val map = parse(json) ?: return false
        var changed = false
        for ((k, v) in map) if (defaults.containsKey(k)) { values[k] = v; changed = true }
        if (changed) persist()
        return changed
    }

    fun snapshot(): Map<String, String> = LinkedHashMap(values)

    companion object {
        private fun esc(s: String): String {
            val sb = StringBuilder()
            for (ch in s) when (ch) {
                '"' -> sb.append("\\\"")
                '\\' -> sb.append("\\\\")
                '\n' -> sb.append("\\n")
                '\t' -> sb.append("\\t")
                '\r' -> sb.append("\\r")
                else -> sb.append(ch)
            }
            return sb.toString()
        }

        /** Minimal object-of-strings parser (no nesting) matching [toJson]. Returns null on malformed input. */
        fun parse(json: String): Map<String, String>? {
            val out = LinkedHashMap<String, String>()
            val s = json.trim()
            if (s.length < 2 || s[0] != '{' || s[s.length - 1] != '}') return null
            var i = 1
            val n = s.length - 1
            fun ws() { while (i < n && s[i].isWhitespace()) i++ }
            fun str(): String? {
                if (i >= n || s[i] != '"') return null
                i++
                val sb = StringBuilder()
                while (i < n) {
                    val c = s[i++]
                    if (c == '\\' && i < n) {
                        when (s[i++]) {
                            '"' -> sb.append('"'); '\\' -> sb.append('\\'); 'n' -> sb.append('\n')
                            't' -> sb.append('\t'); 'r' -> sb.append('\r'); else -> return null
                        }
                    } else if (c == '"') return sb.toString() else sb.append(c)
                }
                return null
            }
            ws()
            if (i >= n) return out    // empty object {}
            while (i < n) {
                ws(); val k = str() ?: return null
                ws(); if (i >= n || s[i] != ':') return null; i++
                ws(); val v = str() ?: return null
                out[k] = v
                ws(); if (i < n && s[i] == ',') { i++; continue }
                break
            }
            return out
        }
    }
}

/**
 * The bridge the app implements so modules can apply changes live without knowing about the view.
 * Every apply* method both mutates the running renderer/session AND is expected to be persisted by
 * the caller (modules call store.set + the matching apply*). Keeping this an interface is what makes
 * the framework testable off-device.
 */
interface XsetContext {
    val store: XsetStore

    // ---- live render knobs (app-side, no restart) ----
    fun applyFontSizeDp(dp: Int)
    fun applyFontFamily(id: String)        // "jetbrains" | "system"
    fun applyLineSpacing(pct: Int)         // 90..160 (% of natural line height)
    fun applyLetterSpacing(milli: Int)     // 0..120 (em/1000)
    fun applyCursorStyle(style: CursorStyle)
    fun applyCursorBlink(on: Boolean)
    fun applyCursorColor(argb: Int)
    fun applyPaddingScale(pct: Int)        // 50..200 (% of natural inset)
    fun applyBoldBright(on: Boolean)

    // ---- theme/session knobs (through the existing configure() path) ----
    fun applyTheme(fg: Int, bg: Int, cursor: Int)
    fun applyForeground(argb: Int)
    fun applyBackground(argb: Int)
    fun applyScrollback(lines: Int)

    // ---- storage access (Settings ▸ Storage Access) ----
    /** Persisted opt-in state. */
    fun storageEnabled(): Boolean
    /** One-line permission/state description for the screen. */
    fun storageStatus(): String
    /** Enable the feature: request the OS permission for this Android version and expose ~/sdcard on
     *  the RUNNING shell (no respawn). Returns a status line. Denial is safe (the terminal keeps
     *  running). */
    fun enableStorage(): String
    /** Disable the feature: remove ~/sdcard[/-1] from the running shell immediately (same session). */
    fun disableStorage(): String

    // ---- guest runtime ----
    /** Which backend runs the Linux guest and why, e.g. "vhdp — self-test passed …". */
    fun guestBackend(): String = "unknown"

    // ---- misc ----
    fun appInfo(): List<Pair<String, String>>
    fun status(msg: String)
    fun reapplyAll()                       // push every persisted knob to the live renderer/session
    fun exportConfig(): String?            // returns path on success
    fun importConfig(): Boolean
    fun requestClose()

    /** Open the developer-contact channel from the About screen and return a status line for the
     *  dashboard. The default just reports the address (so any implementer stays valid and the
     *  address is still shown); the app overrides this to launch a mail intent and fall back to
     *  copying the address to the clipboard, it never crashes or force-closes the terminal. */
    fun contactDeveloper(): String = DEVELOPER_EMAIL
}
