package com.dracxterm

import android.content.Context
import android.os.Handler
import android.os.Looper
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * Owns up to [MAX] terminal workspaces. Each workspace is a fully independent [TerminalSession]
 * (its own PTY, shell, current directory, scrollback and viewport, held natively), so switching
 * is only a matter of rebinding the single shared [TerminalView] to another session.
 *
 * Sessions are created and started lazily the first time a workspace is shown, at the current
 * grid size, and configured through [configure] (colour theme). Workspace 1 adopts the session
 * MainActivity already created, so the single-workspace path is byte-for-byte the old behaviour.
 *
 * STARTUP THREADING (anti-jank / anti-flicker root-cause fix):
 *  The lazy `session.start()` runs `Bootstrap.prepare()` (home seeding, user provisioning, compat
 *  shim install, real file I/O every launch) followed by `forkpty` + proot exec. Doing that on the
 *  UI thread stalls the Choreographer, which is the "heavy transition" felt when opening Workspaces
 *  2..5 and also widens the window in which the empty grid can flash. Here the heavy start is moved
 *  to a single-thread background [starter]; only the cheap, UI-owning steps (theme configure + the
 *  atomic [TerminalView.switchSession] rebind) run back on the main thread once the PTY exists. The
 *  view's existing "hold the previous frame until first output" logic then performs one atomic
 *  present, so no half-ready session is ever shown. A monotonic [switchSeq] discards a stale start
 *  whose target is no longer the active workspace (rapid tab taps / swipes).
 */
