package com.dracxterm.xset

import kotlin.math.max
import kotlin.math.min

/** Cyberpunk palette (aligned with res/values/colors.xml term_* tokens). ARGB. */
/**
 * Controller chrome palette. FACADE over [XsetDesign], historical symbol names are kept so no
 * render call-site changes, but every value now resolves to the single design-system source.
 * (Verified byte-identical to the pre-refactor constants by the host mirror's token-parity test.)
 */
object Palette {
    const val BG         = XsetDesign.Surface.BG
    const val PANEL      = XsetDesign.Surface.PANEL_LEFT
    const val PANEL_R    = XsetDesign.Surface.PANEL_RIGHT
    const val BORDER     = XsetDesign.Surface.BORDER
    const val ACCENT     = XsetDesign.ACCENT            // purple
    const val CYAN       = XsetDesign.State.INFO
    const val GREEN      = XsetDesign.State.SUCCESS
    const val AMBER      = XsetDesign.State.WARNING
    const val ERROR      = XsetDesign.State.ERROR       // NEW: promoted into chrome for error recovery
    const val FG         = XsetDesign.Text.PRIMARY
    const val DIM        = XsetDesign.Text.DIM
    const val TITLE      = XsetDesign.Text.TITLE
    const val SEL_BG     = XsetDesign.State.SEL_BG_FOCUS  // selected row (focused)
    const val SEL_BG_DIM = XsetDesign.State.SEL_BG_BLUR
    const val SEL_FG     = XsetDesign.State.SEL_FG
}

private const val A_BOLD = 1
private const val A_UNDERLINE = 2

/**
 * The `xset` dashboard engine: navigation state machine + two-panel renderer. Pure of Android APIs
 * (renders into a [TuiCanvas]; input arrives as normalized key names) so the whole thing is
 * host-verifiable. Live changes flow through [XsetContext]; the engine here is never edited to add a
 * module, modules come from [XsetRegistry].
 */
class XsetController(private val ctx: XsetContext) : XsetSurface {

    enum class Focus { LEFT, RIGHT }

    companion object {
        /** Key marker for the synthetic row produced when a module fails to build (error recovery). */
        const val ERROR_KEY = "_xset_module_error"
    }

    override var active = false; private set
    private var focus = Focus.LEFT
    private var selModule = 0
    private var selSetting = 0
    private var leftScroll = 0
    private var rightScroll = 0
    private var status = ""
    private var searchActive = false
    private var searchQuery = ""

    // Live-preview cursor, published for TerminalView's shared cursor renderer so the preview cursor
    // is drawn by the IDENTICAL path (same baseline/lineH/charW/style geometry) as the real terminal
    // cursor, no faked glyph, no manual offset. -1 = no preview cursor this frame.
    private var previewCursorIndex = -1
    private var previewCursorStyle = CursorStyle.BLOCK
    private var previewCursorColor = Palette.ACCENT
    private var renderCols = 0
    fun previewCursorIndex(): Int = previewCursorIndex
    fun previewCursorStyle(): CursorStyle = previewCursorStyle
    fun previewCursorColor(): Int = previewCursorColor

    private var cachedFor = -1
    private var cache: List<Setting> = emptyList()

    private fun modules() = XsetRegistry.modules()

    private fun settings(): List<Setting> {
        if (cachedFor != selModule) { cache = modules().getOrNull(selModule)?.let { safeBuild(it) } ?: emptyList(); cachedFor = selModule }
        return cache
    }
    private fun invalidate() { cachedFor = -1 }

    /**
     * Error recovery: build a module's rows, but never let a faulty module take down the dashboard.
     * On any Throwable we log it and return a single INFO row marked with [ERROR_KEY], so the terminal
     * and every other module keep working. This is the contract behind the "modul gagal dimuat" rule.
     */
    private fun safeBuild(m: XsetModule): List<Setting> = try {
        m.build(ctx)
    } catch (t: Throwable) {
        android.util.Log.e("xset", "module '${m.id}' failed to build; isolating", t)
        val msg = t.message ?: t.javaClass.simpleName
        val err = StatusKind.ERROR
        listOf(
            Setting(
                key = ERROR_KEY,
                label = "${err.icon} ${m.title} failed to load",
                kind = SettingKind.INFO,
                hint = msg,
                read = { msg },
            )
        )
    }

