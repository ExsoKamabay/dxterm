package com.xdrac.rootfs

import android.content.Context
import android.util.Log

/**
 * The one place that answers "where does this build's Linux image come from".
 *
 * There is no build flavour, no BuildConfig field and nothing in the APK's name that says
 * whether this is the offline or the online installer. The answer is the packaged assets
 * themselves: `assets/rootfs/` either carries an archive or it does not, and the same
 * directory the build validated is the directory read back here through [android.content.res.AssetManager].
 * Keeping that decision in one class is the point: a mode test copied into several classes
 * is how the two ways of installing drift apart.
 *
 * Only the *asset* side is decided here. An image the user already downloaded lives in the
 * app's own directory and stays [RootfsDiscovery]'s business, because it is not a property
 * of the build.
 */
class RootfsSourceResolver(private val ctx: Context) {

    sealed class Source {
        /** An archive shipped inside the APK: extract it on first run, no network, no questions. */
        data class Bundled(val archive: RootfsArchive) : Source()
        /** No archive in the APK: offer the ABI-filtered catalogue from `assets/rootfsURLS.json`. */
        data object RemoteCatalog : Source()
    }

    /**
     * Archives under `assets/rootfs/`, newest classification rules applied.
     *
     * `AssetManager.list` returns every name in the directory, so the README and any
     * placeholder come back too; [RootfsArchive.classify] is what rejects them, and it is the
     * same rule the Gradle validation uses. A name it does not recognise is never treated as
     * an image -- an unreadable "rootfs" that silently became a BusyBox-only first run is
     * exactly the failure this guards.
     */
    fun bundledArchives(): List<RootfsArchive> {
        val names = runCatching { ctx.assets.list(ASSET_DIR)?.toList() ?: emptyList() }
            .onFailure { Log.w(ShellLocator.TAG, "[SOURCE] cannot list assets/$ASSET_DIR: ${it.message}") }
            .getOrDefault(emptyList())
        return names.sorted().mapNotNull {
            RootfsArchive.classify(it, RootfsArchive.Source.Asset("$ASSET_DIR/$it"))
        }
    }

    /**
     * Which installer this build is.
     *
     * More than one archive cannot come out of a validated build (`validateBundledRootfs`
     * fails first), so reaching that branch means the APK was assembled some other way. It
     * still installs offline rather than falling back to a download: the payload is
     * demonstrably in the APK, and quietly asking the user to fetch a second copy over the
     * network would be the worse answer. The condition is logged loudly.
     */
    fun resolve(): Source {
        val bundled = bundledArchives()
        if (bundled.isEmpty()) {
            Log.i(ShellLocator.TAG, "[SOURCE] no image in assets/$ASSET_DIR -> online installer (catalogue)")
            return Source.RemoteCatalog
        }
        if (bundled.size > 1) {
            Log.e(ShellLocator.TAG,
                "[SOURCE] ${bundled.size} images in assets/$ASSET_DIR (" +
                    bundled.joinToString { it.fileName } + "); the build should have refused this.")
        }

        // The APK is universal (arm64-v8a + x86_64), but a bundled offline image is a single
        // architecture. Using an arm64 image on an x86_64 device (or vice versa) installs a rootfs
        // whose /bin/sh cannot exec -- a broken terminal. So a bundled image is only used offline
        // when its arch matches the device (or is unnamed/unknown, where we cannot tell and keep the
        // old behaviour). When every bundled image is a KNOWN mismatch, fall through to the online
        // catalogue, which serves this device's matching arch from assets/rootfsURLS.json.
        val deviceArch = RootfsDiscovery.deviceArch()
        val usable = bundled.firstOrNull { it.arch == deviceArch || it.arch == "unknown" }
        if (usable == null) {
            Log.w(ShellLocator.TAG,
                "[SOURCE] bundled image(s) [" + bundled.joinToString { "${it.fileName} (${it.arch})" } +
                    "] do not match device arch '$deviceArch' -> online installer (catalogue) for the matching arch")
            return Source.RemoteCatalog
        }
        Log.i(ShellLocator.TAG,
            "[SOURCE] bundled image ${usable.fileName} (arch=${usable.arch}, device=$deviceArch) -> offline installer")
        return Source.Bundled(usable)
    }

    companion object {
        /** Asset directory the build packages a local image into. */
        const val ASSET_DIR = RootfsDiscovery.ASSET_DIR
    }
}
