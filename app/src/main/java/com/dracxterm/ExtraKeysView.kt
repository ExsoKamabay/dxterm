package com.dracxterm

import android.content.Context
import android.graphics.Color
import android.util.AttributeSet
import android.util.TypedValue
import android.view.Gravity
import android.widget.Button
import android.widget.HorizontalScrollView
import android.widget.LinearLayout

/**
 * The bottom extra-keys toolbar shown above the IME. Momentary keys fire onKey;
 * CTRL / ALT are sticky toggles reported via onModifier.
 */
class ExtraKeysView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : HorizontalScrollView(context, attrs) {

    interface Listener {
        fun onKey(name: String)                       // ESC TAB UP DOWN LEFT RIGHT HOME END PGUP PGDN FIND PASTE BKSP
        fun onModifier(name: String, active: Boolean) // CTRL ALT
    }

    var listener: Listener? = null

    private val row = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        // Extra trailing inset so the final key keeps a margin at the right edge (never looks
        // "cut off"); the fading edge below signals the row scrolls to reveal the rest.
        setPadding(dp(6), dp(6), dp(16), dp(6))
        clipToPadding = false
    }

    // label -> logical name; sticky flag
    private data class Key(val label: String, val name: String, val sticky: Boolean = false)

    private val keys = listOf(
        // Order mirrors the reference toolbar: arrows, zoom, CTRL, then the rest (scrollable).
        Key("▲", "UP"), Key("▼", "DOWN"), Key("⇩", "SCROLL_BOTTOM"),
        Key("⊖", "ZOOM_OUT"), Key("⊕", "ZOOM_IN"),
        Key("CTRL", "CTRL", sticky = true),
        Key("◀", "LEFT"), Key("▶", "RIGHT"), Key("ALT", "ALT", sticky = true),
        Key("ESC", "ESC"), Key("TAB", "TAB"),
        Key("HOME", "HOME"), Key("END", "END"),
        Key("PGUP", "PGUP"), Key("PGDN", "PGDN"),
        Key("FIND", "FIND"), Key("PASTE", "PASTE"),
        Key("⌫", "BKSP")
    )

    private val activeModifiers = mutableSetOf<String>()

    init {
        isHorizontalScrollBarEnabled = false
        isHorizontalFadingEdgeEnabled = true      // visible hint that the toolbar scrolls
        setFadingEdgeLength(dp(24))
        clipToPadding = false
        addView(row)
        keys.forEach { row.addView(makeButton(it)) }
    }

    private fun makeButton(key: Key): Button {
        return Button(context).apply {
            text = key.label
            isAllCaps = false
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            setTextColor(Color.parseColor("#22D3EE"))
            setBackgroundResource(R.drawable.key_bg)
            minWidth = dp(44); minHeight = dp(40)
            val lp = LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT).apply { marginEnd = dp(6) }
            layoutParams = lp
            gravity = Gravity.CENTER
            setPadding(dp(12), 0, dp(12), 0)
            setOnClickListener { onKeyTapped(key, this) }
        }
    }

    private fun onKeyTapped(key: Key, button: Button) {
        if (key.sticky) {
            val nowActive = !activeModifiers.contains(key.name)
            if (nowActive) activeModifiers.add(key.name) else activeModifiers.remove(key.name)
            button.setTextColor(if (nowActive) Color.parseColor("#8A5CF6") else Color.parseColor("#22D3EE"))
            listener?.onModifier(key.name, nowActive)
        } else {
            listener?.onKey(key.name)
        }
    }

    /** Called by the host after a modifier was consumed by a real keystroke, to un-stick it. */
    fun clearModifier(name: String) {
        if (activeModifiers.remove(name)) {
            for (i in 0 until row.childCount) {
                val b = row.getChildAt(i) as? Button ?: continue
                if (keys.getOrNull(i)?.name == name) b.setTextColor(Color.parseColor("#22D3EE"))
            }
        }
    }

    private fun dp(v: Int): Int =
        (v * resources.displayMetrics.density).toInt()
}