    // ---- lifecycle ----
    fun open(moduleId: String?) {
        active = true; focus = Focus.LEFT; selSetting = 0; leftScroll = 0; rightScroll = 0
        searchActive = false; searchQuery = ""; status = ""
        val idx = moduleId?.let { XsetRegistry.index(it) } ?: -1
        selModule = if (idx >= 0) idx else 0
        invalidate()
        if (idx >= 0) { focus = Focus.RIGHT }   // `xset theme` opens straight into the module
    }
    override fun close() { active = false; searchActive = false }

    // ---- input ----
    /**
     * Correct the status line from the host when an asynchronous result lands.
     *
     * A row whose value is decided by the OS (Storage Access waits on a permission dialog) writes
     * its status at the moment of the keypress, from the value it optimistically set. If the answer
     * comes back "no", the row fixes itself but that sentence does not, and the screen then says ON
     * next to a row reading OFF.
     */
    fun setStatus(msg: String) { status = msg }

    /** Only the search box takes text; every other row is driven by single-key commands. */
    override val acceptsText: Boolean get() = searchActive

    /** Normalized special keys: UP/DOWN/LEFT/RIGHT/ENTER/ESC/TAB/BACKTAB/HOME/END/PGUP/PGDN/BKSP. */
    override fun onSpecial(name: String): Boolean {
        if (!active) return false
        if (searchActive) return searchSpecial(name)
        when (name) {
            "UP" -> move(-1)
            "DOWN" -> move(+1)
            "LEFT" -> onLeft()
            "RIGHT" -> onRight()
            "ENTER" -> onEnter()
            "ESC" -> onEsc()
            "TAB", "BACKTAB" -> toggleFocus()
            "HOME" -> jump(true)
            "END" -> jump(false)
            "PGUP" -> page(-1)
            "PGDN" -> page(+1)
            "BKSP" -> {}
        }
        return true
    }

    /** Character input (search text, quick keys, Ctrl+S). */
    override fun onChar(c: Char, ctrl: Boolean, alt: Boolean): Boolean {
        if (!active) return false
        if (ctrl && (c == 's' || c == 'S')) { ctx.status("saved"); status = "Configuration saved ✓"; return true }
        if (searchActive) { searchChar(c); return true }
        // Case-folded, as 'q' already was. Shift or caps lock is not a different intent, and
        // leaving the capitals unbound made the navigation keys silently dead while Q still quit.
        when (c.lowercaseChar()) {
            '/' -> { searchActive = true; searchQuery = ""; status = "" }
            'q' -> ctx.requestClose()
            'j' -> move(+1)
            'k' -> move(-1)
            'h' -> onLeft()
            'l' -> onRight()
            else -> {}
        }
        return true
    }

    private fun toggleFocus() { focus = if (focus == Focus.LEFT) Focus.RIGHT else Focus.LEFT }

    private fun move(dir: Int) {
        if (focus == Focus.LEFT) {
            val n = modules().size; if (n == 0) return
            selModule = ((selModule + dir) % n + n) % n
            selSetting = 0; rightScroll = 0; invalidate()
        } else {
            val n = settings().size; if (n == 0) return
            selSetting = (selSetting + dir).coerceIn(0, n - 1)
        }
    }

    private fun jump(first: Boolean) {
        if (focus == Focus.LEFT) { selModule = if (first) 0 else max(0, modules().size - 1); selSetting = 0; invalidate() }
        else { selSetting = if (first) 0 else max(0, settings().size - 1) }
    }

    private fun page(dir: Int) { repeat(6) { move(dir) } }

    private fun onLeft() {
        if (focus == Focus.RIGHT) {
            val s = settings().getOrNull(selSetting)
            if (s != null && s.adjustable) applyAdjust(s, -1) else focus = Focus.LEFT
        }
    }

    private fun onRight() {
        if (focus == Focus.LEFT) { enterModule() }
        else {
            val s = settings().getOrNull(selSetting) ?: return
            if (s.adjustable) applyAdjust(s, +1)
            else if (s.kind == SettingKind.ACTION) runAction(s)
        }
    }

