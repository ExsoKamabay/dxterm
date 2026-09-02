package com.xdrac.xset

/**
 * drac-Xterm, Official Design System.
 *
 * SINGLE SOURCE OF TRUTH for every `xset` surface (settings dashboard today; Command Palette /
 * AI panel / Session manager tomorrow). All chrome colors, semantic state colors, spacing,
 * typography, border glyphs, and Unicode icons are defined here exactly ONCE.
 *
 * Contract:
 *  - `Palette` (controller chrome) and `C` (theme-preset palette) are thin FACADES over these
 *    tokens. They keep their historical symbol names for source stability but must resolve to the
 *    identical ARGB defined here, so centralization causes zero visual regression.
 *  - Every glyph in [Icon] is a single-width BMP code point (audited in one place) to avoid grid
 *    misalignment on the device font (JetBrains Mono).
 *  - New surfaces read tokens from here; they never invent ad-hoc colors.
 *
 * All colors are 0xAARRGGBB (opaque). Values are frozen; changing one is a visual API change.
 */
object XsetDesign {

    // ---- Brand ----
    const val ACCENT      = 0xFF8A5CF6.toInt()   // primary purple (focus, accents)
    const val ACCENT_SOFT = 0xFFB794F6.toInt()   // lavender (titles, highlights)

    // ---- Surface (backgrounds & separators) ----
    object Surface {
        const val BG          = 0xFF0A0A0C.toInt()   // app/terminal background
        const val PANEL_LEFT  = 0xFF121218.toInt()   // left (module list) panel
        const val PANEL_RIGHT = 0xFF141419.toInt()   // right (settings) panel
        const val BORDER      = 0xFF2A2A38.toInt()   // box borders / dividers
    }

    // ---- Text ----
    object Text {
        const val PRIMARY = 0xFFE6E6E6.toInt()   // default foreground
        const val DIM     = 0xFF6B7280.toInt()   // secondary / hints
        const val TITLE   = 0xFFB794F6.toInt()   // headers
        const val ACCENT  = 0xFF8A5CF6.toInt()   // emphasized
    }

    // ---- Semantic state (focus / selection / disabled / status) ----
    // Every surface reads state from these tokens, never from a raw colour, so a theme change
    // reaches the whole dashboard at once.
    object State {
        const val SEL_BG_FOCUS = 0xFF241B45.toInt()  // selected row, panel has focus
        const val SEL_BG_BLUR  = 0xFF17161F.toInt()  // selected row, panel does NOT have focus
        const val SEL_FG       = 0xFFFFFFFF.toInt()   // selected row foreground
        const val DISABLED     = 0xFF6B7280.toInt()   // non-interactive / greyed
        const val INFO         = 0xFF22D3EE.toInt()   // cyan
        const val SUCCESS      = 0xFF3DDC84.toInt()   // green
        const val WARNING      = 0xFFF5A524.toInt()   // amber
        const val ERROR        = 0xFFFF6B6B.toInt()   // red
    }

    // ---- Terminal ANSI reference (canonical 16-color names for preset authors) ----
    // These name the standard slots; theme presets may reference them for consistency. They do not
    // rewire the C++ engine's own ANSI table (that lives in the engine/theme path).
    object Ansi {
        const val BLACK   = 0xFF000000.toInt(); const val RED     = 0xFFFF6B6B.toInt()
        const val GREEN   = 0xFF3DDC84.toInt(); const val YELLOW  = 0xFFF5A524.toInt()
        const val BLUE    = 0xFF5B9CFF.toInt(); const val MAGENTA = 0xFF8A5CF6.toInt()
        const val CYAN    = 0xFF22D3EE.toInt(); const val WHITE   = 0xFFC7C7CC.toInt()
        const val B_BLACK = 0xFF6B7280.toInt(); const val B_RED   = 0xFFFF8A8A.toInt()
        const val B_GREEN = 0xFF6BE9A6.toInt(); const val B_YELLOW= 0xFFFFC85C.toInt()
        const val B_BLUE  = 0xFF8CBAFF.toInt(); const val B_MAGENTA=0xFFB794F6.toInt()
        const val B_CYAN  = 0xFF67E8F9.toInt(); const val B_WHITE = 0xFFFFFFFF.toInt()
    }

    // ---- Preset background shades (extra dark surfaces used only by built-in theme presets) ----
    object Preset {
        const val SLATE = 0xFF12141A.toInt()
        const val NAVY  = 0xFF0B1020.toInt()
        const val DEEP  = 0xFF101014.toInt()
        const val PLUM  = 0xFF14101C.toInt()
    }

    // ---- Spacing / layout ratios (cells) ----
    object Spacing {
        const val PANEL_PAD = 1     // inner padding inside a panel
        const val GUTTER    = 1     // gap between icon and label
        const val LEFT_PCT  = 28    // left panel = 28% of cols ...
        const val LEFT_MIN  = 14    // ... clamped to [14, 26]
        const val LEFT_MAX  = 26
        const val MIN_COLS  = 30    // below this, render the "too small" notice
        const val MIN_ROWS  = 10
    }

    // ---- Typography (attr bit layout shared with the renderer's grid format) ----
    object Type {
        const val A_BOLD        = 1
        const val A_UNDERLINE   = 2
        const val FAMILY_MONO   = "jetbrains"
        const val FAMILY_SYSTEM = "system"
    }

    // ---- Border glyph sets ----
    class BorderSet(
        val tl: Char, val tr: Char, val bl: Char, val br: Char,
        val h: Char, val v: Char,
        val tDown: Char, val tUp: Char, val teeL: Char, val teeR: Char, val cross: Char,
    )
    object Border {
        val ROUNDED = BorderSet('╭', '╮', '╰', '╯', '─', '│', '┬', '┴', '┤', '├', '┼')
        val SHARP   = BorderSet('┌', '┐', '└', '┘', '─', '│', '┬', '┴', '┤', '├', '┼')
        val DOUBLE  = BorderSet('╔', '╗', '╚', '╝', '═', '║', '╦', '╩', '╣', '╠', '╬')
        val HEAVY   = BorderSet('┏', '┓', '┗', '┛', '━', '┃', '┳', '┻', '┫', '┣', '╋')
    }

    // ---- Icon registry (the one place all Unicode glyphs are declared & width-audited) ----
    object Icon {
        // module icons (left panel taxonomy)
        const val APPEARANCE  = "❖"
        const val THEME       = "◈"
        const val FONT        = "A"
        const val CURSOR      = "▉"
        const val BACKGROUND  = "▚"
        const val PERFORMANCE = "↯"
        const val BACKUP      = "↧"
        const val ABOUT       = "ⓘ"
        // affordance / status glyphs
        const val OK     = "✓"
        const val WARN   = "⚠"
        const val ERR    = "✗"
        const val INFO   = "ⓘ"
        const val ACTION = "▶"
        const val SEL    = "▸"
        const val ADJ_L  = "‹"
        const val ADJ_R  = "›"
    }
}

/**
 * Severity of a status/result line. Maps to a design-system color + icon so every surface reports
 * INFO/SUCCESS/WARNING/ERROR identically. Used by module ACTION results and by error recovery.
 */
enum class StatusKind(val color: Int, val icon: String) {
    INFO(XsetDesign.State.INFO, XsetDesign.Icon.INFO),
    SUCCESS(XsetDesign.State.SUCCESS, XsetDesign.Icon.OK),
    WARNING(XsetDesign.State.WARNING, XsetDesign.Icon.WARN),
    ERROR(XsetDesign.State.ERROR, XsetDesign.Icon.ERR),
}
