package com.dracxterm

/**
 * Thin JNI binding to the native terminal engine (libxterm.so).
 * Every method maps 1:1 to a Java_com_dracxterm_NativeTerminal_* symbol in native-lib.cpp.
 */
object NativeTerminal {
    init { System.loadLibrary("xterm") }

    // meta[] indices returned by nativeSnapshot (mirror of xterm::Terminal::M_*).
    const val M_COLS = 0
    const val M_ROWS = 1
    const val M_CURSOR_VIS = 2
    const val M_APP_CURSOR = 3
    const val M_MOUSE_MODE = 4
    const val M_BRACKETED = 5
    const val M_VIEW_OFFSET = 6
    const val M_SCROLLBACK = 7
    const val M_BELL = 8
    const val M_TITLE_REV = 9
    const val M_CLIP_REV = 10
    const val M_ON_ALT = 11
    const val M_FOCUS = 12
    const val M_APPCTRL_REV = 13
    const val M_COUNT = 14

    // Attribute bit set on a base cell that carries combining marks (mirror ATTR_COMBINING).
    const val ATTR_COMBINING = 1024

    /** Creates a PTY-backed session, execs argv[0], starts the reader thread. Returns a handle (0 on failure). */
    @JvmStatic external fun nativeCreate(
        cols: Int, rows: Int, argv: Array<String>, env: Array<String>, cwd: String
    ): Long

    @JvmStatic external fun nativeWrite(handle: Long, data: ByteArray)
    @JvmStatic external fun nativeResize(handle: Long, cols: Int, rows: Int)
    @JvmStatic external fun nativeGeneration(handle: Long): Long
    @JvmStatic external fun nativeRunning(handle: Long): Boolean

    /**
     * Copies the visible grid into the caller's arrays; meta filled per M_* indices.
     * Returns cursor index (row*cols+col), -1 if scrolled off, or -2 if arrays too small.
     */
    @JvmStatic external fun nativeSnapshot(
        handle: Long, glyphs: IntArray, fg: IntArray, bg: IntArray, attr: IntArray, meta: IntArray
    ): Int

    // Scrollback viewport.
    @JvmStatic external fun nativeScroll(handle: Long, deltaLines: Int)
    @JvmStatic external fun nativeScrollToBottom(handle: Long)

    // Selection (viewport row/col).
    @JvmStatic external fun nativeSelectStart(handle: Long, row: Int, col: Int)
    @JvmStatic external fun nativeSelectExtend(handle: Long, row: Int, col: Int)
    @JvmStatic external fun nativeSelectWord(handle: Long, row: Int, col: Int)
    @JvmStatic external fun nativeClearSelection(handle: Long)
    @JvmStatic external fun nativeSelectionText(handle: Long): String

    /** Search history+screen; returns absolute row or -1. Reveals the match in the viewport. */
    @JvmStatic external fun nativeSearch(handle: Long, query: String, fromRow: Int, forward: Boolean): Int

    /** Report a mouse event when the program enabled tracking. type: 0 press, 1 release, 2 move. */
    @JvmStatic external fun nativeMouse(handle: Long, button: Int, col: Int, row: Int, type: Int)

    /** Wrap paste text with bracketed-paste markers when the program enabled mode 2004. */
    @JvmStatic external fun nativeWrapPaste(handle: Long, text: String): ByteArray

    @JvmStatic external fun nativeTitle(handle: Long): String
    @JvmStatic external fun nativeClipboard(handle: Long): String

    /** Pending private app-control payload (OSC 5391), then cleared. "" when none. */
    @JvmStatic external fun nativeAppControl(handle: Long): String

    /** Apply theme colours (ARGB) and scrollback depth. Call once after create. */
    @JvmStatic external fun nativeConfigure(handle: Long, fg: Int, bg: Int, cursor: Int, scrollback: Int)

    /** Themed cursor colour (ARGB). */
    @JvmStatic external fun nativeCursorColor(handle: Long): Int

    /** Full grapheme (base + combining marks) at a viewport cell, for cells flagged ATTR_COMBINING. */
    @JvmStatic external fun nativeCellGrapheme(handle: Long, row: Int, col: Int): String

    @JvmStatic external fun nativeDestroy(handle: Long)
}