    private fun onEnter() {
        if (focus == Focus.LEFT) enterModule()
        else settings().getOrNull(selSetting)?.let { s ->
            when (s.kind) {
                SettingKind.ACTION -> runAction(s)
                SettingKind.INFO -> {}
                else -> applyActivate(s)
            }
        }
    }

    private fun onEsc() { if (focus == Focus.RIGHT) focus = Focus.LEFT else ctx.requestClose() }

    private fun enterModule() {
        focus = Focus.RIGHT; selSetting = 0; rightScroll = 0
        status = "xset › " + (modules().getOrNull(selModule)?.title ?: "")
    }

    private fun applyAdjust(s: Setting, dir: Int) { s.adjust(dir); status = "${s.label}: ${s.display()}" }
    private fun applyActivate(s: Setting) { s.activate(); status = "${s.label}: ${s.display()}" }
    private fun runAction(s: Setting) { val m = s.run?.invoke(); status = m ?: "${s.label} ✓"; invalidate() }

    // ---- search ----
    private fun searchChar(c: Char) {
        if (c == '\n' || c == '\r') { commitSearch(); return }
        if (c.code >= 32) { searchQuery += c }
    }
    private fun searchSpecial(name: String): Boolean {
        when (name) {
            "ENTER" -> commitSearch()
            "ESC" -> { searchActive = false; searchQuery = ""; status = "search cancelled" }
            "BKSP" -> if (searchQuery.isNotEmpty()) searchQuery = searchQuery.dropLast(1)
            else -> {}
        }
        return true
    }
    private fun commitSearch() {
        val q = searchQuery.trim().lowercase()
        searchActive = false
        if (q.isEmpty()) return
        val mods = modules()
        for (mi in mods.indices) {
            val list = safeBuild(mods[mi])
            val si = list.indexOfFirst { it.label.lowercase().contains(q) || it.key.lowercase().contains(q) }
            if (si >= 0) {
                selModule = mi; selSetting = si; focus = Focus.RIGHT; rightScroll = 0; invalidate()
                status = "found: ${list[si].label}"; return
            }
        }
        // module-title match fallback
        val mm = mods.indexOfFirst { it.title.lowercase().contains(q) || it.id.contains(q) }
        if (mm >= 0) { selModule = mm; enterModule(); status = "found module: ${mods[mm].title}" }
        else status = "no match: $searchQuery"
    }

    // ---- rendering ----
    override fun render(cols: Int, rows: Int): TuiCanvas {
        val cv = TuiCanvas(cols, rows, Palette.FG, Palette.BG)
        cv.clear(Palette.BG)
        previewCursorIndex = -1          // cleared each frame; renderPreview re-publishes when shown
        renderCols = cols
        if (cols < 30 || rows < 10) { renderTooSmall(cv); return cv }

        val leftW = (cols * 28 / 100).coerceIn(14, 26)
        val divCol = 1 + leftW               // vertical divider column
        val right0 = divCol + 1
        val rightW = (cols - 2) - right0     // inner width of right panel content

        // outer frame + header
        cv.box(0, 0, rows - 1, cols - 1, Palette.BORDER, Palette.BG)
        val title = " drac-Xterm · xset "
        cv.text(0, (cols - title.length) / 2, title, Palette.TITLE, Palette.BG, A_BOLD)
        cv.text(1, 2, "Terminal Configuration Framework", Palette.DIM, Palette.BG)
        val badge = "● READY"
        cv.textRight(1, cols - 2, badge, Palette.GREEN, Palette.BG, A_BOLD)

        // panel separators
        val bodyTop = 3
        val bodyBottom = rows - 6
        val sepClose = rows - 5
        val statusRow = rows - 4
        val footSep = rows - 3
        val footRow = rows - 2
        cv.hline(2, 1, cols - 2, '─'.code, Palette.BORDER, Palette.BG)
        cv.teeRight(2, 0, Palette.BORDER, Palette.BG); cv.teeLeft(2, cols - 1, Palette.BORDER, Palette.BG)
        cv.tDown(2, divCol, Palette.BORDER, Palette.BG)
        cv.vline(divCol, bodyTop, bodyBottom, '│'.code, Palette.BORDER, Palette.BG)
        cv.hline(sepClose, 1, cols - 2, '─'.code, Palette.BORDER, Palette.BG)
        cv.teeRight(sepClose, 0, Palette.BORDER, Palette.BG); cv.teeLeft(sepClose, cols - 1, Palette.BORDER, Palette.BG)
        cv.tUp(sepClose, divCol, Palette.BORDER, Palette.BG)
        cv.hline(footSep, 1, cols - 2, '─'.code, Palette.BORDER, Palette.BG)
        cv.teeRight(footSep, 0, Palette.BORDER, Palette.BG); cv.teeLeft(footSep, cols - 1, Palette.BORDER, Palette.BG)

        renderLeft(cv, bodyTop, bodyBottom, leftW)
        renderRight(cv, bodyTop, bodyBottom, right0, rightW)
        renderStatus(cv, statusRow, cols)
        renderFooter(cv, footRow, cols)
        return cv
    }

