package com.xdrac

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.text.InputType
import android.util.AttributeSet
import android.view.Choreographer
import android.view.HapticFeedbackConstants
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import android.view.ViewConfiguration
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import com.xdrac.xset.CursorStyle
import com.xdrac.xset.XsetController
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

// xterm mouse button codes for the scroll wheel (Cb 64/65). Sent when a program has mouse
// tracking enabled, so a finger swipe scrolls the program instead of dragging its left button.
private const val MOUSE_WHEEL_UP = 64
private const val MOUSE_WHEEL_DOWN = 65

// Upper bound on synthesized keystrokes/wheel reports per ACTION_MOVE. A move event carries at
// most a couple of lines at 60 Hz; the cap only exists so a very fast drag (or a stall that
// batches several frames into one event) cannot dump a burst of arrow keys into the PTY.
private const val MAX_KEYS_PER_MOVE = 4

/**
 * Renders the native screen buffer and forwards input to the session.
 * Rendering is damage-driven: each vsync frame we compare the native generation
 * counter and only re-snapshot + invalidate when it changed.
 */
class TerminalView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : View(context, attrs), Choreographer.FrameCallback {

    private var session: TerminalSession? = null

    // Persisted UI prefs (zoom). Rendering-only; never touches the PTY.
    private val uiPrefs = context.getSharedPreferences("term_ui", Context.MODE_PRIVATE)

