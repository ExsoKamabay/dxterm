package com.dracxterm.xset

/**
 * Display width of a code point for the fixed cell grid: 2 for East-Asian wide / fullwidth / emoji,
 * otherwise 1. Combining/zero-width marks are treated as 1 (their own cell) so every code point stays
 * visible and the column maths line up; CJK getting its true width of 2 is what stops the overlap seen
 * when the UI language is Chinese. Mirrors the native engine's wcwidth so xset and the terminal agree.
 */
object XsetWidth {
    fun of(cp: Int): Int {
        if ((cp in 0x1100..0x115F) || cp == 0x2329 || cp == 0x232A ||
            (cp in 0x2E80..0x303E) || (cp in 0x3041..0x33FF) || (cp in 0x3400..0x4DBF) ||
            (cp in 0x4E00..0x9FFF) || (cp in 0xA000..0xA4CF) || (cp in 0xA960..0xA97F) ||
            (cp in 0xAC00..0xD7A3) || (cp in 0xF900..0xFAFF) || (cp in 0xFE10..0xFE19) ||
            (cp in 0xFE30..0xFE6F) || (cp in 0xFF00..0xFF60) || (cp in 0xFFE0..0xFFE6) ||
            (cp in 0x1F300..0x1FAFF) || (cp in 0x20000..0x3FFFD)
        ) return 2
        return 1
    }

    /** Total display columns [s] occupies. */
    fun width(s: String): Int {
        var w = 0; var i = 0
        while (i < s.length) { val cp = s.codePointAt(i); i += Character.charCount(cp); w += of(cp) }
        return w
    }

    /** Truncate [s] to at most [maxCols] DISPLAY columns, appending '…' if it was cut. */
    fun fit(s: String, maxCols: Int): String {
        if (maxCols <= 0) return ""
        if (width(s) <= maxCols) return s
        if (maxCols == 1) return "…"
        val sb = StringBuilder(); var used = 0; var i = 0
        while (i < s.length) {
            val cp = s.codePointAt(i); val cw = of(cp)
            if (used + cw > maxCols - 1) break        // leave one column for the ellipsis
            sb.appendCodePoint(cp); used += cw; i += Character.charCount(cp)
        }
        return sb.append('…').toString()
    }
}

/**
 * A styled character grid the dashboard paints into, then hands to TerminalView's shared `drawGrid`.
 * Layout matches the terminal snapshot format exactly (parallel glyph/fg/bg/attr Int arrays) so the
 * same renderer draws both, maximal reuse, guaranteed visual consistency, zero duplicate draw code.
 *
 * Colours are ARGB. Attr bits reuse the renderer's own bit layout (A_BOLD=1, A_UNDERLINE=2, …), plus
 * ATTR_WIDE/ATTR_WIDE_TAIL (256/512) which TerminalView.drawGrid already honours: a wide head is drawn
 * two cells wide and the tail cell is skipped.
 */
class TuiCanvas(val cols: Int, val rows: Int, private val defFg: Int, private val defBg: Int) {
    companion object {
        const val ATTR_WIDE = 256        // glyph spans two cells (matches TerminalView.drawGrid)
        const val ATTR_WIDE_TAIL = 512   // right half of a wide glyph; drawGrid skips it
        const val ATTR_COMBINING = 1024  // cell carries a grapheme cluster (base + marks) in [graphemes]
    }
    val glyphs = IntArray(cols * rows) { ' '.code }
    val fg = IntArray(cols * rows) { defFg }
    val bg = IntArray(cols * rows) { defBg }
    val attr = IntArray(cols * rows)
    // Per-cell grapheme cluster for complex scripts (e.g. Devanagari base + matra). Only set when a
    // cell's text is more than one code point; drawGrid shapes the whole string so marks stay attached.
    private val graphemes = arrayOfNulls<String>(cols * rows)

    private fun inb(r: Int, c: Int) = r in 0 until rows && c in 0 until cols

    /** The grapheme cluster to draw at (r,c), or null to draw the single [glyphs] code point. */
    fun graphemeAt(r: Int, c: Int): String? = if (inb(r, c)) graphemes[r * cols + c] else null

    fun clear(bgc: Int = defBg) {
        for (i in glyphs.indices) { glyphs[i] = ' '.code; fg[i] = defFg; bg[i] = bgc; attr[i] = 0; graphemes[i] = null }
    }

    fun put(r: Int, c: Int, ch: Int, f: Int = defFg, b: Int = defBg, a: Int = 0) {
        if (!inb(r, c)) return
        val i = r * cols + c; glyphs[i] = ch; fg[i] = f; bg[i] = b; attr[i] = a; graphemes[i] = null
    }

