package com.dracxterm.xset

/**
 * A styled character grid the dashboard paints into, then hands to TerminalView's shared `drawGrid`.
 * Layout matches the terminal snapshot format exactly (parallel glyph/fg/bg/attr Int arrays) so the
 * same renderer draws both, maximal reuse, guaranteed visual consistency, zero duplicate draw code.
 *
 * Colours are ARGB. Attr bits reuse the renderer's own bit layout (A_BOLD=1, A_UNDERLINE=2, ...).
 */
class TuiCanvas(val cols: Int, val rows: Int, private val defFg: Int, private val defBg: Int) {
    val glyphs = IntArray(cols * rows) { ' '.code }
    val fg = IntArray(cols * rows) { defFg }
    val bg = IntArray(cols * rows) { defBg }
    val attr = IntArray(cols * rows)

    private fun inb(r: Int, c: Int) = r in 0 until rows && c in 0 until cols

    fun clear(bgc: Int = defBg) {
        for (i in glyphs.indices) { glyphs[i] = ' '.code; fg[i] = defFg; bg[i] = bgc; attr[i] = 0 }
    }

    fun put(r: Int, c: Int, ch: Int, f: Int = defFg, b: Int = defBg, a: Int = 0) {
        if (!inb(r, c)) return
        val i = r * cols + c; glyphs[i] = ch; fg[i] = f; bg[i] = b; attr[i] = a
    }

    /** Write [s] starting at (r,c), clipped to the row. Returns the column after the last glyph. */
    fun text(r: Int, c: Int, s: String, f: Int = defFg, b: Int = defBg, a: Int = 0): Int {
        var x = c
        var idx = 0
        while (idx < s.length && x < cols) {
            val cp = s.codePointAt(idx)
            put(r, x, cp, f, b, a)
            idx += Character.charCount(cp); x++
        }
        return x
    }

    /** Write [s] right-aligned so it ENDS at column [endCol] (inclusive-exclusive end). */
    fun textRight(r: Int, endCol: Int, s: String, f: Int = defFg, b: Int = defBg, a: Int = 0) {
        val w = s.codePointCount(0, s.length)
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
