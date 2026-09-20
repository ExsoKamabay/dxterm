package com.dracxterm.rootfs

import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.TextUtils
import android.text.format.Formatter
import android.util.Log
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.os.LocaleListCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.dracxterm.App
import com.dracxterm.LocaleSupport
import com.dracxterm.MainActivity
import com.dracxterm.R
import com.dracxterm.databinding.ActivityProvisioningBinding
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * Launcher activity. Runs the RootFS provisioning pipeline (first run only) behind a
 * splash/progress screen, then hands off to the terminal. Blocks the back gesture and
 * user interaction while provisioning is in flight.
 *
 * Storage/media permission is deliberately not requested here. Per product decision the app must
 * never surface a permission dialog at startup, storage access is opt-in and only requested when
 * the user chooses Settings ▸ Storage Access ▸ Grant (handled in MainActivity/xset).
 *
 * The Linux image is handled the same way. Nothing is fetched over the network unless the
 * user presses the download button on the consent panel; declining is a first-class
 * outcome that leads to a working BusyBox terminal, not to an error state.
 */
class ProvisioningActivity : AppCompatActivity() {

    private lateinit var binding: ActivityProvisioningBinding
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var busy = true
    private val cancelRequested = AtomicBoolean(false)

    /**
     * What the screen is doing, and the only thing that decides what is on it.
     *
     * Visibility, labels, enabled state and what a press means all read this. They used to be
     * set independently at each call site, which is how the transfer button ended up carrying
     * two unrelated jobs -- Cancel/Resume and the BusyBox fallback -- with whichever listener
     * was installed last deciding what a press did, regardless of what the label said.
     */
    private enum class Phase { BOOTING, CONSENT, DOWNLOADING, PAUSING, PAUSED, INSTALLING, ERROR }

    private var phase = Phase.BOOTING

    /** Set only by [showError]; the fallback is offered for download failures, not for a
     *  half-written sandbox. */
    private var errorAllowsBusybox = false

    /**
     * Rejects a second press that lands within [CLICK_GAP_MS] of an accepted one.
     *
     * Without it a fast double tap on Resume ran two different actions: the first started the
     * transfer and flipped the button to Cancel, the second hit Cancel and stopped it again, so
     * the download appeared not to start at all.
     */
    private var lastClickAt = 0L