    /** Write [s] starting at (r,c), clipped to the row. Returns the column after the last glyph.
     *  Wide (2-cell) code points occupy two columns: the head is marked [ATTR_WIDE] and the follow
     *  cell [ATTR_WIDE_TAIL], so TerminalView.drawGrid draws the glyph two cells wide and skips the
     *  tail — no overlap when the UI language uses wide (e.g. Chinese) glyphs. */
    fun text(r: Int, c: Int, s: String, f: Int = defFg, b: Int = defBg, a: Int = 0): Int {
        var x = c
        val bi = java.text.BreakIterator.getCharacterInstance()
        bi.setText(s)
        var start = bi.first()
        var end = bi.next()
        while (end != java.text.BreakIterator.DONE && x < cols) {
            val cluster = s.substring(start, end)
            start = end; end = bi.next()
            val cp = cluster.codePointAt(0)
            // A cluster longer than its first code point carries combining marks / ZWJ: keep the whole
            // string so the font shapes it (Devanagari matras stay on the base), else draw the code point.
            val combining = cluster.length > Character.charCount(cp)
            val w = XsetWidth.of(cp)
            if (w == 2) {
                if (x + 1 >= cols) { put(r, x, ' '.code, f, b, a); x++; break }  // no room for a wide glyph
                put(r, x, cp, f, b, a or ATTR_WIDE or (if (combining) ATTR_COMBINING else 0))
                if (combining) graphemes[r * cols + x] = cluster
                put(r, x + 1, ' '.code, f, b, ATTR_WIDE_TAIL)
                x += 2
            } else {
                put(r, x, cp, f, b, a or (if (combining) ATTR_COMBINING else 0))
                if (combining) graphemes[r * cols + x] = cluster
                x++
            }
        }
        return x
    }

    /** Write [s] right-aligned so it ENDS at column [endCol] (inclusive-exclusive end). */
    fun textRight(r: Int, endCol: Int, s: String, f: Int = defFg, b: Int = defBg, a: Int = 0) {
        val w = XsetWidth.width(s)
        text(r, (endCol - w).coerceAtLeast(0), s, f, b, a)
    }

    fun fillRect(r0: Int, c0: Int, r1: Int, c1: Int, b: Int) {
        for (r in r0..r1) for (c in c0..c1) if (inb(r, c)) { val i = r * cols + c; glyphs[i] = ' '.code; bg[i] = b; attr[i] = 0 }
    }

    fun hline(r: Int, c0: Int, c1: Int, ch: Int, f: Int, b: Int) { for (c in c0..c1) put(r, c, ch, f, b) }
    fun vline(c: Int, r0: Int, r1: Int, ch: Int, f: Int, b: Int) { for (r in r0..r1) put(r, c, ch, f, b) }

    /** Rounded box border between (r0,c0) and (r1,c1) inclusive. */
    /** Draw a box using [bs] (default = the design-system ROUNDED set). Glyphs come from one source. */
    fun box(r0: Int, c0: Int, r1: Int, c1: Int, f: Int, b: Int, bs: XsetDesign.BorderSet = XsetDesign.Border.ROUNDED) {
        hline(r0, c0 + 1, c1 - 1, bs.h.code, f, b); hline(r1, c0 + 1, c1 - 1, bs.h.code, f, b)
        vline(c0, r0 + 1, r1 - 1, bs.v.code, f, b); vline(c1, r0 + 1, r1 - 1, bs.v.code, f, b)
        put(r0, c0, bs.tl.code, f, b); put(r0, c1, bs.tr.code, f, b)
        put(r1, c0, bs.bl.code, f, b); put(r1, c1, bs.br.code, f, b)
    }

    /** A vertical T-junction character at a border row where an inner divider meets the frame. */
    fun tDown(r: Int, c: Int, f: Int, b: Int) = put(r, c, '┬'.code, f, b)
    fun tUp(r: Int, c: Int, f: Int, b: Int) = put(r, c, '┴'.code, f, b)
    fun teeLeft(r: Int, c: Int, f: Int, b: Int) = put(r, c, '┤'.code, f, b)
    fun teeRight(r: Int, c: Int, f: Int, b: Int) = put(r, c, '├'.code, f, b)
    fun cross(r: Int, c: Int, f: Int, b: Int) = put(r, c, '┼'.code, f, b)

    /** Debug/host: render the glyph plane to text (one line per row). */
    fun asText(): String {
        val sb = StringBuilder()
        for (r in 0 until rows) {
            for (c in 0 until cols) sb.appendCodePoint(glyphs[r * cols + c])
            if (r < rows - 1) sb.append('\n')
        }
        return sb.toString()
    }
}
