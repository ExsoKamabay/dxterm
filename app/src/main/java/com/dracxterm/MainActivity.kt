package com.dracxterm

import android.content.ClipData
import android.content.ClipboardManager
import android.widget.EditText
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import androidx.activity.result.contract.ActivityResultContracts
import android.view.HapticFeedbackConstants
import android.widget.Toast
import android.graphics.Typeface
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsAnimationCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.dracxterm.databinding.ActivityMainBinding
import com.dracxterm.xset.CursorStyle
import com.dracxterm.xset.DEVELOPER_EMAIL
import com.dracxterm.xset.XsetBootstrap
import com.dracxterm.xset.XsetContext
import com.dracxterm.xset.XsetController
import com.dracxterm.xset.XsetDefaults
import com.dracxterm.xset.XsetStore
import java.io.File

class MainActivity : AppCompatActivity() {

    /** Start in the saved language. Without this the terminal side would keep whatever locale
     *  the device is in, while the provisioning screen honoured the user's choice. */
    override fun attachBaseContext(newBase: Context) {
        super.attachBaseContext(LocaleSupport.wrap(newBase))
    }

    /** Rotation and resize are handled in place (see the manifest), and the framework hands the
     *  activity the system configuration, device locale included. Re-apply the saved language so
     *  strings read afterwards (toasts, the find dialog) stay in the language the user chose. */
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        LocaleSupport.applyInPlace(this)
    }


    private lateinit var binding: ActivityMainBinding
    private lateinit var first: TerminalSession
    private lateinit var workspaces: WorkspaceManager
    private var sessionStarted = false

    // Workspace tab-bar diff state: the last-rendered structure. When only the active index changes
    // (a plain switch), the bar is restyled in place instead of rebuilt (see renderWorkspaceBar).
    private var renderedWorkspaceCount = -1
    private var renderedCanAdd = false

    // xset framework
    private lateinit var xsetStore: XsetStore
    private lateinit var xset: XsetController

    // ---- Storage Access (Settings ▸ Storage Access) ----
    // Registered as fields so they are ready before the activity is STARTED. Both re-stage the
    // ~/storage shortcuts and report the resulting state; neither can crash the terminal.
    private val requestStoragePerms =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { onStoragePermissionResult() }
    private val requestAllFilesAccess =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { onStoragePermissionResult() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        // Retarget this activity's resources at the saved language before anything is inflated.
        // attachBaseContext alone is not enough: AppCompatActivity re-applies its own
        // configuration on top of the base it is handed, which put the locale back to the
        // device's and left a restarted app showing the language the user had switched away
        // from while the toggle correctly reported the other one.
        LocaleSupport.applyInPlace(this)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // ---- xset framework: store (SharedPreferences-backed) + module registry + dashboard ----
        xsetStore = XsetStore(loader = ::loadXsetPrefs, saver = ::saveXsetPrefs)
        XsetDefaults.register(xsetStore)
        XsetBootstrap.install()
        xset = XsetController(makeXsetContext())
        binding.terminal.dashboard = xset

        first = TerminalSession(this)
        binding.terminal.attach(first)
        workspaces = WorkspaceManager(this, binding.terminal, ::configureSession) { renderWorkspaceBar() }
        workspaces.adopt(first, started = false)

        // Start workspace 1 once the terminal reports its first geometry, so the PTY winsize
        // matches the real character grid from the very first spawn. After that the TerminalView
        // resizes whichever session is bound (the active workspace) directly.
        binding.terminal.onGeometry = { cols, rows ->
            if (!sessionStarted) {
                sessionStarted = first.start(cols, rows)
                if (sessionStarted) {
                    configureSession(first)
                    workspaces.markActiveStarted()
                    applyXsetOnStart()
                } else {
                    Toast.makeText(this, getString(R.string.app_name) + ": start failed",
                        Toast.LENGTH_LONG).show()
                }
            }
        }

        binding.terminal.onModifierConsumed = { name -> binding.extraKeys.clearModifier(name) }
        // Zoom owns the live font size; the xset Font module only displays it. Without this the
        // row kept the value captured when the dashboard opened, so zooming while it was open
        // showed a stale number and adjusting the row snapped the font back to it.
        binding.terminal.onFontSizeChanged = { dp -> xsetStore.set("font.size", dp.toString()) }
        binding.terminal.onTitle = { /* per-session title; workspace chips show the index */ }
        // Bell (BEL / visual bell) -> a light haptic tick.
        binding.terminal.onBell = {
            binding.terminal.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
        }
        // Selection copy and OSC 52 clipboard writes -> the Android clipboard.
        binding.terminal.onClipboardCopy = { text -> copyToClipboard(text) }
        // Private app-control channel (OSC 5391): the guest `xset` command opens the dashboard.
        binding.terminal.onAppControl = { payload -> handleAppControl(payload) }
        // Shell exit (`exit`/`logout`) in the active workspace. Secondary workspaces (2..5) are torn
        // down, session closed, PTY stopped, slot removed, and focus moves to the previous
        // workspace. The MAIN workspace (1) is never dropped: if it is the only one the session is
        // over and the app closes normally (finish() -> onDestroy -> workspaces.closeAll()); if other
        // workspaces still exist its shell is restarted in place so main can't be lost by accident.
        binding.terminal.onExit = {
            when {
                !workspaces.isMain(workspaces.active) -> workspaces.removeActiveToPrev()
                workspaces.count <= 1 -> finish()
                else -> workspaces.restartActive()
            }
        }

        // Horizontal swipe over the terminal switches workspace (right->left = next).
        binding.swipeContainer.onSwipeLeft = { workspaces.next() }
        binding.swipeContainer.onSwipeRight = { workspaces.prev() }

        renderWorkspaceBar()

        binding.extraKeys.listener = object : ExtraKeysView.Listener {
            override fun onKey(name: String) {
                when (name) {
                    "PASTE" -> pasteFromClipboard()
                    // FIND means "search what is on screen". While the dashboard owns the screen
                    // that is the dashboard's own list, not the scrollback underneath it: the
                    // buffer dialog opened over the overlay and searched text the user could not
                    // see. '/' is the dashboard's search, so the button keeps its meaning.
                    "FIND"  -> if (binding.terminal.dashboardActive()) binding.terminal.sendText("/")
                               else showFindDialog()
                    "ZOOM_IN"  -> binding.terminal.zoomIn()
                    "ZOOM_OUT" -> binding.terminal.zoomOut()
                    "SCROLL_BOTTOM" -> binding.terminal.toBottom()
                    else -> binding.terminal.sendSpecial(name)
                }
                binding.terminal.requestFocus()
            }
            override fun onModifier(name: String, active: Boolean) {
                when (name) {
                    "CTRL" -> binding.terminal.setCtrl(active)
                    "ALT"  -> binding.terminal.setAlt(active)
                }
            }
        }

        binding.btnKeyboard.setOnClickListener { toggleKeyboard() }

        applyInsets()
        syncImeAnimation()

        // Pin the process in the foreground so switching to another app does not let the OS
        // reclaim it and kill the shell/PTY children. Sessions remain owned by this Activity; the
        // service only raises process importance. Started here while the app is in the foreground
        // (always an allowed start point), stopped in onDestroy.
        TerminalService.start(this)
    }

    /**
     * Drive the bottom padding smoothly with the IME animation and coalesce the terminal's PTY
     * resize to a single step at the end. DISPATCH_MODE_STOP keeps the per-frame inset out of
     * the normal apply-listener during the animation, so there is no resize/SIGWINCH storm and
     * no open/close flicker. The apply-listener (applyInsets) still owns the final resting state.
     */
    private fun syncImeAnimation() {
        ViewCompat.setWindowInsetsAnimationCallback(binding.root,
            object : WindowInsetsAnimationCompat.Callback(
                WindowInsetsAnimationCompat.Callback.DISPATCH_MODE_STOP
            ) {
                override fun onPrepare(animation: WindowInsetsAnimationCompat) {
                    binding.terminal.setResizeSuppressed(true)
                }
                override fun onProgress(
                    insets: WindowInsetsCompat,
                    running: MutableList<WindowInsetsAnimationCompat>
                ): WindowInsetsCompat {
                    val ime = insets.getInsets(WindowInsetsCompat.Type.ime())
                    val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
                    // Preserve the horizontal safe-area padding set by applyInsets; only the bottom
                    // tracks the IME during the animation.
                    binding.root.setPadding(
                        binding.root.paddingLeft, 0, binding.root.paddingRight,
                        if (ime.bottom > 0) ime.bottom else bars.bottom
                    )
                    return insets
                }
                override fun onEnd(animation: WindowInsetsAnimationCompat) {
                    binding.terminal.setResizeSuppressed(false)
                }
            })
    }

    /** Push the colour theme into a session. Reads the persisted xset store first, falling back to
     *  colors.xml (the store defaults are seeded to the same colors.xml values, so a fresh install is
     *  visually identical to before xset existed). */
    private fun configureSession(s: TerminalSession) {
        s.configure(
            xsetColor("theme.fg", R.color.term_fg),
            xsetColor("theme.bg", R.color.term_bg),
            xsetColor("cursor.color", R.color.term_accent),
            xsetStore.getInt("perf.scrollback", 5000)
        )
    }

    // ---------------- xset bridge ----------------

    private fun xsetColor(key: String, fallbackRes: Int): Int =
        xsetStore.getInt(key, ContextCompat.getColor(this, fallbackRes))

    private fun loadXsetPrefs(): Map<String, String> {
        val sp = getSharedPreferences("xset", MODE_PRIVATE)
        val m = HashMap<String, String>()
        for ((k, v) in sp.all) if (v is String) m[k] = v
        return m
    }

    private fun saveXsetPrefs(vals: Map<String, String>) {
        val e = getSharedPreferences("xset", MODE_PRIVATE).edit()
        e.clear(); for ((k, v) in vals) e.putString(k, v); e.apply()
    }

    /** Push the persisted theme (fg/bg/cursor/scrollback) to the active session + the view. */
    private fun pushTheme() {
        val fg = xsetColor("theme.fg", R.color.term_fg)
        val bg = xsetColor("theme.bg", R.color.term_bg)
        val cur = xsetColor("cursor.color", R.color.term_accent)
        val sb = xsetStore.getInt("perf.scrollback", 5000)
        binding.terminal.setDefaultBg(bg)
        binding.terminal.setCursorColor(cur)
        // Every workspace, not just the visible one: a session is configured at start, so the
        // others would keep the previous colours until they were closed.
        if (::workspaces.isInitialized) workspaces.forEachSession { it.configure(fg, bg, cur, sb) }
        // The engine re-themed the cells in place; tell the damage-driven loop to re-read them.
        // Without this the repaint above redraws the PREVIOUS snapshot and the change only became
        // visible once the shell next printed something.
        binding.terminal.refreshAfterConfigChange()
    }

    /** Apply persisted render knobs once the grid exists. Font SIZE is intentionally left to the
     *  existing zoom system (single source of truth) so the startup-zoom behaviour is preserved. */
    private fun applyXsetOnStart() {
        val s = xsetStore
        binding.terminal.setFontFamilyId(s.get("font.family"))
        binding.terminal.setLineSpacingPct(s.getInt("font.linespacing", 100))
        binding.terminal.setLetterSpacingMilli(s.getInt("font.letterspacing", 0))
        binding.terminal.setPaddingScalePct(s.getInt("layout.padding", 100))
        binding.terminal.setCursorStyle(CursorStyle.from(s.get("cursor.style")))
        binding.terminal.setCursorBlink(s.getBool("cursor.blink"))
        binding.terminal.setBoldBright(s.getBool("font.bold_bright"))
        pushTheme()
    }

    private fun makeXsetContext(): XsetContext = object : XsetContext {
        override val store: XsetStore get() = xsetStore
        override fun applyFontSizeDp(dp: Int) { binding.terminal.setFontSizeDp(dp) }
        override fun applyFontFamily(id: String) { binding.terminal.setFontFamilyId(id) }
        override fun applyLineSpacing(pct: Int) { binding.terminal.setLineSpacingPct(pct) }
        override fun applyLetterSpacing(milli: Int) { binding.terminal.setLetterSpacingMilli(milli) }
        override fun applyCursorStyle(style: CursorStyle) { binding.terminal.setCursorStyle(style) }
        override fun applyCursorBlink(on: Boolean) { binding.terminal.setCursorBlink(on) }
        override fun applyCursorColor(argb: Int) { binding.terminal.setCursorColor(argb); pushTheme() }
        override fun applyPaddingScale(pct: Int) { binding.terminal.setPaddingScalePct(pct) }
        override fun applyBoldBright(on: Boolean) { binding.terminal.setBoldBright(on) }
        override fun applyTheme(fg: Int, bg: Int, cursor: Int) { pushTheme() }
        override fun applyForeground(argb: Int) { pushTheme() }
        override fun applyBackground(argb: Int) { pushTheme() }
        override fun applyScrollback(lines: Int) { pushTheme() }
        override fun storageEnabled(): Boolean = xsetStore.getBool("storage.enabled")
        override fun storageStatus(): String = PermissionManager.storageStatusText(this@MainActivity)
        override fun enableStorage(): String {
            xsetStore.set("storage.enabled", "on")
            // live on the running shell, no session respawn (same PTY, same native handle). The storage
            // backing is bound unconditionally at spawn, so enabling only flips the ~/sdcard[/-1] symlinks
            // on. If permission is already held, applyStorageVisibility does that immediately; otherwise we
            // request it and apply on the result (onStoragePermissionResult). Never fabricates ~/sdcard.
            return if (PermissionManager.storageGranted(this@MainActivity)) {
                Bootstrap.applyStorageVisibility(this@MainActivity, true)
                "Storage enabled ✓. ~/sdcard mounted on this shell"
            } else {
                requestStorageAccess()   // returns straight back into xset; visibility applies on result
                "Requesting permission…"
            }
        }
        override fun disableStorage(): String {
            xsetStore.set("storage.enabled", "off")
            // OFF removes the ~/sdcard[/-1] symlinks from the running shell right now, a genuine
            // filesystem change on the same session, not a cosmetic flag or `ls` filter. The hidden
            // backing bind stays until the session ends (the backend's binds are fixed at spawn),
            // but it is no
            // longer reachable by the ~/sdcard names.
            Bootstrap.applyStorageVisibility(this@MainActivity, false)
            return "Storage disabled. ~/sdcard removed from this shell"
        }
        override fun appInfo(): List<Pair<String, String>> = deviceInfo()
        override fun guestBackend(): String {
            val rootfs = java.io.File(filesDir, "rootfs")
            val d = com.dracxterm.guest.GuestBackend.decision(this@MainActivity, rootfs)
                ?: return "not decided yet (no Linux rootfs, or first start pending)"
            return "${d.backend.name.lowercase()} — ${d.detail}"
        }
        override fun contactDeveloper(): String {
            // No mail client is a normal state on a device that runs a terminal, and
            // startActivity throws rather than returning false, so the address goes to the
            // clipboard instead. The user gets the address either way.
            val sent = runCatching {
                startActivity(Intent(Intent.ACTION_SENDTO).apply {
                    data = android.net.Uri.parse("mailto:$DEVELOPER_EMAIL")
                    putExtra(Intent.EXTRA_SUBJECT, "${getString(R.string.app_name)} feedback")
                    addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                })
            }.isSuccess
            return if (sent) {
                "Opening email to $DEVELOPER_EMAIL…"
            } else {
                runCatching {
                    (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
                        .setPrimaryClip(ClipData.newPlainText("dracXterm developer", DEVELOPER_EMAIL))
                }
                "No email app. Address copied: $DEVELOPER_EMAIL"
            }
        }
        override fun status(msg: String) { /* dashboard shows its own status line; no toast spam */ }
        override fun reapplyAll() { applyXsetOnStart(); binding.terminal.setFontSizeDp(xsetStore.getInt("font.size", binding.terminal.currentFontDp())) }
        override fun exportConfig(): String? = runCatching {
            val f = File(filesDir, "xset-config.json")
            f.writeText(xsetStore.toJson()); f.absolutePath
        }.getOrNull()
        override fun importConfig(): Boolean = runCatching {
            val f = File(filesDir, "xset-config.json")
            if (!f.exists()) false else xsetStore.fromJson(f.readText())
        }.getOrDefault(false)
        override fun requestClose() { binding.terminal.closeDashboard() }
    }

    /** Read-only, device-derived system information for the About screen. Every value comes from a
     *  cheap O(1) query (no shelling out, nothing that can stall the terminal) and NOTHING is
     *  hardcoded, the fields are read from Build/DisplayMetrics/system services at call time. */
    private fun deviceInfo(): List<Pair<String, String>> {
        val dm = resources.displayMetrics
        val gib = 1_073_741_824.0
        val verName = runCatching { packageManager.getPackageInfo(packageName, 0).versionName ?: "?" }.getOrDefault("?")
        val primaryAbi = runCatching { Build.SUPPORTED_ABIS.firstOrNull() ?: "?" }.getOrDefault("?")
        val abis = runCatching { Build.SUPPORTED_ABIS.joinToString(",") }.getOrDefault("?")
        val kernel = runCatching { System.getProperty("os.version") ?: "?" }.getOrDefault("?")
        val ram = runCatching {
            val mi = android.app.ActivityManager.MemoryInfo()
            (getSystemService(ACTIVITY_SERVICE) as android.app.ActivityManager).getMemoryInfo(mi)
            "%.1f GB".format(mi.totalMem / gib)
        }.getOrDefault("?")
        val storage = runCatching {
            val st = android.os.StatFs(filesDir.absolutePath)
            "%.1f / %.1f GB free".format(st.availableBytes / gib, st.totalBytes / gib)
        }.getOrDefault("?")
        val patch = runCatching { Build.VERSION.SECURITY_PATCH }.getOrDefault("?")
        return listOf(
            "Manufacturer" to Build.MANUFACTURER,
            "Model"        to Build.MODEL,
            "Android"      to "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})",
            "CPU ABI"      to primaryAbi,
            "Supported"    to abis,
            "Kernel"       to kernel,
            "Screen"       to "${dm.widthPixels}×${dm.heightPixels}",
            "Density"      to "${dm.densityDpi}dpi (${"%.1f".format(dm.density)}x)",
            "CPU cores"    to Runtime.getRuntime().availableProcessors().toString(),
            "Memory"       to ram,
            "Storage"      to storage,
            "Build ID"     to Build.ID,
            "Security"     to patch,
            "App version"  to verName,
            "Application"  to packageName,
        )
    }

    /** OSC 5391 payloads: "DRACX;open;<moduleId>", "DRACX;open;xset", "DRACX;close". */
    private fun handleAppControl(payload: String) {
        val p = payload.split(";")
        if (p.size < 2 || p[0] != "DRACX") return
        when (p[1]) {
            "open" -> {
                val mod = p.getOrNull(2)?.takeIf { it.isNotEmpty() && it != "xset" }
                runOnUiThread {
                    xsetStore.set("font.size", binding.terminal.currentFontDp().toString())  // sync zoom-owned value for display
                    binding.terminal.openDashboard(mod)
                }
            }
            "close" -> runOnUiThread { binding.terminal.closeDashboard() }
        }
    }

    /** Apply the active/inactive visual state to an existing workspace chip (no view creation). */
    private fun styleWorkspaceChip(chip: TextView, active: Boolean) {
        chip.setBackgroundResource(
            if (active) R.drawable.session_chip_active else R.drawable.session_chip)
        chip.setTextColor(ContextCompat.getColor(this,
            if (active) R.color.term_fg else R.color.key_fg))
        chip.alpha = if (active) 1f else 0.7f
    }

    /** Update the workspace tab row: one chip per workspace (active highlighted) + a "+" chip while
     *  under the WorkspaceManager.MAX limit.
     *
     *  Incremental by design: a plain workspace SWITCH does not change the row's structure (same
     *  number of chips, same "+" presence), so we only restyle the active state in place, we do not
     *  removeAllViews()+re-inflate. An unconditional rebuild forces a measure/layout/draw pass on
     *  the tab row on every switch, adding avoidable churn to the transition. A full rebuild happens
     *  only when the structure actually changes (add/remove/first build). */
    private fun renderWorkspaceBar() {
        val tabs = binding.sessionTabs
        val count = workspaces.count
        val canAdd = workspaces.canAdd()

        // Fast path: structure unchanged -> restyle active state only.
        if (count == renderedWorkspaceCount && canAdd == renderedCanAdd &&
            tabs.childCount == count + (if (canAdd) 1 else 0)) {
            for (i in 0 until count) {
                (tabs.getChildAt(i) as? TextView)?.let { styleWorkspaceChip(it, workspaces.isActive(i)) }
            }
            return
        }

        // Structure changed: full rebuild.
        tabs.removeAllViews()
        val d = resources.displayMetrics.density
        fun dp(v: Int) = (v * d).toInt()
        fun params(endMargin: Int) = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { marginEnd = dp(endMargin) }

        for (i in 0 until count) {
            val chip = TextView(this).apply {
                text = getString(R.string.workspace_chip, i + 1)
                setPadding(dp(12), dp(5), dp(12), dp(5))
                textSize = 13f
                typeface = Typeface.MONOSPACE
                setOnClickListener {
                    workspaces.switchTo(i); binding.terminal.requestFocusAndKeyboard()
                }
            }
            styleWorkspaceChip(chip, workspaces.isActive(i))
            tabs.addView(chip, params(6))
        }
        if (canAdd) {
            val add = TextView(this).apply {
                text = getString(R.string.workspace_add)
                setBackgroundResource(R.drawable.session_chip)
                setPadding(dp(14), dp(5), dp(14), dp(5))
                textSize = 15f
                setTextColor(ContextCompat.getColor(this@MainActivity, R.color.term_accent))
                setOnClickListener {
                    workspaces.addWorkspace(); binding.terminal.requestFocusAndKeyboard()
                }
            }
            tabs.addView(add, params(0))
        }
        renderedWorkspaceCount = count
        renderedCanAdd = canAdd
    }

    private fun showFindDialog() {
        val input = EditText(this).apply { hint = getString(R.string.find_hint) }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle(R.string.find_title)
            .setView(input)
            .setPositiveButton(R.string.find_next) { _, _ ->
                val q = input.text.toString()
                if (q.isNotEmpty() && !binding.terminal.find(q)) {
                    Toast.makeText(this, R.string.find_none, Toast.LENGTH_SHORT).show()
                }
                binding.terminal.requestFocus()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun copyToClipboard(text: String) {
        if (text.isEmpty()) return
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("dracXterm", text))
        Toast.makeText(this, R.string.copied, Toast.LENGTH_SHORT).show()
    }

    private fun pasteFromClipboard() {
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val clip = cm.primaryClip ?: return
        if (clip.itemCount == 0) return
        val text = clip.getItemAt(0).coerceToText(this)?.toString() ?: return
        if (text.isNotEmpty()) binding.terminal.paste(text)
    }

    /** The storage-permission flow for this Android version: the system "all files access"
     *  screen from API 30, the legacy READ/WRITE prompt below it. A refusal, or a device with
     *  no such screen, leaves the terminal running with app-private storage. */
    private fun requestStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val intent = PermissionManager.allFilesAccessIntent(this)
                ?: Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
            runCatching { requestAllFilesAccess.launch(intent) }.onFailure {
                Toast.makeText(this, "Grant: Settings ▸ Apps ▸ ${getString(R.string.app_name)} ▸ All files access",
                    Toast.LENGTH_LONG).show()
            }
        } else {
            val perms = PermissionManager.legacyStoragePermissions()
            if (perms.isNotEmpty()) runCatching { requestStoragePerms.launch(perms) }
        }
    }

    /** Result of a storage-permission request (either flow). The user is still inside xset, so we apply
     *  visibility live on the active shell, no respawn, no session swap (same PTY, same native handle).
     *  The storage backing is bound unconditionally at spawn, so granting now only needs the ~/sdcard
     *  symlink flipped on; applyStorageVisibility does that and returns true. We then repaint the
     *  dashboard so its Storage row reflects the new state. If permission was denied, nothing changes and
     *  the terminal keeps working exactly as before. */
    private fun onStoragePermissionResult() {
        val ok = PermissionManager.storageGranted(this)
        // enableStorage() persists the opt-in BEFORE the OS is asked, because the answer arrives
        // here asynchronously. If the answer is no, that optimistic flag has to be taken back: the
        // xset row reads it directly, so leaving it on made the row report "ON" with nothing
        // mounted, and it stayed wrong across restarts.
        if (!ok && xsetStore.getBool("storage.enabled")) xsetStore.set("storage.enabled", "off")
        val enabled = xsetStore.getBool("storage.enabled")
        // The dashboard wrote its status line when the key was pressed, from the optimistic value.
        // Replace it with the answer that actually came back, or it contradicts the row above it.
        xset.setStatus(
            if (ok && enabled) "Storage Access: ON" else "Storage Access: OFF (permission not granted)"
        )
        binding.terminal.invalidate()   // repaint xset so the Storage row shows the updated status live
        // Apply visibility live on the active shell, no respawn.
        val applied = if (ok && enabled) Bootstrap.applyStorageVisibility(this, true) else true
        Toast.makeText(
            this,
            when {
                ok && enabled && applied -> "Storage granted ✓. ~/sdcard mounted on this shell"
                ok && enabled            -> "Storage granted. Re-open xset ▸ Storage if ~/sdcard is empty"
                ok                       -> "Storage access granted"
                else                     -> "Storage access not granted. The terminal still works"
            },
            Toast.LENGTH_LONG
        ).show()
    }

    /** Push the status bar out of the top bar and let the extra-keys row ride above the IME. */
    private fun applyInsets() {
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cut = insets.getInsets(WindowInsetsCompat.Type.displayCutout())
            val ime = insets.getInsets(WindowInsetsCompat.Type.ime())
            // Status bar goes into the top bar's own top padding (keeps its background edge-to-edge).
            binding.topBar.setPadding(
                binding.topBar.paddingLeft, bars.top,
                binding.topBar.paddingRight, binding.topBar.paddingBottom
            )
            // Horizontal safe-area (notch/cutout in landscape) + bottom = IME when up, else nav bar.
            // This guarantees the whole workspace, terminal AND the extra-keys toolbar, stays inside
            // the safe area and is never clipped by the system bars.
            val left = maxOf(bars.left, cut.left)
            val right = maxOf(bars.right, cut.right)
            val bottom = if (ime.bottom > 0) ime.bottom else bars.bottom
            binding.root.setPadding(left, 0, right, bottom)
            updateKeyboardIcon(ime.bottom > 0)
            insets
        }
        ViewCompat.requestApplyInsets(binding.root)
    }

    /** Reflect keyboard state on the toggle button: a keyboard glyph when the IME is up (tap to
     *  hide), an up-chevron over a keyboard bar when it is down (tap to show). */
    private fun updateKeyboardIcon(imeVisible: Boolean) {
        binding.btnKeyboard.setImageResource(
            if (imeVisible) R.drawable.ic_keyboard else R.drawable.ic_keyboard_show)
    }

    /**
     * Raise the keyboard when the app opens.
     *
     * Once per launch, not once per resume: coming back from another app restores whatever
     * IME state was left behind, and forcing the keyboard up there would fight the user
     * who had just put it away. Backgrounding and returning is not "opening the app".
     *
     * Deferred to the first window focus rather than done in onCreate. A show request
     * made before the window has focus is dropped by the system, so asking earlier would
     * work on a fast device and silently do nothing on a slow one.
     *
     * Uses the same WindowInsetsController path as [toggleKeyboard] instead of
     * InputMethodManager.showSoftInput: SHOW_IMPLICIT is ignored under the API 33+ IME
     * restrictions, and going through the controller also means the toolbar icon updates
     * from the inset listener exactly as it does for a manual toggle.
     */
    private var keyboardRaisedOnLaunch = false

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus || keyboardRaisedOnLaunch) return
        keyboardRaisedOnLaunch = true
        binding.terminal.requestFocus()
        WindowInsetsControllerCompat(window, binding.terminal)
            .show(WindowInsetsCompat.Type.ime())
    }

    private fun toggleKeyboard() {
        val controller = WindowInsetsControllerCompat(window, binding.terminal)
        val visible = ViewCompat.getRootWindowInsets(binding.root)
            ?.isVisible(WindowInsetsCompat.Type.ime()) ?: false
        binding.terminal.requestFocus()
        if (visible) controller.hide(WindowInsetsCompat.Type.ime())
        else controller.show(WindowInsetsCompat.Type.ime())
    }

    override fun onDestroy() {
        TerminalService.stop(this)
        workspaces.closeAll()
        super.onDestroy()
    }
}
