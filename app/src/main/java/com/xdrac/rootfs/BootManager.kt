package com.xdrac.rootfs

import android.content.Context
import android.util.Log
import java.io.File

/**
 * Runs source resolution, validation, extraction, configuration and verification in order, and
 * decides how to boot:
 *   - rootfs already valid                   -> LINUX       (provisioning skipped)
 *   - an image is bundled in the APK         -> provision offline, then LINUX
 *   - an image was already downloaded        -> provision, then LINUX
 *   - no archive, offer not yet declined     -> NEEDS_IMAGE (caller asks the user)
 *   - no archive, offer declined             -> BUSYBOX     (a working terminal; not an error)
 *   - archive present but fails              -> ERROR       (halt with a clear reason)
 * Runs on the calling (background) thread; UI updates flow through [Listener].
 */
class BootManager(private val ctx: Context) {

    enum class BootMode { LINUX, BUSYBOX, NEEDS_IMAGE, ERROR }
    data class Outcome(val mode: BootMode, val message: String)

    interface Listener {
        fun onStage(stage: ProvisioningState.Stage, message: String, percent: Int) // percent<0 => indeterminate

        /**
         * Fine-grained activity for the live preview: a path being written, a log line.
         *
         * Separate from [onStage] because the two answer different questions. A stage says
         * "extracting"; this says which file, right now. Default no-op so a caller that
         * only wants stages does not have to care.
         */
        fun onDetail(line: String) {}
    }

    private val discovery = RootfsDiscovery(ctx)
    private val sources = RootfsSourceResolver(ctx)
    private val validator = RootfsValidator(ctx)
    private val extractor = RootfsExtractor(ctx)
    private val configurator = RootfsConfigurator()
    private val runtime = RuntimeValidator(ctx)
    private val state = ProvisioningState(ctx)
    private val rootfs = File(ctx.filesDir, "rootfs")

    fun boot(listener: Listener): Outcome {
        Log.i(ShellLocator.TAG, "[BOOT] BootManager.boot() start; rootfs=${rootfs.absolutePath}")
        reclaimStaleTrees()
        // 1. Already provisioned and still valid -> straight to Linux.
        if (state.isProvisioned() && runtime.isRootfsReady()) {
            runRecovery(listener)
            listener.onStage(ProvisioningState.Stage.DONE, "Linux environment ready", 100)
            return Outcome(BootMode.LINUX, "reused existing rootfs")
        }

        // 2. Which installer is this? One question, one answer, asked in one place.
        listener.onStage(ProvisioningState.Stage.NONE, "Looking for a Linux image…", 3)
        return when (val source = sources.resolve()) {
            // Offline: the image is inside the APK. Install it straight away -- no catalogue,
            // no consent panel, no network. A failure here is reported as a failure rather
            // than quietly demoted to a download, because the APK does carry the payload.
            is RootfsSourceResolver.Source.Bundled -> {
                listener.onStage(
                    ProvisioningState.Stage.NONE,
                    "Preparing bundled image ${source.archive.fileName}…", 5
                )
                provision(source.archive, listener)
            }

            // Online: nothing shipped with the APK. An image the user already downloaded is
            // still used first, so a retry after a failed or interrupted install does not
            // spend the download again.
            RootfsSourceResolver.Source.RemoteCatalog -> {
                val downloaded = discovery.selectLocal()
                if (downloaded != null) return provision(downloaded, listener)

                if (!runtime.isBusyboxReady()) {
                    return Outcome(BootMode.ERROR, "No Linux image and BusyBox is missing")
                }
                // BusyBox works. Offer the download once; after that, stop asking.
                if (state.imageOfferDeclined) {
                    listener.onStage(ProvisioningState.Stage.DONE, "Starting BusyBox shell", 100)
                    Outcome(BootMode.BUSYBOX, "busybox mode (image offer previously declined)")
                } else {
                    Outcome(BootMode.NEEDS_IMAGE, "no Linux image present")
                }
            }
        }
    }