    private fun renderLeft(cv: TuiCanvas, top: Int, bottom: Int, leftW: Int) {
        cv.text(top, 2, "CUSTOMIZATION", Palette.DIM, Palette.BG, A_BOLD)
        val listTop = top + 1
        val viewH = bottom - listTop + 1
        val mods = modules()
        if (selModule < leftScroll) leftScroll = selModule
        if (selModule >= leftScroll + viewH) leftScroll = selModule - viewH + 1
        for (row in 0 until viewH) {
            val mi = leftScroll + row
            if (mi >= mods.size) break
            val r = listTop + row
            val m = mods[mi]
            val sel = mi == selModule
            val focused = sel && focus == Focus.LEFT
            val bg = when { focused -> Palette.SEL_BG; sel -> Palette.SEL_BG_DIM; else -> Palette.BG }
            if (bg != Palette.BG) cv.fillRect(r, 1, r, leftW, bg)
            val marker = if (sel) "▸" else " "
            val fg = when { focused -> Palette.SEL_FG; sel -> Palette.ACCENT; else -> Palette.FG }
            cv.text(r, 1, " $marker ${m.icon} ", if (sel) Palette.ACCENT else Palette.DIM, bg)
            cv.text(r, 6, fit(m.title, leftW - 5), fg, bg, if (focused) A_BOLD else 0)
        }
        // scroll hints
        if (leftScroll > 0) cv.text(listTop, leftW, "↑", Palette.DIM, Palette.BG)
        if (leftScroll + viewH < mods.size) cv.text(bottom, leftW, "↓", Palette.DIM, Palette.BG)
    }

    private fun renderRight(cv: TuiCanvas, top: Int, bottom: Int, c0: Int, w: Int) {
        val mod = modules().getOrNull(selModule)
        val list = settings()
        val header = (mod?.title ?: "").uppercase()
        cv.text(top, c0, "$header", Palette.CYAN, Palette.BG, A_BOLD)
        cv.text(top, c0 + header.length + 1, "· ${list.size} options", Palette.DIM, Palette.BG)

        val listTop = top + 2
        // reserve 4 rows at the bottom of the right panel for the live preview when there is room
        val previewH = if (bottom - listTop + 1 >= 8) 4 else 0
        val listBottom = bottom - previewH - (if (previewH > 0) 1 else 0)
        val viewH = max(1, listBottom - listTop + 1)

        if (selSetting < rightScroll) rightScroll = selSetting
        if (selSetting >= rightScroll + viewH) rightScroll = selSetting - viewH + 1

        val keyW = min(w * 6 / 10, 22)
        for (row in 0 until viewH) {
            val si = rightScroll + row
            if (si >= list.size) break
            val r = listTop + row
            val s = list[si]
            val sel = si == selSetting
            val focused = sel && focus == Focus.RIGHT
            val bg = when { focused -> Palette.SEL_BG; sel -> Palette.SEL_BG_DIM; else -> Palette.BG }
            if (bg != Palette.BG) cv.fillRect(r, c0, r, c0 + w - 1, bg)
            val marker = if (focused) "▸ " else "  "
            cv.text(r, c0, marker, Palette.ACCENT, bg)
            val isErr = s.key == ERROR_KEY
            val lblFg = when { isErr -> Palette.ERROR; focused -> Palette.SEL_FG; else -> Palette.FG }
            cv.text(r, c0 + 2, fit(s.label, keyW), lblFg, bg, if (focused || isErr) A_BOLD else 0)
            val valCol = c0 + 2 + keyW + 1
            val valFg = when (s.kind) {
                SettingKind.ACTION -> Palette.GREEN
                SettingKind.INFO -> Palette.DIM
                SettingKind.TOGGLE -> if (s.display() == "ON") Palette.GREEN else Palette.DIM
                else -> Palette.CYAN
            }
            val vtxt = when (s.kind) {
                SettingKind.ENUM, SettingKind.INT -> if (focused) "‹ ${s.display()} ›" else s.display()
                else -> s.display()
            }
            cv.text(r, valCol, fit(vtxt, c0 + w - valCol), valFg, bg, A_BOLD)
        }
        if (rightScroll > 0) cv.text(listTop, c0 + w - 1, "↑", Palette.DIM, Palette.BG)
        if (rightScroll + viewH < list.size) cv.text(listBottom, c0 + w - 1, "↓", Palette.DIM, Palette.BG)

        if (previewH > 0) renderPreview(cv, listBottom + 2, c0, w)
    }