class WorkspaceManager(
    private val ctx: Context,
    private val terminal: TerminalView,
    private val configure: (TerminalSession) -> Unit,
    private val onChanged: () -> Unit
) {
    companion object { const val MAX = 5 }

    // started: the async start has completed successfully. starting: a start is in flight on the
    // starter thread. Without the in-flight flag, re-selecting a workspace before its first start
    // finishes re-enters the !started branch and calls session.start() a SECOND time on the same
    // session, overwriting its native handle without closing the first -> a leaked PTY + reader
    // thread + proot/bash process tree that nothing can ever destroy.
    private class Slot(var session: TerminalSession) {
        var started = false; var starting = false
        // A command to type into the session once its shell is up (e.g. the activity monitor), and
        // whether this slot is that monitor tab (so only one is ever opened).
        var pendingInput: String? = null
        var isWatcher = false
    }

    private val slots = ArrayList<Slot>()
    var active = 0; private set

    // Serialises the heavy proot spawns off the UI thread; results are posted back to the main thread.
    private val starter: ExecutorService = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    // Bumped on every switch. A background start only presents if its captured value still matches,
    // so a superseded start becomes a ready-but-unshown session in its slot (instant on a later visit).
    private var switchSeq = 0
    // NOTE: Storage Access no longer forces any session respawn. Storage is grafted once at spawn to a
    // hidden backing tree and toggled live via home symlinks on the running shell (see Bootstrap
    // .applyStorageVisibility), so switching a storage state preserves every session's identity
    // (native handle / PTY / process). There is deliberately no storage epoch here anymore.

    val count get() = slots.size
    fun canAdd() = slots.size < MAX
    fun isActive(index: Int) = index == active
    fun activeSession(): TerminalSession = slots[active].session

    /** Every workspace's session, so a global setting reaches all of them and not only the one on
     *  screen. Sessions are configured once at start, so without this a theme change left every
     *  other open workspace on the previous colours for the rest of its life. */
    fun forEachSession(block: (TerminalSession) -> Unit) { for (s in slots) block(s.session) }

    /** Adopt MainActivity's first session as workspace 1. */
    fun adopt(first: TerminalSession, started: Boolean) {
        slots.clear()
        slots.add(Slot(first).apply { this.started = started })
        active = 0
        onChanged()
    }

    /** Flag workspace 1 as started once MainActivity's initial start() succeeds. */
    fun markActiveStarted() { if (slots.isNotEmpty()) slots[active].started = true }

    fun addWorkspace() {
        if (!canAdd()) return
        slots.add(Slot(TerminalSession(ctx)))
        switchTo(slots.size - 1)
    }

    /**
     * Open a background tab that runs [cmd] (e.g. `claw watch\n`) for the live activity monitor,
     * WITHOUT stealing focus from the current workspace, so the user can keep typing where they are
     * and tap the new chip when they want to watch. At most one monitor tab exists at a time.
     * Returns false when the tab limit is reached or a monitor is already open.
     */
    fun addWatcherTab(cmd: String): Boolean {
        if (!canAdd()) return false
        if (slots.any { it.isWatcher }) return false
        val c = terminal.gridCols(); val r = terminal.gridRows()
        if (c <= 0 || r <= 0) return false
        val slot = Slot(TerminalSession(ctx)).apply { pendingInput = cmd; isWatcher = true }
        slots.add(slot)
        onChanged()                              // show the new chip; active is unchanged
        // Start it without bumping switchSeq: active != this slot, so it will not present, but it
        // does start and then types its command from the success post above.
        startInBackground(slot, slot.session, c, r, switchSeq)
        return true
    }

    /** Close the background activity-monitor tab if one is open (humanizer turned off). Returns
     *  whether a watcher tab was found and removed. Rebinds the view to a live workspace after. */
    fun closeWatcherTab(): Boolean {
        val idx = slots.indexOfFirst { it.isWatcher }
        if (idx < 0) return false
        val watcher = slots.removeAt(idx).session
        if (active > idx) active-- else if (active >= slots.size) active = slots.size - 1
        if (active < 0) active = 0
        onChanged()
        // Rebind the shared view to a live workspace BEFORE closing the monitor's session. Closed
        // while still bound, the view sees its session stop and fires onExit, which on the main
        // workspace means finish(): the whole app closed when the user left the monitor with Ctrl+C.
        switchTo(active)
        runCatching { watcher.close() }
        return true
    }

    fun switchTo(index: Int) {
        if (index !in slots.indices) return
        active = index
        onChanged()                        // reflect the selected tab immediately (cheap, incremental)
        val slot = slots[index]
        // A start is already in flight for this slot. Do NOT launch a second one (that leaks the
        // first session's native handle). The in-flight start presents itself on completion if this
        // is still the active workspace; switchSeq is deliberately NOT bumped here so it can.
        if (slot.starting) return
        val seq = ++switchSeq
        val c = terminal.gridCols(); val r = terminal.gridRows()
        when {
            !slot.started ->
                // First visit: start off the UI thread, then present atomically when the PTY exists.
                startInBackground(slot, slot.session, c, r, seq)
            !slot.session.running() -> {
                // Respawn this workspace only when its shell has exited (a dead session would otherwise
                // show a frozen screen on return). Storage toggles never reach here, they are applied
                // live to the running shell without a respawn.
                runCatching { slot.session.close() }
                val fresh = TerminalSession(ctx)
                slot.session = fresh
                slot.started = false
                startInBackground(slot, fresh, c, r, seq)
            }
            else ->
                // Already live with content: instant rebind (the view refreshes immediately, gen > 0).
                terminal.switchSession(slot.session)
        }
    }

    /**
     * Run the heavy [TerminalSession.start] on the [starter] thread, then hop back to the main thread
     * to configure the theme and, only if this start is still the current target, perform the atomic
     * [TerminalView.switchSession] present. Slot bookkeeping and every TerminalView touch happen on the
     * main thread; the background thread only owns the blocking start of [session].
     */
    private fun startInBackground(slot: Slot, session: TerminalSession, c: Int, r: Int, seq: Int) {
        if (c <= 0 || r <= 0) { slot.started = false; return }
        slot.starting = true                 // block a concurrent re-entry from starting this slot again
        starter.execute {
            val ok = runCatching { session.start(c, r) }.getOrDefault(false)
            main.post {
                slot.starting = false
                val curIdx = slots.indexOf(slot)
                if (curIdx < 0) { if (ok) runCatching { session.close() } ; return@post }  // slot removed meanwhile
                slot.started = ok
                if (ok) configure(session)
                // Type a queued command (e.g. `claw watch`) once the shell has had a moment to come
                // up. Runs whether or not this slot is the visible one, so a background monitor tab
                // starts working immediately.
                if (ok) slot.pendingInput?.let { cmd ->
                    slot.pendingInput = null
                    main.postDelayed({ runCatching { session.writeText(cmd) } }, 900)
                }
                // Present only if this is still the active target and no newer switch superseded it.
                if (ok && seq == switchSeq && active == curIdx) terminal.switchSession(session)
            }
        }
    }

    fun next() { if (slots.size > 1) switchTo((active + 1) % slots.size) }
    fun prev() { if (slots.size > 1) switchTo((active - 1 + slots.size) % slots.size) }

    /** Workspace 1 (slot 0) is the main workspace, it is never auto-removed on shell exit. */
    fun isMain(index: Int) = index == 0

    /**
     * A SECONDARY workspace's shell has exited (`exit`/`logout`). Fully tear it down, close the
     * session (stops the PTY and frees native resources), drop the slot, then switch to the
     * PREVIOUS still-live workspace. No-op guard for the main/last slot: that case is handled by the
     * caller so the main workspace can never disappear by accident.
     */
    fun removeActiveToPrev() {
        if (slots.size <= 1 || active == 0) return   // never remove the last one or the main here
        val idx = active
        runCatching { slots[idx].session.close() }   // stop PTY + free native session
        slots.removeAt(idx)
        // Land on the previous workspace (or the first if we removed slot 1).
        switchTo((idx - 1).coerceIn(0, slots.size - 1))
    }

    /**
     * Restart the active workspace's shell in place with a fresh session, keeping the slot. Used when
     * the MAIN workspace's shell exits while other workspaces still exist: the main workspace must not
     * be lost, so instead of removing it we give it a new live shell. The heavy start runs off the UI
     * thread (same path as a first open) so the recovery does not stall the frame either.
     */
    fun restartActive() {
        val slot = slots[active]
        if (slot.starting) return            // a start is already in flight; let it produce the live shell
        runCatching { slot.session.close() }
        val fresh = TerminalSession(ctx)
        slot.session = fresh
        slot.started = false
        val seq = ++switchSeq
        val c = terminal.gridCols(); val r = terminal.gridRows()
        startInBackground(slot, fresh, c, r, seq)
        onChanged()
    }

    // NOTE: A former onStorageConfigChanged() respawned the active session on every Storage-Access
    // toggle. It has been removed: storage is now grafted once at spawn and toggled live on the
    // running shell (Bootstrap.applyStorageVisibility), so a toggle preserves the active session's
    // identity (native handle / PTY / process) with no respawn. restartActive() above is retained
    // strictly for MAIN-workspace shell-exit recovery, a path unrelated to storage.

    /** Close every session (called from Activity.onDestroy). */
    fun closeAll() {
        starter.shutdownNow()                        // stop pending/late starts; no orphan spawns
        slots.forEach { runCatching { it.session.close() } }
        slots.clear()
    }
}
