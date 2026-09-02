dracXterm - Linux RootFS image location
=======================================

What is in this folder decides which installer the build produces. There is one
source set and one build pipeline; there are no product flavours.

    empty (this README only)  ->  ONLINE installer
        The APK carries no image. On first run the app lists the images this device
        can actually run, from assets/rootfsURLS.json filtered by ABI, downloads the
        one the user picks, verifies it against the pinned SHA-256, then unpacks it.
        Nothing is fetched until the user asks for it.

    exactly one archive       ->  OFFLINE installer
        The archive is packaged into the APK. On first run the app extracts it
        straight from assets: no distro list, no network, no consent prompt.

    more than one archive     ->  BUILD FAILS
        The build cannot know which image was meant, so it refuses and names the
        files it found. Keep one, or none.

Supported archive extensions:
    .tar.xz   .txz   .tar.gz   .tgz   .tar

Anything else in this folder (this README, .gitkeep, checksums, notes) is NOT an
archive and is ignored by both the build validation and the app.

Building your own APK with an image inside it
---------------------------------------------

    ./scripts/fetch-rootfs.sh --list      # what is on offer
    ./scripts/fetch-rootfs.sh             # puts one archive here
    ./gradlew assembleDebug               # now an offline installer

Only arm64 / aarch64 images match this build's ABI (arm64-v8a); nothing else can run.

Do NOT commit an image here. A ~200 MB archive in git history bloats every clone and,
via Git LFS, exhausts the free bandwidth quota after a handful of fetches. Keep it in
your local working copy only.

How the app decides, at runtime
-------------------------------

One class: com.xdrac.rootfs.RootfsSourceResolver. It lists this directory through
AssetManager and classifies the names; BootManager asks it and does nothing else to
work out the mode. No BuildConfig field, no Gradle task name, no APK name.

Where the app puts a downloaded image
-------------------------------------

    <filesDir>/rootfs-image/<name>.tar.xz     (deleted after a successful extraction)
    <filesDir>/rootfs/                        (the extracted environment)

The download URLs and checksums live in app/src/main/assets/rootfsURLS.json.