    // Bundled fixed-pitch font -> device-independent cell metrics (dense, reference-like).
    // Falls back to the platform monospace if the asset is missing.
    private val termTypeface: Typeface =
        runCatching { Typeface.createFromAsset(context.assets, "fonts/JetBrainsMono-Regular.ttf") }
            .getOrDefault(Typeface.MONOSPACE)

    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = termTypeface
    }
    private val bgPaint = Paint()
    private val cursorPaint = Paint().apply { color = 0xFF8A5CF6.toInt() }

    private var charW = 0f
    private var lineH = 0f
    private var baseline = 0f

    // Content inset: a proportional left/top breathing margin so column 0 (e.g. the
    // box-drawing prompt glyphs ┌/└) does not hug the screen edge and look clipped.
    // Expressed relative to the cell so it scales with zoom, and applied CONSISTENTLY in
    // measure (recomputeGrid), draw (onDraw) and touch (cellCol/cellRow). It is not a raw
    // View padding, which would desync the grid math from the draw origin.
    private var padX = 0f
    private var padY = 0f

    // Font size is expressed in dp (density only, not scaledDensity) so the terminal grid is
    // predictable and not inflated by the system accessibility font-scale. User-zoomable.
    private val defaultFontDp = 12f

    // The login banner (assets/banner.art) is 69 columns wide: one line is, the rest are
    // 63 or less, and banner.sh centres it. The terminal therefore has to open wider
    // than 69 or the first thing a user sees is either broken ASCII or art jammed against
    // both edges with no margin at all.
    //
    // 78 columns puts about four either side of the centred art. On a 720px screen that
    // costs font size: the fit lands on the 7.5dp floor below, which is small. That is a
    // deliberate trade the maintainer chose over trimming the art. The widest line is
    // the dragon's tail, and shortening it would change the drawing.
    //
    // Anyone who disagrees can pinch; this only ever decides the very first launch.
    private val BANNER_FIT_COLS = 78f

    // Zoom moves in fixed steps rather than by free multiplication. Without this, pinch
    // leaves fractional sizes (12.7431dp) that no button press can land on, currentFontDp()
    // rounds for display, and xset then shows a number the view is not actually using.
    // Quantising on every commit makes pinch, the ⊖/⊕ keys and the settings screen agree.
    private val fontStepDp = 0.5f

    private var fontSizeDp = defaultFontDp
    private val minFontDp = 7f
    private val maxFontDp = 28f

    /** Snap to the zoom ladder and clamp. Every path that changes the font goes through it. */
    private fun quantiseFont(dp: Float): Float =
        (kotlin.math.round(dp / fontStepDp) * fontStepDp).coerceIn(minFontDp, maxFontDp)

    private var cols = 0
    private var rows = 0

    // While the IME is animating we suppress PTY grid resizes and keep rendering the current
    // grid, then resize exactly once when the animation settles. This removes the per-frame
    // resize/SIGWINCH storm that causes the open/close keyboard flicker (no timers involved).
    private var resizeSuppressed = false
    fun setResizeSuppressed(suppressed: Boolean) {
        if (resizeSuppressed == suppressed) return
        resizeSuppressed = suppressed
        if (!suppressed) recomputeGrid(width, height)   // settle to the final size, once
    }

    // Snapshot buffers (reallocated on resize).
    private var glyphs = IntArray(0)
    private var fg = IntArray(0)
    private var bg = IntArray(0)
    private var attr = IntArray(0)
    private val meta = IntArray(NativeTerminal.M_COUNT)
    private var cursorIndex = -1
    private var cursorVisible = true
    private var appCursor = false
    private var mouseMode = 0
    // True while a full-screen program (nano, vim, less, htop) owns the display via the
    // alternate screen. Mirrors meta[M_ON_ALT] and is the switch that decides whether a
    // vertical swipe drives our scrollback viewport or is handed to the program.
    private var onAlt = false
    private var focusReporting = false
    private var lastGen = -1L

    // Workspace-switch anti-flicker. When a freshly-started session that has not produced any
    // output yet (generation still 0) is bound, we HOLD the currently painted frame instead of
    // snapshotting its empty grid, until its shell prints the first prompt (generation advances)
    // or a safety deadline elapses. This removes the blank-grid flash seen when opening a new
    // workspace for the first time. Sessions that already have content are unaffected.
    //
    // The first byte from the shell is the real trigger; the deadline only bounds a shell
    // that prints nothing at all. 1200 ms because a cold proot+bash login, sourcing
    // /etc/profile then ~/.profile then ~/.bashrc then the banner, routinely needs more than
    // 150 ms to emit anything, and a cap that fires first paints the empty grid, which is the
    // flash this hold exists to prevent. A generous bound costs nothing: the previous
    // workspace's finished frame stays on screen throughout, so the wait is never a blank.
    private var awaitFirstContent = false
    private var awaitDeadlineNanos = 0L
    // Generation the hold is measured AGAINST. A resize() bumps the native generation (grid reflow),
    // so the hold must release on real shell output BEYOND this base, not on the bare resize.
    private var awaitBaseGen = 0L
    // Coalescing cursor for the atomic first-open present. Once the fresh session's output advances
    // past awaitBaseGen, the first burst is not presented. Its generation is remembered here
    // and presented once it has settled, so the prompt and banner arrive as one frame rather
    // than as a partial prompt that then completes. -1 means no output yet.
    private var pendingPresentGen = -1L
    // How long output must stay quiet before the frame is presented. The two-line login prompt
    // arrives over several PTY reads with gaps measured on-device at roughly 100 ms, so waiting a
    // single vsync still catches it half-drawn. Waiting for quiet instead means waiting until the
    // shell has stopped printing and is idle at its prompt. Re-armed by every new burst.
    private var awaitQuiescentNanos = 0L
    private val FIRST_CONTENT_QUIESCENT_NS = 180_000_000L   // 180 ms idle (> observed ~100 ms gap)
    // While holding for a fresh session, glyphs still hold the previous workspace's finished
    // frame, which onDraw keeps painting, so the screen is never blank. Only a grid resize
    // (reallocFor) can zero them;
    // if that happens before the new session has any content to reflow, this flips true and onDraw
    // paints the terminal background for the brief remainder of the hold. Cleared per hold/present.
    private var holdGridWiped = false
    private val FIRST_CONTENT_TIMEOUT_NS = 1_200_000_000L   // 1200 ms safety cap (bound, not the trigger)

    private var lastTitleRev = -1
    private var lastBell = -1
    private var lastClipRev = -1
    private var lastAppCtrlRev = -1

    // Sticky modifiers driven by the extra-keys toolbar.
    private var ctrlActive = false
    private var altActive = false

    var onGeometry: ((Int, Int) -> Unit)? = null
    var onModifierConsumed: ((String) -> Unit)? = null
    var onTitle: ((String) -> Unit)? = null
    var onBell: (() -> Unit)? = null
    var onClipboardCopy: ((String) -> Unit)? = null
    var onExit: (() -> Unit)? = null
    // Private app-control channel (OSC 5391), e.g. the in-terminal `xset` settings command.
    var onAppControl: ((String) -> Unit)? = null

    private var wasRunning = true
    private var themedCursor = false

    private var DEFAULT_BG = 0xFF0A0A0C.toInt()

    // ---- xset dashboard (in-terminal TUI overlay) ----
    // When [dashboard]?.active, input is routed to the dashboard and the grid is painted from it;
    // the PTY/session are untouched. Null/inactive => normal terminal behaviour (zero change).
    var dashboard: XsetController? = null
    private fun dashActive(): Boolean = dashboard?.active == true
    private fun repaintDash() = invalidate()

    // ---- xset render knobs (all default to the pre-xset behaviour) ----
    private var cursorStyle = CursorStyle.BLOCK
    private var cursorBlink = false
    private var cursorBlinkOn = true
    private var boldBright = false
    private var lineSpacingPct = 100
    private var letterSpacingMilli = 0
    private var paddingScalePct = 100
    private var fontFamilyId = "jetbrains"

    // ---- attribute bits (mirror xterm::Attr / SnapAttr) ----
    private val A_BOLD = 1; private val A_UNDERLINE = 2
    private val A_ITALIC = 8; private val A_DIM = 16; private val A_STRIKE = 32
    private val A_WIDE_TAIL = 512; private val A_COMBINING = NativeTerminal.ATTR_COMBINING

    // ---- touch state ----
    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private val longPressTimeout = ViewConfiguration.getLongPressTimeout().toLong()
    private var downX = 0f; private var downY = 0f
    private var lastScrollY = 0f   // previous ACTION_MOVE y; scroll accumulates the per-move delta
    private var selecting = false
    private var scrolling = false
    private var scrollAccum = 0f
    private var mouseCol = -1; private var mouseRow = -1   // last reported mouse cell (drag)
    private val longPressRunnable = Runnable { beginSelection() }

    // Pinch-to-zoom (two-finger). During the gesture only the font is repainted (cheap); the
    // grid/PTY are resized exactly ONCE at the end, so a pinch no longer fires a per-frame
    // SIGWINCH storm, that storm was the source of the zoom flicker / excessive redraw.
    private var pinching = false
    private val scaleDetector = ScaleGestureDetector(context,
        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScaleBegin(d: ScaleGestureDetector): Boolean { pinching = true; return true }
            override fun onScale(d: ScaleGestureDetector): Boolean {
                // Not quantised mid-gesture: pinch should track the fingers smoothly.
                // onScaleEnd snaps to the ladder, so where it lands is still a step.
                val target = (fontSizeDp * d.scaleFactor).coerceIn(minFontDp, maxFontDp)
                if (target != fontSizeDp) { fontSizeDp = target; applyFontMetrics(); invalidate() }
                return true
            }
            override fun onScaleEnd(d: ScaleGestureDetector) {
                pinching = false
                fontSizeDp = quantiseFont(fontSizeDp)
                applyFontMetrics()
                recomputeGrid(width, height); saveZoom(); lastGen = -1L; invalidate()
            }
        })
    private var lastTapAt = 0L

    // Incremental search state.
    private var searchQuery = ""
    private var searchRow = Int.MAX_VALUE

    init {
        isFocusable = true
        isFocusableInTouchMode = true
        fontSizeDp = quantiseFont(uiPrefs.getFloat("fontDp", defaultFontDp))
        applyFontMetrics()
    }

    fun attach(session: TerminalSession) { this.session = session }
    fun setCtrl(active: Boolean) { ctrlActive = active }
    fun setAlt(active: Boolean) { altActive = active }

    // ---- xset render-knob API (called by the XsetContext bridge; all default-preserving) ----
    fun setFontSizeDp(dp: Int) {
        val t = quantiseFont(dp.toFloat()); if (t == fontSizeDp) return
        fontSizeDp = t; saveZoom(); applyFontMetrics(); recomputeGrid(width, height); lastGen = -1L; invalidate()
    }
    fun currentFontDp(): Int = (fontSizeDp + 0.5f).toInt()
    fun setFontFamilyId(id: String) {
        if (id == fontFamilyId) return
        fontFamilyId = id; applyFontMetrics(); recomputeGrid(width, height); lastGen = -1L; invalidate()
    }
    fun setLineSpacingPct(pct: Int) {
        lineSpacingPct = pct.coerceIn(90, 160); applyFontMetrics(); recomputeGrid(width, height); lastGen = -1L; invalidate()
    }
    fun setLetterSpacingMilli(m: Int) {
        letterSpacingMilli = m.coerceIn(0, 120); applyFontMetrics(); recomputeGrid(width, height); lastGen = -1L; invalidate()
    }
    fun setPaddingScalePct(pct: Int) {
        paddingScalePct = pct.coerceIn(50, 200); applyFontMetrics(); recomputeGrid(width, height); lastGen = -1L; invalidate()
    }
    fun setCursorStyle(style: CursorStyle) { cursorStyle = style; invalidate() }
    fun setCursorBlink(on: Boolean) { cursorBlink = on; startBlink() }
    fun setCursorColor(argb: Int) { cursorPaint.color = argb; themedCursor = true; invalidate() }
    fun setBoldBright(on: Boolean) { boldBright = on; invalidate() }
    fun setDefaultBg(argb: Int) { DEFAULT_BG = argb; setBackgroundColor(argb); invalidate() }

    /**
     * Re-pull the grid after a native `configure()` (theme colours, scrollback).
     *
     * ScreenBuffer::applyConfig re-themes every cell that carried the OLD default colour, in the
     * live grid, the alternate screen and the scrollback. That mutation happens on the UI thread
     * and damages no cell through the reader thread, so `generation()` never moves -- and
     * generation is the only thing [doFrame] consults before calling [refreshSnapshot]. Without
     * this the engine held the new colours while the painted snapshot held the old ones, and the
     * screen only caught up when the shell happened to print something. That is exactly why a
     * theme change looked like it needed "leave xset, then touch the terminal".
     *
     * Deliberately the same idiom the other knob setters use (`lastGen = -1L`) rather than an
     * immediate [refreshSnapshot]: during the first-open hold [doFrame] owns `lastGen` and must
     * keep painting the retained frame, and re-snapshotting underneath it would present the new
     * session's empty grid -- the flash the hold exists to prevent.
     */
    fun refreshAfterConfigChange() { lastGen = -1L; invalidate() }

    private val blinkRunnable = object : Runnable {
        override fun run() { cursorBlinkOn = !cursorBlinkOn; invalidate(); postDelayed(this, 530) }
    }
    private fun startBlink() {
        removeCallbacks(blinkRunnable)
        cursorBlinkOn = true
        // Drive the blink phase whenever Blink is ON, including while the xset dashboard is open, so
        // the live PREVIEW cursor blinks on the same phase as the terminal. This is safe: with the
        // dashboard active onDraw returns after painting the dashboard (the terminal's own cursor is
        // never drawn), so the running timer only toggles the shared phase the preview reads.
        if (cursorBlink) postDelayed(blinkRunnable, 530)
        invalidate()
    }

    /** Open/close the xset dashboard overlay. While open, input routes to it and the PTY is idle. */
    fun openDashboard(moduleId: String?) { dashboard?.open(moduleId); startBlink(); requestFocusAndKeyboard(); invalidate() }
    fun closeDashboard() { dashboard?.close(); startBlink(); invalidate() }
    fun dashboardActive(): Boolean = dashboard?.active == true

    /** Active character grid, used by the WorkspaceManager to start a new session at the
     *  current size before it becomes visible. */
    fun gridCols() = cols
    fun gridRows() = rows

    /**
     * Rebind the view to a different session (workspace switch). Each session keeps its own
     * PTY/screen/scrollback natively, so switching is just: drop transient UI state, bind the
     * new session, size it to the current grid, and force a fresh snapshot + repaint.
     */
    fun switchSession(s: TerminalSession) {
        if (s === session) return
        removeCallbacks(longPressRunnable)
        selecting = false; scrolling = false
        session?.clearSelection()
        session = s
        // Sample whether the shell has produced real output BEFORE touching the size. A resize()
        // bumps the native generation counter (grid reflow); if we sampled it afterwards a freshly
        // started, still-silent session would look like it "has content" and get painted as an empty
        // reflow frame, the exact blank→prompt flash the hold is meant to prevent. This is reachable
        // whenever the grid changed between the background start and this present (e.g. the keyboard
        // opening shrank the grid), which is precisely the first-open-with-keyboard case.
        val hadOutput = s.isValid() && s.generation() > 0L
        if (cols > 0 && rows > 0 && s.isValid()) s.resize(cols, rows)
        // Force every out-of-band tracker to re-fire for the newly shown session.
        lastTitleRev = -1; lastBell = -1; lastClipRev = -1; lastAppCtrlRev = -1
        themedCursor = false; wasRunning = true
        if (s.isValid() && !hadOutput) {
            // Fresh session, no shell output yet: keep the current frame on screen and wait for the
            // shell's first real output before painting, so we never flash its empty grid. We hold
            // relative to the generation AFTER the (possible) resize bump, so the resize reflow alone
            // does not release the hold, only genuine shell output (generation > base) does.
            awaitFirstContent = true
            holdGridWiped = false            // glyphs still hold the previous workspace's VALID frame
            pendingPresentGen = -1L          // no output seen at this grid yet
            awaitDeadlineNanos = System.nanoTime() + FIRST_CONTENT_TIMEOUT_NS
            awaitBaseGen = s.generation()
            lastGen = awaitBaseGen
            invalidate()                     // keep painting the previous frame until the new one is ready
        } else {
            // Existing session already has content: refresh immediately to show it (instant revisit).
            awaitFirstContent = false
            holdGridWiped = false
            pendingPresentGen = -1L
            lastGen = -1L
            invalidate()
        }
    }

    fun requestFocusAndKeyboard() {
        requestFocus()
        val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE)
                as android.view.inputmethod.InputMethodManager
        imm.showSoftInput(this, android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT)
    }

    // ---- lifecycle ----
    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        Choreographer.getInstance().postFrameCallback(this)
        startBlink()
    }
    override fun onDetachedFromWindow() {
        Choreographer.getInstance().removeFrameCallback(this)
        removeCallbacks(longPressRunnable)
        removeCallbacks(blinkRunnable)
        super.onDetachedFromWindow()
    }
    override fun doFrame(frameTimeNanos: Long) {
        val s = session
        if (s != null && s.isValid()) {
            val gen = s.generation()
            if (awaitFirstContent) {
                // ATOMIC FIRST-OPEN PRESENT. While holding, onDraw keeps painting the PREVIOUS
                // workspace's valid frame (never a stale-swap, a zeroed grid, or a half-arrived
                // prompt; only a resize with a still-empty new session falls back to a brief
                // background). Release, and present the new session in one frame, only when its
                // first COMPLETE output is ready at the SETTLED grid.
                when {
                    System.nanoTime() >= awaitDeadlineNanos -> {
                        // Safety bound: a shell that printed nothing within the cap. Present as-is.
                        awaitFirstContent = false; holdGridWiped = false; pendingPresentGen = -1L; lastGen = -1L
                        startBlink()
                    }
                    resizeSuppressed -> {
                        // An IME resize is still animating: the final grid is not settled. Keep the
                        // clean-background hold so we never present at the pre-keyboard size and then
                        // reflow/jump the prompt (the workspace-2 first-open flash).
                        Choreographer.getInstance().postFrameCallback(this); return
                    }
                    gen == awaitBaseGen -> {
                        // No real shell output at the settled grid yet (resize bumps are absorbed
                        // into awaitBaseGen, so a bare reflow never releases the hold).
                        Choreographer.getInstance().postFrameCallback(this); return
                    }
                    gen != pendingPresentGen -> {
                        // A new output burst arrived: (re)arm the quiescence window. The prompt prints
                        // over several PTY reads with gaps up to ~100 ms, so we must wait for the shell
                        // to go QUIET (finished printing, idle at the prompt), not merely one vsync ,
                        // or we present a partial prompt that then redraws.
                        pendingPresentGen = gen
                        awaitQuiescentNanos = System.nanoTime() + FIRST_CONTENT_QUIESCENT_NS
                        Choreographer.getInstance().postFrameCallback(this); return
                    }
                    System.nanoTime() < awaitQuiescentNanos -> {
                        // Generation stable this frame, but not quiet long enough yet, keep holding
                        // so a late final line (the second prompt row) is included in the one present.
                        Choreographer.getInstance().postFrameCallback(this); return
                    }
                    else -> {
                        // Output has been quiet for the quiescence window → the first COMPLETE prompt
                        // is ready. Present it atomically.
                        awaitFirstContent = false; holdGridWiped = false; pendingPresentGen = -1L; lastGen = -1L
                        startBlink()   // new prompt's cursor starts ON immediately, not mid-blink-off
                    }
                }
            }
            if (gen != lastGen) { lastGen = gen; refreshSnapshot(); invalidate() }
            val run = s.running()
            if (wasRunning && !run) { wasRunning = false; onExit?.invoke() }
        }
        Choreographer.getInstance().postFrameCallback(this)
    }

    private fun refreshSnapshot() {
        val s = session ?: return
        if (glyphs.isEmpty()) return
        var cursor = s.snapshot(glyphs, fg, bg, attr, meta)
        if (cursor == -2) {
            reallocFor(meta[NativeTerminal.M_COLS], meta[NativeTerminal.M_ROWS])
            cursor = s.snapshot(glyphs, fg, bg, attr, meta)
        }
        cursorIndex = cursor
        if (!themedCursor) { val cc = s.cursorColor(); if (cc != 0) { cursorPaint.color = cc; themedCursor = true } }
        cursorVisible = meta[NativeTerminal.M_CURSOR_VIS] == 1
        appCursor = meta[NativeTerminal.M_APP_CURSOR] == 1
        mouseMode = meta[NativeTerminal.M_MOUSE_MODE]
        onAlt = meta[NativeTerminal.M_ON_ALT] == 1
        focusReporting = meta[NativeTerminal.M_FOCUS] == 1

        val tRev = meta[NativeTerminal.M_TITLE_REV]
        if (tRev != lastTitleRev) { lastTitleRev = tRev; onTitle?.invoke(s.title()) }
        val bell = meta[NativeTerminal.M_BELL]
        if (lastBell in 0 until bell) onBell?.invoke()
        lastBell = bell
        val cRev = meta[NativeTerminal.M_CLIP_REV]
        if (cRev != lastClipRev) { if (lastClipRev >= 0) onClipboardCopy?.invoke(s.clipboard()); lastClipRev = cRev }
        val aRev = meta[NativeTerminal.M_APPCTRL_REV]
        if (aRev != lastAppCtrlRev) {
            if (lastAppCtrlRev >= 0) { val p = s.appControl(); if (p.isNotEmpty()) onAppControl?.invoke(p) }
            lastAppCtrlRev = aRev
        }
    }

    /** Immediately re-snapshot and repaint after a UI-thread mutation (scroll/selection),
     *  which does not go through the reader-thread generation counter. */
    private fun uiRefresh() { refreshSnapshot(); invalidate() }

    private fun reallocFor(c: Int, r: Int) {
        val n = max(1, c * r)
        glyphs = IntArray(n); fg = IntArray(n); bg = IntArray(n); attr = IntArray(n)
    }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        super.onSizeChanged(w, h, ow, oh)
        recomputeGrid(w, h)
    }

    /** Recompute the character grid for a pixel size and resize the PTY to match. */
    private fun recomputeGrid(w: Int, h: Int) {
        if (charW <= 0f || lineH <= 0f || w <= 0 || h <= 0) return
        maybeApplyStartupZoom(w)
        // Usable area excludes the content inset (left+right, top+bottom) so the grid never
        // overflows into the margin, the column count shrinks by the margin, it is not wrong.
        val usableW = w - 2f * padX
        val usableH = h - 2f * padY
        val newCols = max(1, floor(usableW / charW).toInt())
        val newRows = max(1, floor(usableH / lineH).toInt())
        if (newCols == cols && newRows == rows) return
        if (resizeSuppressed) return   // defer the PTY resize until the IME animation settles
        cols = newCols; rows = newRows
        reallocFor(cols, rows)                     // fresh, ZEROED snapshot arrays for the new grid
        val s = session
        if (s != null && s.isValid()) {
            s.resize(cols, rows)                   // native reflow (content-preserving) + SIGWINCH
            if (awaitFirstContent) {
                // Holding for a fresh session's first prompt when a grid resize lands (keyboard
                // settling). reallocFor just zeroed glyphs, so the previous frame we were showing is
                // gone. Two cases:
                if (s.generation() > awaitBaseGen) {
                    // The new session ALREADY produced real output (e.g. it printed at the pre-keyboard
                    // size during the hold). Its native buffer is content-preserving across resize, so
                    // the reflowed prompt is valid NOW, present it in one atomic frame at the final
                    // grid. No background, no reflow-jump: the user goes straight from the previous
                    // frame to the complete prompt at the settled size.
                    awaitFirstContent = false; holdGridWiped = false; pendingPresentGen = -1L
                    refreshSnapshot(); lastGen = s.generation(); invalidate()
                    startBlink()   // new prompt's cursor starts ON immediately
                } else {
                    // The new session has printed nothing yet and the previous frame is gone; paint a
                    // clean background for the brief remainder of the hold, and keep waiting for the
                    // shell's first real output at this settled grid (resize bump absorbed into base).
                    holdGridWiped = true
                    awaitBaseGen = s.generation()
                    lastGen = awaitBaseGen
                    pendingPresentGen = -1L
                }
            } else {
                // Live session: the native buffer already holds the REFLOWED content, so repopulate
                // the snapshot NOW, before any invalidate, so the freshly-zeroed arrays can never
                // be painted. That empty-grid frame between realloc and the next vsync WAS the resize
                // flicker (keyboard open/close, workspace-2 first open after the keyboard settles,
                // and zoom). refreshSnapshot reads the reflowed prompt, so the present stays clean.
                refreshSnapshot()
                lastGen = s.generation()
                invalidate()
            }
        } else {
            onGeometry?.invoke(cols, rows)
            lastGen = -1L
        }
    }

    // ---- font metrics & zoom (rendering only; PTY is resized, its contents are untouched) ----
    private fun applyFontMetrics() {
        textPaint.typeface = if (fontFamilyId == "system") Typeface.MONOSPACE else termTypeface
        textPaint.textSize = fontSizeDp * resources.displayMetrics.density
        val fm = textPaint.fontMetrics
        // Letter spacing widens the cell (glyph stays left-aligned in the cell); at 0 this is a no-op.
        val baseCharW = textPaint.measureText("M")
        charW = baseCharW + (letterSpacingMilli / 1000f) * textPaint.textSize
        // ascent/descent (not top/bottom) packs rows tightly like a desktop terminal,
        // matching the reference density instead of the looser external-leading spacing.
        val natural = ceil(fm.descent - fm.ascent)
        lineH = ceil(natural * lineSpacingPct / 100f)
        // centre the glyph vertically when the line is expanded by line-spacing (100% => unchanged)
        baseline = -fm.ascent + (lineH - natural) / 2f
        // Left/right breathing room so column 0 (box-drawing prompt glyphs ┌/└) never hugs
        // the screen edge. Proportional to the font AND floored to a device-independent
        // minimum so it stays clearly visible at small font sizes (0.5·cell was ~3px at 12dp
        // and looked like no margin at all). Top gets a quarter cell. Scaled by padding knob.
        val d = resources.displayMetrics.density
        val ps = paddingScalePct / 100f
        padX = max(charW * 0.75f, 8f * d) * ps
        padY = max(lineH * 0.25f, 4f * d) * ps
    }

    private fun applyZoom(factor: Float) {
        // Quantised, and forced to move at least one step. Multiplying a small font by
        // 1.1 and rounding could land back on the same value, so the button would look
        // dead at the small end of the range.
        var target = quantiseFont(fontSizeDp * factor)
        if (target == fontSizeDp) {
            target = quantiseFont(fontSizeDp + if (factor > 1f) fontStepDp else -fontStepDp)
        }
        if (target == fontSizeDp) return
        fontSizeDp = target
        saveZoom()
        applyFontMetrics(); recomputeGrid(width, height); lastGen = -1L; invalidate()
    }

    /** Notified after any font-size change lands, so a host mirroring the size elsewhere (the xset
     *  store, which the Font module displays) does not go stale behind a zoom. */
    var onFontSizeChanged: ((Int) -> Unit)? = null

    private fun saveZoom() {
        uiPrefs.edit().putFloat("fontDp", fontSizeDp).apply()
        onFontSizeChanged?.invoke(currentFontDp())
    }

    // Documented startup zoom: on the very first run (no persisted font size) pick a font that
    // makes the ~BANNER_FIT_COLS-wide banner fit the screen width, so the terminal opens already
    // proportional and legible instead of at Android's tiny default, using the real font
    // pipeline, not a faked/permanent size. Clamped to a comfortable range; the user's later
    // pinch/zoom choice is persisted and always wins.
    private val hasSavedZoom = uiPrefs.contains("fontDp")
    private var startupZoomDone = false
    private fun maybeApplyStartupZoom(w: Int) {
        if (startupZoomDone || hasSavedZoom || w <= 0 || charW <= 0f) return
        startupZoomDone = true
        val pxPerColPerDp = charW / fontSizeDp                 // one column's px width per 1dp
        val avail = w - 2f * padX
        // Floor 7.5dp, not 8: at 8dp a 720px screen yields ~69 columns, exactly the art's
        // width, so the banner touches both edges. Half a dp buys the margin.
        val fitDp = quantiseFont((avail / (BANNER_FIT_COLS * pxPerColPerDp)).coerceIn(7.5f, 14f))
        if (kotlin.math.abs(fitDp - fontSizeDp) > 0.1f) {
            fontSizeDp = fitDp
            applyFontMetrics()
        }
        saveZoom()
    }

    /** Toolbar ⇩ : jump back to the live bottom of the buffer. */
    fun toBottom() { session?.scrollToBottom(); uiRefresh() }

    /** Public zoom controls (⊖ / ⊕ toolbar keys + pinch + double-tap reset). */
    fun zoomIn() = applyZoom(1.1f)
    fun zoomOut() = applyZoom(1f / 1.1f)
    fun resetZoom() {
        val target = quantiseFont(defaultFontDp)
        if (fontSizeDp == target) return
        fontSizeDp = target
        saveZoom()
        applyFontMetrics(); recomputeGrid(width, height); lastGen = -1L; invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        if (cols == 0 || rows == 0 || glyphs.isEmpty()) return
        if (awaitFirstContent && holdGridWiped && dashboard?.active != true) {
            // Hold, and a resize wiped the retained previous frame while the new session has no
            // content yet: paint the terminal background (brief) rather than a zeroed grid.
            canvas.drawColor(DEFAULT_BG)
            return
        }
        val d = dashboard
        if (d != null && d.active) {
            // Paint the settings dashboard from its own styled grid, reusing the same renderer, and
            // draw the live-preview cursor through the same cursor path as the terminal, at the cell
            // the controller published, so its baseline/height/size are identical (no faked glyph).
            val cv = d.render(cols, rows)
            val pc = d.previewCursorIndex()
            // Preview cursor follows the same blink phase as the terminal: Blink OFF ⇒ always visible;
            // Blink ON ⇒ visible only while cursorBlinkOn (toggled by blinkRunnable). Same cell, same
            // shared cursor renderer, identical baseline/height/width/style to the real cursor.
            val previewCursorOn = pc >= 0 && (!cursorBlink || cursorBlinkOn)
            drawGrid(canvas, cv.glyphs, cv.fg, cv.bg, cv.attr,
                cursorCol = pc, drawCursor = previewCursorOn,
                curColor = d.previewCursorColor(), curStyle = d.previewCursorStyle(), combining = false)
            return
        }
        // During the first-open hold we paint the previous workspace's FROZEN frame. Do not draw its
        // blinking cursor. A cursor blinking on the retained command line for the length of the
        // transition is what reads as "the command line flickers" when a tab is opened. The new
        // session's cursor appears once, with its prompt.
        val showCursor = !awaitFirstContent && cursorVisible && isFocused && cursorBlinkOn
        drawGrid(canvas, glyphs, fg, bg, attr,
            cursorCol = if (showCursor) cursorIndex else -1,
            drawCursor = showCursor,
            curColor = cursorPaint.color, curStyle = cursorStyle, combining = true)
    }

    /**
     * The single grid renderer, shared by the terminal and the xset dashboard. Draws [g]/[f]/[b]/[at]
     * (glyph/fg/bg/attr, row-major cols×rows) using the current font metrics/insets, then optionally
     * paints the cursor in [curStyle]. [combining] enables grapheme lookup (terminal only).
     */
    private fun drawGrid(
        canvas: Canvas, g: IntArray, f: IntArray, b: IntArray, at: IntArray,
        cursorCol: Int, drawCursor: Boolean, curColor: Int, curStyle: CursorStyle, combining: Boolean,
    ) {
        for (r in 0 until rows) {
            val y = padY + r * lineH
            for (c in 0 until cols) {
                val i = r * cols + c
                if (i >= g.size) continue
                val a = at[i]
                if ((a and A_WIDE_TAIL) != 0) continue   // drawn by the wide head
                val x = padX + c * charW
                val cellBg = b[i]
                if (cellBg != DEFAULT_BG) {
                    bgPaint.color = cellBg
                    val wCells = if ((a and 256) != 0) 2 else 1   // ATTR_WIDE
                    canvas.drawRect(x, y, x + charW * wCells, y + lineH, bgPaint)
                }
                val cp = g[i]
                if (cp != ' '.code && cp != 0) {
                    var color = f[i]
                    if ((a and A_DIM) != 0) color = (color and 0x00FFFFFF) or (0xA0 shl 24)
                    if (boldBright && (a and A_BOLD) != 0) color = brighten(color)
                    textPaint.color = color
                    textPaint.isFakeBoldText = (a and A_BOLD) != 0
                    textPaint.isUnderlineText = (a and A_UNDERLINE) != 0
                    textPaint.isStrikeThruText = (a and A_STRIKE) != 0
                    textPaint.textSkewX = if ((a and A_ITALIC) != 0) -0.25f else 0f
                    val text = if (combining && (a and A_COMBINING) != 0)
                        (session?.cellGrapheme(r, c) ?: String(Character.toChars(cp)))
                    else String(Character.toChars(cp))
                    canvas.drawText(text, x, y + baseline, textPaint)
                }
            }
        }
        if (drawCursor && cursorCol in 0 until cols * rows) {
            val cr = cursorCol / cols
            val cc = cursorCol % cols
            val x = padX + cc * charW; val y = padY + cr * lineH
            cursorPaint.color = curColor
            val d = resources.displayMetrics.density
            when (curStyle) {
                CursorStyle.BLOCK -> {
                    canvas.drawRect(x, y, x + charW, y + lineH, cursorPaint)
                    val cp = if (cursorCol < g.size) g[cursorCol] else 0
                    if (cp != ' '.code && cp != 0) {
                        textPaint.color = DEFAULT_BG; textPaint.textSkewX = 0f
                        canvas.drawText(String(Character.toChars(cp)), x, y + baseline, textPaint)
                    }
                }
                CursorStyle.BAR -> canvas.drawRect(x, y, x + max(1.5f * d, 2f), y + lineH, cursorPaint)
                CursorStyle.UNDERLINE -> canvas.drawRect(x, y + lineH - max(1.5f * d, 2f), x + charW, y + lineH, cursorPaint)
                CursorStyle.HOLLOW -> {
                    val t = max(1f * d, 1.5f)
                    canvas.drawRect(x, y, x + charW, y + t, cursorPaint)
                    canvas.drawRect(x, y + lineH - t, x + charW, y + lineH, cursorPaint)
                    canvas.drawRect(x, y, x + t, y + lineH, cursorPaint)
                    canvas.drawRect(x + charW - t, y, x + charW, y + lineH, cursorPaint)
                }
            }
        }
    }

    private fun brighten(c: Int): Int {
        val a = (c ushr 24) and 0xFF
        fun up(v: Int) = (v + (255 - v) * 40 / 100).coerceIn(0, 255)
        val r = up((c ushr 16) and 0xFF); val g = up((c ushr 8) and 0xFF); val bl = up(c and 0xFF)
        return (a shl 24) or (r shl 16) or (g shl 8) or bl
    }

    // ---- touch: selection, history scroll, mouse reporting ----
    private fun cellCol(x: Float) = ((x - padX) / charW).toInt().coerceIn(0, max(0, cols - 1))
    private fun cellRow(y: Float) = ((y - padY) / lineH).toInt().coerceIn(0, max(0, rows - 1))

    private fun beginSelection() {
        val s = session ?: return
        // No mouseMode guard any more. The long-press timer is only armed on the TOUCH path
        // (ACTION_DOWN returns early for a real pointer), and finger drags no longer report
        // button events, so without this a long press inside htop/vim would do nothing at all.
        // Termux behaves the same way: onLongPress always enters text-selection mode.
        selecting = true
        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
        s.selectStart(cellRow(downY), cellCol(downX))   // anchor at the exact cell
        uiRefresh()
    }

    /**
     * Route one vertical scroll gesture. Mirrors Termux's `TerminalView.doScroll()`
     * (terminal-view/src/main/java/com/termux/view/TerminalView.java) and its three cases:
     *
     *  1. mouse tracking on  → report WHEEL buttons, so the program scrolls itself;
     *  2. alternate screen   → send Arrow Up/Down, so nano/vim/less/htop move their own view
     *                          (they own the display and there is no history of ours to move);
     *  3. otherwise          → move our scrollback viewport, as before.
     *
     * [lines] > 0 means the finger dragged DOWN, i.e. the user is asking for EARLIER content,
     * which is Arrow Up / wheel-up. Cases 1 and 2 are capped per MOVE event so one fast drag
     * cannot flood the PTY with hundreds of keystrokes; case 3 keeps the exact line count
     * because the viewport is ours and moving it is free.
     */
    private fun applyScrollGesture(lines: Int, x: Float, y: Float) {
        val s = session ?: return
        val up = lines > 0
        when {
            mouseMode != 0 -> {
                val button = if (up) MOUSE_WHEEL_UP else MOUSE_WHEEL_DOWN
                val col = cellCol(x); val row = cellRow(y)
                repeat(min(abs(lines), MAX_KEYS_PER_MOVE)) { s.mouse(button, col, row, 0) }
            }
            onAlt -> {
                val bytes = TermKeys.arrow(if (up) "UP" else "DOWN", appCursor) ?: return
                repeat(min(abs(lines), MAX_KEYS_PER_MOVE)) { s.write(bytes) }
            }
            else -> s.scroll(lines)
        }
        uiRefresh()
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        // While the xset dashboard overlay is active the PTY is idle and hidden; swallow touch so a
        // swipe/pinch/long-press cannot leak through and scroll/select/resize the terminal behind it.
        // The dashboard itself is keyboard-driven. Normal terminal touch handling is unchanged when closed.
        if (dashboard?.active == true) return true
        val s = session ?: return false
        scaleDetector.onTouchEvent(event)
        if (scaleDetector.isInProgress) {          // pinch-zoom owns the gesture
            removeCallbacks(longPressRunnable); selecting = false; scrolling = false
            return true
        }
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.x; downY = event.y; lastScrollY = event.y
                selecting = false; scrolling = false; scrollAccum = 0f
                // Raw button reporting is for a real pointer only. A finger is not a mouse: sending
                // press/drag/release for touch made a swipe in vim (`set mouse=a`) start a visual
                // selection instead of scrolling. Touch always goes through the gesture path below,
                // exactly as Termux does (TerminalView.onTouchEvent gates on SOURCE_MOUSE).
                if (mouseMode != 0 && event.isFromSource(InputDevice.SOURCE_MOUSE)) {
                    mouseCol = cellCol(event.x); mouseRow = cellRow(event.y)
                    s.mouse(0, mouseCol, mouseRow, 0)
                    return true
                }
                postDelayed(longPressRunnable, longPressTimeout)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (mouseMode != 0 && event.isFromSource(InputDevice.SOURCE_MOUSE)) {
                    val c = cellCol(event.x); val r = cellRow(event.y)   // engine filters 1000/1002/1003
                    if (c != mouseCol || r != mouseRow) { mouseCol = c; mouseRow = r; s.mouse(0, c, r, 2) }
                    return true
                }
                val dx = event.x - downX; val dy = event.y - downY
                if (selecting) {
                    s.selectExtend(cellRow(event.y), cellCol(event.x)); uiRefresh()
                    return true
                }
                if (!scrolling && (abs(dx) > touchSlop || abs(dy) > touchSlop)) {
                    scrolling = true
                    removeCallbacks(longPressRunnable)
                }
                if (scrolling) {
                    // Accumulate the delta since the PREVIOUS move, not since the DOWN anchor. The
                    // anchor was only re-based on whole-line consumption before, so intra-line moves
                    // were re-counted from the stale anchor every frame → the grid scrolled up to ~2x
                    // faster than the finger (jumpy). Per-move deltas make the scroll gain 1:1 while
                    // the native line-granular scrollback semantics (scrollback, scroll-to-bottom,
                    // selection) stay untouched.
                    scrollAccum += (event.y - lastScrollY)
                    val lines = (scrollAccum / lineH).toInt()
                    if (lines != 0) { applyScrollGesture(lines, event.x, event.y); scrollAccum -= lines * lineH }
                }
                lastScrollY = event.y
                return true
            }
            MotionEvent.ACTION_UP -> {
                if (mouseMode != 0 && event.isFromSource(InputDevice.SOURCE_MOUSE)) {
                    s.mouse(0, cellCol(event.x), cellRow(event.y), 1)   // report the button release
                    return true
                }
                removeCallbacks(longPressRunnable)
                val moved = abs(event.x - downX) > touchSlop || abs(event.y - downY) > touchSlop
                if (selecting) {
                    if (!moved) s.selectWord(cellRow(event.y), cellCol(event.x)) // no drag -> whole word
                    uiRefresh()
                    val text = s.selectionText()
                    if (text.isNotEmpty()) onClipboardCopy?.invoke(text)
                    selecting = false
                    return true
                }
                if (!scrolling && !moved) {
                    val now = System.currentTimeMillis()
                    if (now - lastTapAt < 300L) {          // double-tap -> reset zoom
                        lastTapAt = 0L; resetZoom()
                    } else {
                        lastTapAt = now
                        // A TAP (not a drag) still counts as a click for a mouse-tracking program,
                        // so tapping a menu in htop/vim keeps working now that finger DRAGS scroll
                        // instead of dragging a button. Same split as Termux's onSingleTapUp.
                        if (mouseMode != 0) {
                            val c = cellCol(event.x); val r = cellRow(event.y)
                            s.mouse(0, c, r, 0); s.mouse(0, c, r, 1)
                        }
                        s.clearSelection(); s.scrollToBottom(); uiRefresh(); requestFocusAndKeyboard()
                    }
                }
                scrolling = false
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                removeCallbacks(longPressRunnable); selecting = false; scrolling = false
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    // ---- input ----
    fun sendSpecial(name: String) {
        val d = dashboard
        if (d != null && d.active) {
            d.onSpecial(name)
            if (ctrlActive) { ctrlActive = false; onModifierConsumed?.invoke("CTRL") }
            if (altActive) { altActive = false; onModifierConsumed?.invoke("ALT") }
            repaintDash(); return
        }
        val ctrl = ctrlActive; val alt = altActive
        // xterm modifier param: 1 + shift(1) + alt(2) + ctrl(4). Only ctrl/alt are stickable here.
        val mod = 1 + (if (alt) 2 else 0) + (if (ctrl) 4 else 0)
        val bytes: ByteArray = if (mod > 1) {
            TermKeys.modified(name, mod) ?: run {
                // No CSI-modified form for this key (ESC/TAB/…): fall back to plain, alt-prefixed.
                val plain = TermKeys.arrow(name, appCursor) ?: TermKeys.special(name) ?: return
                if (alt) TermKeys.alt(plain) else plain
            }
        } else {
            TermKeys.arrow(name, appCursor) ?: TermKeys.special(name) ?: return
        }
        session?.scrollToBottom()
        session?.write(bytes)
        if (ctrl) { ctrlActive = false; onModifierConsumed?.invoke("CTRL") }
        if (alt)  { altActive = false; onModifierConsumed?.invoke("ALT") }
    }

    private fun sendChar(c: Char) {
        val d = dashboard
        if (d != null && d.active) {
            d.onChar(c, ctrlActive, altActive)
            if (ctrlActive) { ctrlActive = false; onModifierConsumed?.invoke("CTRL") }
            if (altActive) { altActive = false; onModifierConsumed?.invoke("ALT") }
            repaintDash(); return
        }
        val s = session ?: return
        s.scrollToBottom()
        when {
            ctrlActive -> { s.write(byteArrayOf(TermKeys.ctrl(c))); ctrlActive = false; onModifierConsumed?.invoke("CTRL") }
            altActive  -> { s.write(TermKeys.alt(c.toString().toByteArray(Charsets.UTF_8))); altActive = false; onModifierConsumed?.invoke("ALT") }
            else       -> s.writeText(c.toString())
        }
    }

    fun sendText(text: CharSequence) {
        if (text.isEmpty()) return
        val d = dashboard
        if (d != null && d.active) {
            for (ch in text) d.onChar(ch, false, false)
            repaintDash(); return
        }
        if (ctrlActive || altActive) { sendChar(text[0]); if (text.length > 1) session?.writeText(text.substring(1)) }
        else session?.writeText(text.toString())
    }

    /**
     * Paste text, honouring bracketed-paste mode negotiated by the program.
     *
     * The dashboard is checked first, for the same reason [sendChar] and [sendText] check it: while
     * an overlay is up the shell behind it is idle and completely hidden. Writing there put the
     * clipboard on a command line nobody could see, and a clipboard ending in a newline ran it.
     * The overlay gets the text only where it means text (its search box); anywhere else a paste
     * is dropped rather than replayed as one hotkey per character.
     */
    fun paste(text: String) {
        val d = dashboard
        if (d != null && d.active) {
            if (d.acceptsText) { for (ch in text) d.onChar(ch, false, false); repaintDash() }
            return
        }
        session?.scrollToBottom(); session?.paste(text)
    }

    /** Find the previous occurrence of [query] in scrollback+screen and reveal it. Returns true on hit. */
    fun find(query: String): Boolean {
        val s = session ?: return false
        if (query.isEmpty()) return false
        if (query != searchQuery) { searchQuery = query; searchRow = Int.MAX_VALUE }
        val from = if (searchRow == Int.MAX_VALUE) Int.MAX_VALUE else searchRow - 1
        val hit = s.search(query, from, false)   // search backward (toward older history)
        if (hit >= 0) { searchRow = hit; uiRefresh(); return true }
        searchRow = Int.MAX_VALUE                 // wrap for the next call
        return false
    }

    override fun onCheckIsTextEditor(): Boolean = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = InputType.TYPE_NULL
        outAttrs.imeOptions = EditorInfo.IME_ACTION_NONE or EditorInfo.IME_FLAG_NO_EXTRACT_UI or
                EditorInfo.IME_FLAG_NO_FULLSCREEN
        return object : BaseInputConnection(this, false) {
            override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean { sendText(text); return true }
            override fun setComposingText(text: CharSequence, newCursorPosition: Int): Boolean { sendText(text); return true }
            override fun deleteSurroundingText(before: Int, after: Int): Boolean {
                repeat(before) { sendSpecial("BKSP") }; repeat(after) { sendSpecial("DEL") }; return true
            }
            override fun sendKeyEvent(event: KeyEvent): Boolean {
                if (event.action == KeyEvent.ACTION_DOWN) return this@TerminalView.onKeyDown(event.keyCode, event)
                return true
            }
            override fun performEditorAction(actionCode: Int): Boolean { sendSpecial("ENTER"); return true }
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        // xset dashboard: the only combo the sendSpecial() path can't express is Shift+Tab.
        val d = dashboard
        if (d != null && d.active && keyCode == KeyEvent.KEYCODE_TAB && event.isShiftPressed) {
            d.onSpecial("BACKTAB"); repaintDash(); return true
        }
        val handled = when (keyCode) {
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> { sendSpecial("ENTER"); true }
            KeyEvent.KEYCODE_DEL          -> { sendSpecial("BKSP"); true }
            KeyEvent.KEYCODE_FORWARD_DEL  -> { sendSpecial("DEL"); true }
            KeyEvent.KEYCODE_TAB          -> { sendSpecial("TAB"); true }
            KeyEvent.KEYCODE_ESCAPE       -> { sendSpecial("ESC"); true }
            KeyEvent.KEYCODE_DPAD_UP      -> { sendSpecial("UP"); true }
            KeyEvent.KEYCODE_DPAD_DOWN    -> { sendSpecial("DOWN"); true }
            KeyEvent.KEYCODE_DPAD_LEFT    -> { sendSpecial("LEFT"); true }
            KeyEvent.KEYCODE_DPAD_RIGHT   -> { sendSpecial("RIGHT"); true }
            KeyEvent.KEYCODE_MOVE_HOME    -> { sendSpecial("HOME"); true }
            KeyEvent.KEYCODE_MOVE_END     -> { sendSpecial("END"); true }
            KeyEvent.KEYCODE_PAGE_UP      -> { sendSpecial("PGUP"); true }
            KeyEvent.KEYCODE_PAGE_DOWN    -> { sendSpecial("PGDN"); true }
            else -> {
                val u = event.unicodeChar
                if (u != 0) { sendChar(u.toChar()); true } else false
            }
        }
        return if (handled) true else super.onKeyDown(keyCode, event)
    }

    override fun onFocusChanged(gainFocus: Boolean, direction: Int, previouslyFocusedRect: android.graphics.Rect?) {
        super.onFocusChanged(gainFocus, direction, previouslyFocusedRect)
        if (focusReporting) session?.write((if (gainFocus) "\u001B[I" else "\u001B[O").toByteArray(Charsets.UTF_8))
    }

}