    private fun renderPreview(cv: TuiCanvas, r0: Int, c0: Int, w: Int) {
        cv.text(r0, c0, "LIVE PREVIEW", Palette.DIM, Palette.BG, A_BOLD)
        cv.hline(r0, c0 + 12, c0 + w - 1, '─'.code, Palette.BORDER, Palette.BG)
        val fg = ctx.store.getInt("theme.fg", Palette.FG)
        val bg = ctx.store.getInt("theme.bg", Palette.BG)
        val cur = ctx.store.getInt("theme.cursor", Palette.ACCENT)
        cv.fillRect(r0 + 1, c0, r0 + 1, c0 + w - 1, bg)
        val prompt = "[drac@xterm ~]$ "
        val sample = "echo hi"
        val end = cv.text(r0 + 1, c0, fit(prompt, w), Palette.GREEN, bg)
        val afterText = cv.text(r0 + 1, end, fit(sample, c0 + w - end), fg, bg)
        // Publish the cursor cell INLINE at the input position (right after "echo hi") on the same row
        // as the text. TerminalView renders it through the shared drawGrid cursor path, so its
        // baseline, height, width and style geometry are identical to the real terminal cursor.
        val cx = min(afterText, c0 + w - 1)
        previewCursorIndex = (r0 + 1) * renderCols + cx
        previewCursorStyle = CursorStyle.from(ctx.store.get("cursor.style"))
        previewCursorColor = ctx.store.getInt("cursor.color", cur)
    }

    private fun renderStatus(cv: TuiCanvas, r: Int, cols: Int) {
        if (searchActive) {
            cv.text(r, 2, "/", Palette.AMBER, Palette.BG, A_BOLD)
            cv.text(r, 3, searchQuery, Palette.FG, Palette.BG)
            cv.put(r, 3 + searchQuery.length, '▏'.code, Palette.AMBER, Palette.BG)
        } else {
            cv.text(r, 2, fit(status.ifEmpty { "xset › ${modules().getOrNull(selModule)?.title ?: ""}" }, cols - 4), Palette.DIM, Palette.BG)
        }
    }

    private fun renderFooter(cv: TuiCanvas, r: Int, cols: Int) {
        val keys = "↑↓ Move   ←→ Adjust   ⏎ Apply   Esc Back   / Search   ^S Save   Q Exit"
        val start = max(2, (cols - keys.length) / 2)
        cv.text(r, start, fit(keys, cols - 1 - start), Palette.DIM, Palette.BG)
    }

    private fun renderTooSmall(cv: TuiCanvas) {
        val msg = "xset needs a larger view"
        cv.text(cv.rows / 2, max(0, (cv.cols - msg.length) / 2), fit(msg, cv.cols), Palette.AMBER, Palette.BG)
    }

    private fun fit(s: String, w: Int): String {
        if (w <= 0) return ""
        if (s.length <= w) return s
        if (w <= 1) return s.substring(0, w)
        return s.substring(0, w - 1) + "…"
    }

    // exposed for host tests
    fun debugState(): String = "focus=$focus mod=$selModule($cachedFor) set=$selSetting search=$searchActive/'$searchQuery' status='$status'"
    fun curModuleId(): String = modules().getOrNull(selModule)?.id ?: ""
    fun curSettingKey(): String = settings().getOrNull(selSetting)?.key ?: ""
}
