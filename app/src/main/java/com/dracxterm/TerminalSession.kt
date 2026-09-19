package com.dracxterm

import android.content.Context

/** Kotlin-side owner of one native session handle. */
class TerminalSession(private val ctx: Context) {

    private var handle: Long = 0L
    var cols = 80; private set
    var rows = 24; private set

    fun start(cols: Int, rows: Int): Boolean {
        this.cols = cols; this.rows = rows
        val spec = Bootstrap.prepare(ctx)
        handle = NativeTerminal.nativeCreate(cols, rows, spec.argv, spec.env, spec.cwd)
        return handle != 0L
    }

    fun isValid() = handle != 0L
    fun running() = handle != 0L && NativeTerminal.nativeRunning(handle)
    fun generation() = if (handle != 0L) NativeTerminal.nativeGeneration(handle) else 0L

    fun write(bytes: ByteArray) { if (handle != 0L) NativeTerminal.nativeWrite(handle, bytes) }
    fun writeText(s: String) = write(s.toByteArray(Charsets.UTF_8))

    fun resize(cols: Int, rows: Int) {
        if (handle == 0L || (cols == this.cols && rows == this.rows)) return
        this.cols = cols; this.rows = rows
        NativeTerminal.nativeResize(handle, cols, rows)
    }

    fun snapshot(glyphs: IntArray, fg: IntArray, bg: IntArray, attr: IntArray, meta: IntArray): Int =
        if (handle != 0L) NativeTerminal.nativeSnapshot(handle, glyphs, fg, bg, attr, meta) else -1

    // ---- viewport ----
    fun scroll(deltaLines: Int) { if (handle != 0L) NativeTerminal.nativeScroll(handle, deltaLines) }
    fun scrollToBottom() { if (handle != 0L) NativeTerminal.nativeScrollToBottom(handle) }

    // ---- selection ----
    fun selectStart(row: Int, col: Int) { if (handle != 0L) NativeTerminal.nativeSelectStart(handle, row, col) }
    fun selectExtend(row: Int, col: Int) { if (handle != 0L) NativeTerminal.nativeSelectExtend(handle, row, col) }
    fun selectWord(row: Int, col: Int) { if (handle != 0L) NativeTerminal.nativeSelectWord(handle, row, col) }
    fun clearSelection() { if (handle != 0L) NativeTerminal.nativeClearSelection(handle) }
    fun selectionText(): String = if (handle != 0L) NativeTerminal.nativeSelectionText(handle) else ""

    // ---- search ----
    fun search(query: String, fromRow: Int, forward: Boolean): Int =
        if (handle != 0L) NativeTerminal.nativeSearch(handle, query, fromRow, forward) else -1

    // ---- mouse / paste ----
    fun mouse(button: Int, col: Int, row: Int, type: Int) {
        if (handle != 0L) NativeTerminal.nativeMouse(handle, button, col, row, type)
    }
    fun paste(text: String) {
        if (handle == 0L) return
        write(NativeTerminal.nativeWrapPaste(handle, text))
    }

    // ---- configuration / theme ----
    fun configure(fg: Int, bg: Int, cursor: Int, scrollback: Int) {
        if (handle != 0L) NativeTerminal.nativeConfigure(handle, fg, bg, cursor, scrollback)
    }
    fun cursorColor(): Int = if (handle != 0L) NativeTerminal.nativeCursorColor(handle) else 0
    fun cellGrapheme(row: Int, col: Int): String =
        if (handle != 0L) NativeTerminal.nativeCellGrapheme(handle, row, col) else ""

    // ---- out-of-band state ----
    fun title(): String = if (handle != 0L) NativeTerminal.nativeTitle(handle) else ""
    fun clipboard(): String = if (handle != 0L) NativeTerminal.nativeClipboard(handle) else ""
    fun appControl(): String = if (handle != 0L) NativeTerminal.nativeAppControl(handle) else ""

    fun close() { if (handle != 0L) { NativeTerminal.nativeDestroy(handle); handle = 0L } }
}

/** Byte encodings for special keys and modifier transforms. */
object TermKeys {
    private const val ESC = "\u001B"

    /** Cursor/nav keys with a modifier applied, in xterm's `CSI 1 ; mod FINAL` form (arrows,
     *  Home, End) or `CSI num ; mod ~` form (PgUp/PgDn/Del). Returns null for keys that have no
     *  standard modified encoding (ESC, TAB, ENTER, BKSP), so the caller falls back to plain. */
    fun modified(name: String, mod: Int): ByteArray? {
        val fin = when (name) {
            "UP" -> 'A'; "DOWN" -> 'B'; "RIGHT" -> 'C'; "LEFT" -> 'D'
            "HOME" -> 'H'; "END" -> 'F'; else -> null
        }
        if (fin != null) return "$ESC[1;$mod$fin".toByteArray(Charsets.UTF_8)
        val num = when (name) { "PGUP" -> 5; "PGDN" -> 6; "DEL" -> 3; else -> return null }
        return "$ESC[$num;$mod~".toByteArray(Charsets.UTF_8)
    }

    /** Arrow / home / end keys, honouring DECCKM application-cursor-key mode. */
    fun arrow(name: String, app: Boolean): ByteArray? {
        val fin = when (name) {
            "UP" -> 'A'; "DOWN" -> 'B'; "RIGHT" -> 'C'; "LEFT" -> 'D'
            "HOME" -> 'H'; "END" -> 'F'; else -> return null
        }
        val prefix = if (app) "${ESC}O" else "${ESC}["
        return "$prefix$fin".toByteArray(Charsets.UTF_8)
    }

    fun special(name: String): ByteArray? = when (name) {
        "ESC"   -> ESC
        "TAB"   -> "\t"
        "ENTER" -> "\r"
        "BKSP"  -> "\u007F"
        "PGUP"  -> "${ESC}[5~"
        "PGDN"  -> "${ESC}[6~"
        "DEL"   -> "${ESC}[3~"
        else    -> null
    }?.toByteArray(Charsets.UTF_8)

    /** Ctrl transform: 'a'/'A'..'z'/'Z' and @[\]^_ map to control codes 0x00, 0x1F. */
    fun ctrl(c: Char): Byte {
        val u = c.uppercaseChar()
        return when {
            u in 'A'..'Z' -> (u - 'A' + 1).toByte()
            u == '@'      -> 0
            u in '['..'_' -> (u.code - 64).toByte()
            u == ' '      -> 0
            else          -> c.code.toByte()
        }
    }

    /** Alt transform: prefix ESC. */
    fun alt(bytes: ByteArray): ByteArray = byteArrayOf(0x1B) + bytes
}
