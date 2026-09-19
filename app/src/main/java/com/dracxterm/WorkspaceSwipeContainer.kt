package com.dracxterm

import android.content.Context
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.ViewConfiguration
import android.widget.FrameLayout
import kotlin.math.abs

/**
 * Wraps the TerminalView and steals only strong, clearly-horizontal single-finger swipes to
 * switch workspaces. It deliberately does not intercept:
 *   - multi-finger gestures  (pinch-to-zoom is preserved),
 *   - vertical-dominant drags (history scroll is preserved),
 *   - small movements        (long-press text selection is preserved).
 * The high threshold keeps the terminal's own gestures intact; a swipe must be well beyond
 * touch-slop and mostly horizontal before this container claims it.
 */
class WorkspaceSwipeContainer @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : FrameLayout(context, attrs) {

    var onSwipeLeft: (() -> Unit)? = null    // right -> left  (next workspace)
    var onSwipeRight: (() -> Unit)? = null   // left  -> right (previous workspace)

    private val slop = ViewConfiguration.get(context).scaledTouchSlop
    private val trigger = slop * 3
    private var downX = 0f
    private var downY = 0f
    private var claimed = false

    override fun onInterceptTouchEvent(ev: MotionEvent): Boolean {
        if (ev.pointerCount > 1) return false          // never steal pinch-zoom
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> { downX = ev.x; downY = ev.y; claimed = false }
            MotionEvent.ACTION_MOVE -> {
                val dx = ev.x - downX; val dy = ev.y - downY
                if (!claimed && abs(dx) > trigger && abs(dx) > abs(dy) * 2f) {
                    claimed = true
                    return true
                }
            }
        }
        return false
    }

    override fun onTouchEvent(ev: MotionEvent): Boolean {
        if (ev.actionMasked == MotionEvent.ACTION_UP && claimed) {
            val dx = ev.x - downX
            if (dx <= -trigger) onSwipeLeft?.invoke()
            else if (dx >= trigger) onSwipeRight?.invoke()
            claimed = false
            return true
        }
        return claimed || super.onTouchEvent(ev)
    }
}