    /**
     * Runs the pipeline for an archive that is already on the device or in the APK.
     * Public so the caller can invoke it directly after a download completes, without
     * re-running discovery from scratch.
     */
    fun provision(archive: RootfsArchive, listener: Listener): Outcome {
        // 3. Validate integrity + capacity.
        listener.onStage(ProvisioningState.Stage.VALIDATED, "Validating ${archive.fileName}…", 8)
        when (val v = validator.validate(archive, rootfs)) {
            is RootfsValidator.Result.Invalid -> { state.markFailed(v.reason); return Outcome(BootMode.ERROR, v.reason) }
            RootfsValidator.Result.Ok -> {}
        }

        // 4. Extract (indeterminate; the entry count is unknown while streaming).
        state.stage = ProvisioningState.Stage.EXTRACTING
        val ext = extractor.extract(archive, rootfs) { entries, _, currentPath ->
            listener.onStage(ProvisioningState.Stage.EXTRACTING, "Extracting… $entries files", -1)
            // The extractor already knew which path it was on; it just had nowhere to say
            // it. This is the only part of provisioning long enough for a live view to
            // show anything, so it is the part worth showing.
            if (currentPath.isNotBlank()) listener.onDetail(currentPath)
        }
        if (ext is RootfsExtractor.Result.Failed) { state.markFailed(ext.reason); return Outcome(BootMode.ERROR, ext.reason) }

        // 5. Configure.
        listener.onDetail("Configuring environment…"); listener.onStage(ProvisioningState.Stage.CONFIGURING, "Configuring environment…", 92)
        when (val c = configurator.configure(rootfs)) {
            is RootfsConfigurator.Result.Failed -> { state.markFailed(c.reason); return Outcome(BootMode.ERROR, c.reason) }
            RootfsConfigurator.Result.Ok -> {}
        }

        // 6. Verify the installed environment can actually run.
        listener.onDetail("Verifying installation…"); listener.onStage(ProvisioningState.Stage.VERIFYING, "Verifying installation…", 97)
        val report = runtime.validate()
        if (!report.ok) { state.markFailed(report.detail); return Outcome(BootMode.ERROR, report.detail) }

        state.markDone(archive.fileName)

        // The archive has served its purpose and is the largest thing in the sandbox.
        // Only on-device copies are removed; an asset lives in the APK and is not ours to delete.
        if (archive.source is RootfsArchive.Source.LocalFile) {
            listener.onDetail("Reclaiming space…"); listener.onStage(ProvisioningState.Stage.VERIFYING, "Reclaiming space…", 99)
            discovery.discardLocalImages()
        }

        runRecovery(listener)
        listener.onStage(ProvisioningState.Stage.DONE, "Linux ready", 100)
        return Outcome(BootMode.LINUX, "provisioned ${archive.fileName}")
    }

    /**
     * Delete a `rootfs.old` or `rootfs.tmp` left by an interrupted extraction.
     *
     * The extractor renames the previous tree aside and only deletes it once the new one is in
     * place; a kill in that window leaves a full second copy -- hundreds of megabytes -- that
     * nothing reclaims until the user happens to provision again. Doing it at boot means the
     * space comes back on the next launch instead, and the free-space check in RootfsDownloader
     * then measures what is really available.
     *
     * Only ever touches the two names the extractor itself creates, and only when the live
     * rootfs is not one of them.
     */
    private fun reclaimStaleTrees() {
        for (suffix in listOf(".old", ".tmp")) {
            val stale = File(rootfs.parentFile, rootfs.name + suffix)
            if (!stale.isDirectory) continue
            val freed = runCatching { stale.walkBottomUp().filter { it.isFile }.sumOf { it.length() } }
                .getOrDefault(0L)
            if (runCatching { stale.deleteRecursively() }.getOrDefault(false)) {
                Log.i(ShellLocator.TAG, "[BOOT] reclaimed ${stale.name} ($freed bytes) from an interrupted install")
            }
        }
    }

    /** Repair apt/dpkg ownership + interrupted state before handing off to Linux. Marker-gated,
     *  best-effort, never fatal (a recovery failure must not block a working terminal). */
    private fun runRecovery(listener: Listener) {
        listener.onDetail("Finalizing package manager…"); listener.onStage(ProvisioningState.Stage.VERIFYING, "Finalizing package manager…", -1)
        runCatching { DpkgRecoveryEngine.run(ctx, rootfs) }
            .onFailure { Log.w(ShellLocator.TAG, "[BOOT] recovery skipped: ${it.message}") }
    }
}