    private fun claimClick(): Boolean {
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - lastClickAt < CLICK_GAP_MS) return false
        lastClickAt = now
        return true
    }

    /** The image a paused download belongs to, so Resume continues the same one. */
    private var pausedEntry: RootfsCatalog.Entry? = null

    /** Guards against a second download thread: the button is disabled while one runs, but a
     *  fast double tap, a resume racing a finishing pause, or a rotation can still get through. */
    private val downloadRunning = AtomicBoolean(false)

    private val state by lazy { ProvisioningState(this) }
    private val downloader by lazy { RootfsDownloader(this) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(ShellLocator.TAG, "[BOOT] ProvisioningActivity started")
        // Retarget this activity's resources at the saved language before anything is inflated.
        // attachBaseContext alone is not enough: AppCompatActivity re-applies its own
        // configuration on top of the base it is handed, which put the locale back to the
        // device's and left a restarted app showing the language the user had switched away
        // from while the toggle correctly reported the other one.
        LocaleSupport.applyInPlace(this)
        binding = ActivityProvisioningBinding.inflate(layoutInflater)
        setContentView(binding.root)
        hideAllControls()

        // Prevent leaving mid-provision. While the consent panel is up the user is not
        // blocked: back means "not now", which is the same as continuing with BusyBox.
        // The on-screen button and the system gesture do the same thing, decided in one place,
        // so the two can never disagree about what "back" means on this screen.
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = onBackAction()
        })
        binding.back.setOnClickListener { if (claimClick()) onBackAction() }

        // One listener for the transfer button, for its whole life. What a press means is read
        // from the phase at the moment of the press, so the button can never run an action that
        // belongs to a state it is no longer in.
        binding.cancel.setOnClickListener {
            if (!claimClick()) return@setOnClickListener
            when (phase) {
                // Ask the worker to stop at its next chunk boundary; it closes the stream and
                // keeps the .part file. PAUSING is what the user sees until it actually stops.
                Phase.DOWNLOADING -> {
                    cancelRequested.set(true)
                    // Flag first, then interrupt: the worker must already see "cancelled" when the
                    // read it is blocked in throws, so the throw is read as the user's cancel and
                    // not as a transfer failure.
                    downloader.abort()
                    // Say what is happening. The worker stops at its next checkpoint, and
                    // connecting can take a moment, so a silently greyed button would read as
                    // the press having done nothing.
                    binding.status.text = getString(R.string.consent_pausing)
                    setPhase(Phase.PAUSING)
                }
                Phase.PAUSED -> pausedEntry?.let { startDownload(it) }
                Phase.ERROR -> if (errorAllowsBusybox) declineAndContinue(persist = false)
                else -> Unit
            }
        }
        binding.retry.setOnClickListener { if (claimClick()) startProvisioning() }

        // The Indonesian default is applied in App.onCreate, before any activity exists.
        // Setting it here recreated this activity in the middle of its own provisioning
        // pass; see App for what that broke.
        renderLangToggle()
        binding.langToggle.setOnClickListener { toggleLanguage() }
        applyInsets()
        sizePreview()
        startProvisioning()
    }

    // ------------------------------------------------------------------ pipeline

    /** The button offers the language you are not in, so its label is the destination. */
    private fun renderLangToggle() {
        binding.langToggle.text =
            if (LocaleSupport.tag(this).startsWith("en")) getString(R.string.lang_switch_to)
            else getString(R.string.lang_toggle)
    }

    /**
     * Switch language without recreating anything.
     *
     * The old implementation called AppCompatDelegate.setApplicationLocales, which is defined to
     * recreate every started activity: that is the flash, and it also threw away whatever this
     * screen was in the middle of. Now the choice is saved, this activity's resources are
     * retargeted, and the strings currently on screen are re-read in place. Nothing is torn
     * down, so the provisioning thread, the download, the picker selection and the scroll
     * position all survive the switch.
     */
    private fun toggleLanguage() {
        LocaleSupport.save(this, LocaleSupport.other(LocaleSupport.tag(this)))
        LocaleSupport.applyInPlace(this)
        // The catalogue keeps the note in the language it was parsed in, and Locale.getDefault
        // is what it reads, so it has to be dropped after the locale moves, not before.
        RootfsCatalog.invalidate()
        renderAllText()
    }

    /**
     * Re-read every string this screen is currently showing.
     *
     * Anything set once from the layout would otherwise keep the old language, because the
     * views are not recreated. Written to be safe to call at any point: each part only
     * rewrites what it owns, and the consent panel is redrawn only while it is the thing on
     * screen, so a switch during a download does not resurrect it.
     */
    private fun renderAllText() {
        renderLangToggle()
        // Content descriptions are text too. They come from the layout at inflation time, so
        // without this the screen reader kept announcing the back button in the language the
        // user had just switched away from.
        binding.back.contentDescription = getString(R.string.back)
        binding.provisionSubtitle.setText(R.string.provision_subtitle)
        binding.consentTitle.setText(R.string.consent_title)
        binding.consentPickerLabel.setText(R.string.consent_pick_distro)
        // Through renderPhase, so the label always matches what a press will actually do.
        renderPhase()
        binding.retry.setText(R.string.retry)
        if (binding.consent.visibility == View.VISIBLE) {
            val entries = RootfsCatalog.entriesFor(this)
            catalogEntries = entries
            binding.consentArch.text = resources.getQuantityString(
                R.plurals.consent_arch,
                entries.size,
                Build.SUPPORTED_ABIS.firstOrNull() ?: "unknown",
                entries.size
            )
            // The picker labels come from the catalogue, which was just invalidated, so the
            // adapter is rebuilt rather than left holding the previous language's strings.
            val keep = binding.consentPicker.selectedItemPosition
            binding.consentPicker.adapter = ArrayAdapter(
                this, R.layout.spinner_item, entries.map { it.label }
            ).also { it.setDropDownViewResource(R.layout.spinner_dropdown_item) }
            if (keep in entries.indices) binding.consentPicker.setSelection(keep)
            entries.getOrNull(keep)?.let { renderConsentFor(it) }
        }
    }

    /**
     * Keep the content clear of the status and navigation bars.
     *
     * targetSdk 36 means the window is edge-to-edge whether or not this activity asks
     * for it, so without this the download button sits underneath the navigation bar --
     * visible, but only partly tappable. MainActivity already does this; this screen was
     * simply missing it.
     *
     * Padding goes on the scrolling content rather than the ScrollView so the background
     * still reaches the screen edges while nothing scrolls under the bars for good.
     */
    /**
     * What "back" does, for both the button and the system gesture.
     *
     * The ordering is what keeps a transfer safe. A running download is paused rather than
     * abandoned, so the bytes on disk survive and the user stays on the screen instead of being
     * dropped into a shell -- which is what previously read as the app closing itself. Only once
     * nothing is in flight does back mean leaving.
     */
    private fun onBackAction() {
        when {
            // Nothing to go back to while a transfer or an install is in flight, and the button
            // is not on screen in those phases either. Swallowed so the gesture cannot do what
            // the button deliberately does not offer; Cancel is the way out of a download.
            phase == Phase.DOWNLOADING || phase == Phase.PAUSING || phase == Phase.INSTALLING ->
                Unit

            // Paused: the partial file stays; step back to the picker, which will offer to
            // resume rather than to start over.
            phase == Phase.PAUSED -> showConsent()
            // A failure is not a dead end. Back returns to the picker so another image can be
            // chosen or the same one retried; it used to fall through to finish() and close the
            // app, which is the opposite of what a back button on an error screen should do.
            phase == Phase.ERROR -> showConsent()
            binding.consent.visibility == View.VISIBLE -> declineAndContinue(persist = false)
            // Extraction is not interruptible without leaving a half-written sandbox, so back
            // is inert here; the button is disabled too, so this is only reachable by gesture.
            busy -> Unit
            else -> finish()
        }
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    private fun applyInsets() {
        // The scrolling column by id, not by position: the root is a FrameLayout now (it also
        // holds the back button), so getChildAt(0) would pad the ScrollView instead.
        val content = binding.content
        val (l, t, r, b) = listOf(
            content.paddingLeft, content.paddingTop, content.paddingRight, content.paddingBottom
        )
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cut = insets.getInsets(WindowInsetsCompat.Type.displayCutout())
            content.setPadding(
                l + maxOf(bars.left, cut.left),
                t + bars.top,
                r + maxOf(bars.right, cut.right),
                b + bars.bottom
            )
            // The back button is chrome pinned to the frame, so it is outside the padded
            // column and has to clear the status bar and any cutout on its own.
            (binding.back.layoutParams as? android.widget.FrameLayout.LayoutParams)?.let { lp ->
                lp.topMargin = bars.top + dp(12)
                lp.marginStart = maxOf(bars.left, cut.left) + dp(12)
                binding.back.layoutParams = lp
            }
            insets
        }
    }

    /**
     * Run the provisioning pipeline, or attach to the one already running.
     *
     * One pass per process, enforced with [provisioningRunning]. Extraction stages
     * through `rootfs.tmp`, and a second pass starting while the first is mid-flight
     * fails on `mkdir`, which is exactly what a recreated activity used to cause.
     * App.onCreate removed the recreation this app was inflicting on itself, but a
     * recreate can still come from outside (a theme change, a locale change from the
     * system), and starting a second extraction is never the right answer to one.
     *
     * The screen is therefore a separate thing from the work. [liveScreen] is whichever
     * instance is currently on top; the worker reports through it, so a recreated
     * activity picks up the running pass instead of restarting it.
     */
    private fun startProvisioning() {
        busy = true
        // Extraction owns the staging tree until it finishes; there is no way back from here.
        setPhase(Phase.INSTALLING)
        hideAllControls()
        binding.status.visibility = View.VISIBLE
        binding.status.text = getString(R.string.provision_starting)
        binding.progress.visibility = View.VISIBLE
        liveScreen = this

        // A pass that completed while no screen was attached, e.g. it finished during a
        // recreate. Consumed rather than left behind: keeping it would make the retry
        // button on an error screen replay the same error instead of trying again.
        finishedOutcome?.let { outcome ->
            finishedOutcome = null
            finishBoot(outcome)
            return
        }
        if (!provisioningRunning.compareAndSet(false, true)) return

        // applicationContext, not the activity: this thread outlives any single instance.
        val ctx = applicationContext
        thread(name = "provisioning") {
            // Same boundary as the download thread: a throw here would kill the process and
            // latch provisioningRunning, after which no later attempt could ever start.
            val outcome = try {
                BootManager(ctx).boot(relay)
            } catch (t: Throwable) {
                Log.e(ShellLocator.TAG, "[BOOT] unexpected failure", t)
                BootManager.Outcome(
                    BootManager.BootMode.ERROR,
                    "${t.javaClass.simpleName}: ${t.message ?: "unexpected failure"}"
                )
            } finally {
                provisioningRunning.set(false)
            }
            main.post {
                val screen = liveScreen
                if (screen != null) screen.finishBoot(outcome) else finishedOutcome = outcome
            }
        }
    }

    /** Last few lines shown by the live preview. Bounded so it cannot grow unbounded
     *  over a long extraction; only the tail is ever on screen anyway. */
    private val previewLines = ArrayDeque<CharSequence>()

    /** Pixels a preview line may occupy, kept current by [sizePreview]. Zero until the
     *  first layout, in which case a line is shown whole: there is no measurement to
     *  shorten it against yet, and at that point nothing has been drawn either. */
    private var previewWidthPx = 0

    /**
     * Show what provisioning is doing, in place of the static subtitle.
     *
     * Throttled to ~8 updates a second. The extractor reports every 250 entries, which on
     * a fast device is far quicker than anyone can read and fast enough to make the text
     * a blur; the work is what should be visible, not the frame rate.
     */
    private var lastPreviewNanos = 0L
    private fun showDetail(line: String) {
        // Throttle the path stream, never the step messages.
        //
        // The 125ms limit exists because the extractor reports every 250 entries, which
        // is faster than anyone can read. But "Configuring environment…" fires once and
        // matters, and the first version dropped exactly those: they arrived inside a
        // window opened by the flood of paths just before them, so the preview kept
        // showing stale filenames while the step below had already moved on. Paths
        // contain a separator; the step messages do not.
        val isPath = line.contains('/')
        val now = System.nanoTime()
        if (isPath) {
            if (now - lastPreviewNanos < 125_000_000L) return
            lastPreviewNanos = now
        }
        runOnUiThread {
            // Measured on the UI thread: shortening asks the view's own TextPaint how
            // wide the text is, and a TextPaint is not safe to use from the extractor's
            // thread while the view is being drawn from this one.
            val shown = ellipsiseStart(line)
            previewLines.addLast(shown)
            while (previewLines.size > 3) previewLines.removeFirst()
            binding.provisionSubtitle.visibility = View.GONE
            binding.livePreview.visibility = View.VISIBLE
            binding.livePreview.text = previewLines.joinToString("\n")
        }
    }

    /**
     * Keep the tail of a long line and mark the cut, in code rather than by layout.
     *
     * android:ellipsize does nothing on a multi-line TextView. It wraps instead, and a
     * single long path then ate two of the three slots and pushed an older line out. The
     * end of a path is the part that identifies it, so the front is what goes.
     *
     * Measured, not counted. Counting characters only works in a monospace font, and the
     * font here is not one: `android:fontFamily="monospace"` is in the layout, but this
     * device renders the preview proportionally anyway, so 48 characters is a different
     * width for every line. Asking the view's own paint how much fits is right whatever
     * font it ends up drawing with, and it is what guarantees no line wraps.
     */
    private fun ellipsiseStart(line: String): CharSequence {
        val width = previewWidthPx
        if (width <= 0) return line
        return TextUtils.ellipsize(
            line, binding.livePreview.paint, width.toFloat(), TextUtils.TruncateAt.START
        )
    }

    /**
     * Track the width a preview line may occupy.
     *
     * `status` is match_parent in the same LinearLayout, so its width is exactly the
     * space available, after the insets padding, which is why this is a layout listener
     * and not a single read at startup. It also follows a rotation or a split-screen
     * resize without anything else having to notice.
     */
    private fun sizePreview() {
        binding.status.addOnLayoutChangeListener { _, l, _, r, _, _, _, _, _ ->
            previewWidthPx = r - l
        }
    }

    /** Put the subtitle back and drop the preview. Used when returning to the consent
     *  screen, where there is no activity to preview. */
    private fun hideDetail() {
        previewLines.clear()
        binding.livePreview.visibility = View.GONE
        binding.livePreview.text = ""
        binding.provisionSubtitle.visibility = View.VISIBLE
    }

    private fun onStageUpdate(message: String, percent: Int) {
        main.post {
            binding.status.text = message
            if (percent < 0) {
                binding.progress.isIndeterminate = true
            } else {
                binding.progress.isIndeterminate = false
                binding.progress.progress = percent.coerceIn(0, 100)
            }
        }
    }

    /**
     * The manifest keeps this activity alive across rotation, resize and density changes, and
     * the framework rebuilds its resources from the system configuration each time, locale
     * included. Without re-applying the saved language here, every string read after a
     * rotation came out in the device language: an Indonesian consent screen turned into
     * "Downloading ... Cancel" halfway through a download.
     */
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        LocaleSupport.applyInPlace(this)
    }

    override fun onDestroy() {
        // Only if it is still us: on a recreate the replacement may already have claimed
        // the slot, and clearing it then would drop the progress on the floor.
        if (liveScreen === this) liveScreen = null
        super.onDestroy()
    }

    private fun finishBoot(outcome: BootManager.Outcome) {
        busy = false
        when (outcome.mode) {
            BootManager.BootMode.LINUX, BootManager.BootMode.BUSYBOX -> launchTerminal()
            BootManager.BootMode.NEEDS_IMAGE -> showConsent()
            BootManager.BootMode.ERROR -> showError(getString(R.string.provision_failed, outcome.message))
        }
    }

    private fun launchTerminal() {
        startActivity(Intent(this, MainActivity::class.java))
        finish()
    }

    // ------------------------------------------------------------------ consent

    /** The image the user has picked. Set by [showConsent]; null when the device has
     *  no runnable image at all, in which case the consent panel is never shown. */
    private var selected: RootfsCatalog.Entry? = null

    /**
     * The catalogue the picker is currently showing.
     *
     * A field rather than a local captured by the selection listener. The listener outlives the
     * call that installed it, and Spinner posts its selection callback to a later layout pass,
     * so a captured list meant that after a language switch the callback fired with the entries
     * parsed in the previous language and wrote that language's note back over the new one.
     */
    private var catalogEntries: List<RootfsCatalog.Entry> = emptyList()

    private fun showConsent() {
        val entries = RootfsCatalog.entriesFor(this)
        catalogEntries = entries

        // Nothing this device can run. Not an error: BusyBox is a working terminal, and
        // offering a download that PRoot could never execute would waste hundreds of
        // megabytes to end in a failure the user cannot act on.
        if (entries.isEmpty()) {
            binding.status.text = getString(
                R.string.consent_no_images,
                Build.SUPPORTED_ABIS.firstOrNull() ?: "unknown"
            )
            declineAndContinue(persist = true)
            return
        }

        hideAllControls()
        hideDetail()
        setPhase(Phase.CONSENT)
        // The status line carries nothing on this screen, and it is minLines=2 so the shell
        // of it still reserved two blank rows between the language button and the copy
        // below. Hidden here and brought back by whoever next has something to say.
        binding.status.text = ""
        binding.status.visibility = View.GONE
        binding.langToggle.visibility = View.VISIBLE
        binding.consent.visibility = View.VISIBLE

        // Name the architecture before the list rather than leaving it implicit. The
        // list IS the answer to "what can this device run", so saying which device was
        // detected makes the filtering visible instead of mysterious.
        binding.consentArch.text = resources.getQuantityString(
            R.plurals.consent_arch,
            entries.size,
            Build.SUPPORTED_ABIS.firstOrNull() ?: "unknown",
            entries.size
        )

        // A one-item picker is noise; hide it and let the body text say what is coming.
        val single = entries.size == 1
        binding.consentPickerLabel.visibility = if (single) View.GONE else View.VISIBLE
        binding.consentPicker.visibility = if (single) View.GONE else View.VISIBLE

        // Own item layouts rather than the platform's: simple_spinner_item is left-aligned and
        // uses the system font, so the picker was the one element on a centred, serif screen
        // that looked borrowed from somewhere else.
        binding.consentPicker.adapter = ArrayAdapter(
            this,
            R.layout.spinner_item,
            entries.map { it.label }
        ).also { it.setDropDownViewResource(R.layout.spinner_dropdown_item) }

        binding.consentPicker.onItemSelectedListener =
            object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) {
                    catalogEntries.getOrNull(pos)?.let { renderConsentFor(it) }
                }
                override fun onNothingSelected(p: AdapterView<*>?) = Unit
            }

        // Start on the catalogue's stated default, not on whichever entry happens to
        // be first, and move the picker to match so the two never disagree.
        val initial = RootfsCatalog.default(this) ?: entries.first()
        binding.consentPicker.setSelection(entries.indexOf(initial).coerceAtLeast(0))
        renderConsentFor(initial)

        binding.consentDownload.setOnClickListener {
            val entry = selected ?: return@setOnClickListener
            if (!hasNetwork()) {
                binding.status.text = getString(R.string.consent_no_network)
                binding.status.visibility = View.VISIBLE
                return@setOnClickListener
            }
            startDownload(entry)
        }
    }

    /**
     * The user chose not to fetch an image. [persist] records the choice so the offer is
     * not repeated on every launch; a cancelled download passes false, because cancelling
     * a transfer is not the same as saying no.
     */
    /** Redraws the consent copy for one image. Called on first show and on every
     *  selection change, so the size and host always describe what is about to be
     *  downloaded rather than the default. */
    private fun renderConsentFor(entry: RootfsCatalog.Entry) {
        selected = entry
        val partial = downloader.partialBytes(entry)

        binding.consentPickerNote.text = if (entry.note.isBlank()) {
            getString(
                R.string.consent_size_line,
                Formatter.formatShortFileSize(this, entry.approxBytes),
                Formatter.formatShortFileSize(this, entry.approxBytes + entry.approxExtractedBytes)
            )
        } else {
            entry.note + "\n" + getString(
                R.string.consent_size_line,
                Formatter.formatShortFileSize(this, entry.approxBytes),
                Formatter.formatShortFileSize(this, entry.approxBytes + entry.approxExtractedBytes)
            )
        }

        if (partial > 0L) {
            binding.consentBody.text = getString(
                R.string.consent_body_resume,
                entry.label,
                Formatter.formatShortFileSize(this, partial)
            )
            binding.consentDownload.setText(R.string.consent_download_resume)
        } else {
            // The copy no longer names the mirror. It is one more proper noun on a screen the
            // user is reading to make one decision, and the host is not the decision.
            binding.consentBody.text =
                getString(R.string.consent_body)
            binding.consentDownload.setText(R.string.consent_download)
        }
    }

    private fun declineAndContinue(persist: Boolean) {
        if (persist) state.imageOfferDeclined = true
        hideAllControls()
        binding.status.visibility = View.VISIBLE
        binding.status.text = getString(R.string.consent_starting_busybox)
        binding.progress.visibility = View.VISIBLE
        binding.progress.isIndeterminate = true
        launchTerminal()
    }

    // ------------------------------------------------------------------ download

    private fun startDownload(entry: RootfsCatalog.Entry) {
        // One transfer at a time. Two threads writing the same .part file would interleave
        // their Range writes and produce a file that fails its checksum for no visible reason.
        if (!downloadRunning.compareAndSet(false, true)) return

        busy = true
        pausedEntry = entry
        cancelRequested.set(false)
        hideAllControls()
        binding.status.visibility = View.VISIBLE
        binding.progress.visibility = View.VISIBLE
        binding.progress.isIndeterminate = true
        binding.status.text = getString(R.string.consent_downloading_unknown, entry.label, "0 B")
        setPhase(Phase.DOWNLOADING)

        thread(name = "rootfs-download") {
            // A failure boundary, not decoration. Anything escaping here would be an uncaught
            // exception on a background thread, which on Android kills the process -- and on the
            // way out it would leave downloadRunning latched true, so the button would never
            // start another transfer even if the process survived. Every exit path releases the
            // flag and delivers a Result the screen already knows how to show.
            val result = try {
                downloader.download(
                    entry,
                    { downloaded, total -> main.post { renderDownloadProgress(entry, downloaded, total) } },
                    { cancelRequested.get() },
                    // Reported at the moment the downloader decides, which is before the restarted
                    // file is written, so the explanation arrives before the work is redone.
                    { notice -> main.post { binding.status.text = notice } }
                )
            } catch (t: Throwable) {
                Log.e(ShellLocator.TAG, "[DOWNLOAD] unexpected failure", t)
                RootfsDownloader.Result.Failed(
                    "${t.javaClass.simpleName}: ${t.message ?: "unexpected failure"}"
                )
            } finally {
                downloadRunning.set(false)
            }
            main.post { onDownloadFinished(result) }
        }
    }

    private fun setPhase(p: Phase) {
        phase = p
        renderPhase()
    }

    /**
     * Put the controls in the shape the current [phase] calls for.
     *
     * Back is not merely disabled while work is in flight, it is absent: there is no safe
     * destination during a transfer or an install, and a visibly dead control invites tapping.
     * It lives in the frame overlay rather than the scrolling column, so showing and hiding it
     * moves nothing else on the screen.
     *
     * The transfer button's label is read from resources here, so a language switch relabels it
     * wherever it happens to be, and its meaning always matches the label because both come
     * from the same phase.
     */
    private fun renderPhase() {
        // Not on the picker: that screen is the app's main view and has nothing above it to go
        // back to. Back stays where it has a destination -- a paused transfer and an error both
        // step back to the picker.
        val canGoBack = phase == Phase.PAUSED || phase == Phase.ERROR
        binding.back.visibility = if (canGoBack) View.VISIBLE else View.GONE

        when (phase) {
            Phase.DOWNLOADING -> {
                binding.cancel.visibility = View.VISIBLE
                binding.cancel.isEnabled = true
                binding.cancel.setText(R.string.consent_cancel)
            }
            // Between the press and the worker noticing it. The button stays visible and keeps
            // its label so nothing jumps, but takes no further press: the stop is already
            // under way and a second one has nothing to do.
            Phase.PAUSING -> {
                binding.cancel.visibility = View.VISIBLE
                binding.cancel.isEnabled = false
                binding.cancel.setText(R.string.consent_cancel)
            }
            Phase.PAUSED -> {
                binding.cancel.visibility = View.VISIBLE
                binding.cancel.isEnabled = true
                binding.cancel.setText(R.string.consent_resume)
            }
            Phase.ERROR -> {
                binding.cancel.visibility = if (errorAllowsBusybox) View.VISIBLE else View.GONE
                binding.cancel.isEnabled = true
                if (errorAllowsBusybox) binding.cancel.setText(R.string.busybox_fallback)
            }
            else -> binding.cancel.visibility = View.GONE
        }
    }

    /** Paused: progress frozen where it stopped, and the button now offers to continue. */
    private fun showPaused() {
        val kept = pausedEntry?.let { downloader.partialBytes(it) } ?: 0L
        binding.status.visibility = View.VISIBLE
        binding.status.text = getString(
            R.string.consent_paused, Formatter.formatShortFileSize(this, kept)
        )
        binding.progress.visibility = View.VISIBLE
        binding.progress.isIndeterminate = false
        setPhase(Phase.PAUSED)
    }

    private fun renderDownloadProgress(entry: RootfsCatalog.Entry, downloaded: Long, total: Long) {
        if (total > 0L) {
            binding.progress.isIndeterminate = false
            binding.progress.progress = ((downloaded * 100L) / total).toInt().coerceIn(0, 100)
            binding.status.text = getString(
                R.string.consent_downloading,
                entry.label,
                Formatter.formatShortFileSize(this, downloaded),
                Formatter.formatShortFileSize(this, total)
            )
        } else {
            binding.progress.isIndeterminate = true
            binding.status.text = getString(
                R.string.consent_downloading_unknown,
                entry.label,
                Formatter.formatShortFileSize(this, downloaded)
            )
        }
    }

    private fun onDownloadFinished(result: RootfsDownloader.Result) {
        // No direct fiddling with the button here: each branch below moves the phase, and
        // renderPhase puts the controls into the shape that phase calls for.
        when (result) {
            is RootfsDownloader.Result.Cancelled -> {
                // A pause, not an abandonment. The downloader keeps the .part file, so the
                // bytes already fetched stay on disk and Resume continues from them.
                busy = false
                showPaused()
            }
            is RootfsDownloader.Result.Failed -> {
                busy = false
                // Retry restarts the pipeline, which lands back on the consent panel with
                // the "resume" wording when a partial file survived the failure. A failed
                // download must never trap the user: BusyBox is still there, so offer it.
                showError(getString(R.string.consent_download_failed, result.reason), allowBusybox = true)
            }
            is RootfsDownloader.Result.Ok -> extractDownloaded(result.file)
        }
    }

    private fun extractDownloaded(file: File) {
        val archive = RootfsArchive.classify(file.name, RootfsArchive.Source.LocalFile(file))
        if (archive == null) {
            busy = false
            showError(getString(R.string.provision_failed, "unrecognised archive ${file.name}"))
            return
        }
        binding.progress.isIndeterminate = true
        binding.status.text = getString(R.string.provision_starting)
        // The transfer is over and extraction owns the staging tree from here, so the Cancel
        // button goes away with it. Without this the phase stayed DOWNLOADING and a "Cancel"
        // the extraction could not honour sat on screen for the whole install.
        setPhase(Phase.INSTALLING)
        liveScreen = this
        // Same staging directory, same rule: one pass at a time, reported through
        // whichever instance is on screen.
        if (!provisioningRunning.compareAndSet(false, true)) return
        val ctx = applicationContext
        thread(name = "provisioning-downloaded") {
            val outcome = try {
                BootManager(ctx).provision(archive, relay)
            } catch (t: Throwable) {
                Log.e(ShellLocator.TAG, "[BOOT] unexpected failure while installing", t)
                BootManager.Outcome(
                    BootManager.BootMode.ERROR,
                    "${t.javaClass.simpleName}: ${t.message ?: "unexpected failure"}"
                )
            } finally {
                provisioningRunning.set(false)
            }
            main.post {
                val screen = liveScreen
                if (screen != null) screen.finishBoot(outcome) else finishedOutcome = outcome
            }
        }
    }

    // ------------------------------------------------------------------ helpers

    /**
     * [allowBusybox] adds a way out that does not require the failure to be fixed. It is
     * offered for download failures, where a perfectly good BusyBox terminal is waiting,
     * and withheld for provisioning failures, where the sandbox may be half-written and
     * launching the terminal would hide a real problem.
     */
    private fun showError(message: String, allowBusybox: Boolean = false) {
        busy = false
        errorAllowsBusybox = allowBusybox
        setPhase(Phase.ERROR)
        binding.status.visibility = View.VISIBLE
        binding.progress.isIndeterminate = false
        binding.progress.progress = 0
        binding.progress.visibility = View.VISIBLE
        binding.status.text = message
        binding.consent.visibility = View.GONE
        binding.retry.visibility = View.VISIBLE
        // renderPhase already put the transfer button into its ERROR shape, including whether
        // the BusyBox fallback is offered at all. Nothing installs a listener here: the single
        // listener in onCreate reads the phase, so the label and the action cannot drift apart.
        renderPhase()
    }

    private fun hideAllControls() {
        // The language toggle belongs to the consent screen and nowhere else. Once
        // extraction starts there is nothing left to read, and re-reading strings
        // mid-install would recreate the activity underneath a running provisioning
        // thread. showConsent() is the only place that brings it back.
        binding.langToggle.visibility = View.GONE
        binding.retry.visibility = View.GONE
        binding.cancel.visibility = View.GONE
        binding.consent.visibility = View.GONE
        binding.progress.visibility = View.GONE
    }

    private fun hasNetwork(): Boolean {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager ?: return true
        val network = cm.activeNetwork ?: return false
        val caps = cm.getNetworkCapabilities(network) ?: return false
        return caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
    }

    companion object {
        /** Presses closer together than this are one press. 250 ms is the usual double-tap
         *  threshold: it still swallows an accidental second tap on Resume (which would otherwise
         *  land on Cancel and stop the transfer that had just started) while keeping a deliberate
         *  second press responsive. 400 ms measurably felt like a lag on these two buttons. */
        private const val CLICK_GAP_MS = 250L

        /** Characters that fit one preview line at 10sp monospace on a narrow phone.
         *  Deliberately conservative: over-estimating wraps, which is the bug this
         *  replaced. */
        /**
         * True while a provisioning pass is in flight anywhere in this process.
         *
         * Extraction stages through a `rootfs.tmp` directory it creates itself, so two
         * passes cannot share one; the second gets "cannot create staging dir" and the
         * user sees a failure with a perfectly healthy device underneath it.
         */
        private val provisioningRunning = AtomicBoolean(false)

        /** The instance currently on screen, or null between a destroy and the next
         *  create. The worker thread reports through this rather than through the
         *  instance that started it, so a recreate does not silence the progress. */
        @Volatile
        private var liveScreen: ProvisioningActivity? = null

        /** An outcome that arrived with no screen attached, held for the next one. */
        @Volatile
        private var finishedOutcome: BootManager.Outcome? = null

        /** Reports to whichever screen is current instead of capturing one. */
        private val relay = object : BootManager.Listener {
            override fun onDetail(line: String) {
                liveScreen?.showDetail(line)
            }

            override fun onStage(stage: ProvisioningState.Stage, message: String, percent: Int) {
                liveScreen?.onStageUpdate(message, percent)
            }
        }

    }

}
