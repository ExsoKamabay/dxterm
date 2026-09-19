package com.xdrac.vhdp

/**
 * Diagnostics access to the bundled VHDP library (app/src/main/cpp/vhdp).
 *
 * What this is for: reading VHDP's own view of the device (doctor), its capability matrix,
 * and a static inspection of a rootfs directory. Terminal sessions do not run through this
 * object: they run phdp from nativeLibraryDir (guest.GuestBackend), whose rootless engine starts
 * guests through the userland loader. [run] starts a session inside this process instead; the
 * app does not use it.
 *
 * The native library loads on first use, not at app start. Every call is independent and
 * safe from any thread; each can block (doctor with active probes spawns short-lived
 * children), so keep them off the main thread.
 */
object Vhdp {
    // vhdp_status_t values from vhdp.h (stable, never renumbered).
    const val OK = 0
    const val E_INVALID_ARGUMENT = 1
    const val E_ABI_MISMATCH = 2
    const val E_NO_MEMORY = 3
    const val E_INVALID_STATE = 4
    const val E_UNSUPPORTED = 5
    const val E_ENGINE_UNAVAILABLE = 6
    const val E_ROOTFS_INVALID = 7
    const val E_BIND_INVALID = 8
    const val E_EXEC_FAILED = 9
    const val E_PERMISSION_DENIED = 10
    const val E_TIMEOUT = 11
    const val E_BUFFER_TOO_SMALL = 12
    const val E_IO = 13
    const val E_BUSY = 14
    const val E_CANCELLED = 15
    const val E_NOT_FOUND = 16
    const val E_HOST_ENVIRONMENT = 17
    const val E_INTERNAL = 255

    /** VHDP_DOCTOR_NO_ACTIVE_PROBES: read-only inspection, no probe children. */
    const val DOCTOR_NO_ACTIVE_PROBES = 1
    /** VHDP_DOCTOR_REFRESH: ignore a cached report. */
    const val DOCTOR_REFRESH = 2

    /** Longest session attempt [run] accepts; mirrors kMaxRunTimeoutMs in vhdp_jni.cpp. */
    const val MAX_RUN_TIMEOUT_MS = 600_000L

    /** The ABI this app was compiled against (VHDP_ABI_VERSION). */
    const val ABI_VERSION = 1

    fun abiVersion(): Int = VhdpNative.nativeAbiVersion()

    fun version(): String = VhdpNative.nativeVersion().toString(Charsets.UTF_8)

    /** "VHDP_E_ENGINE_UNAVAILABLE" style name; "VHDP_E_UNKNOWN" for values the library does not define. */
    fun statusName(status: Int): String = VhdpNative.nativeStatusName(status).toString(Charsets.UTF_8)

    fun capabilities(): VhdpResult = VhdpNative.nativeCapabilities()

    fun doctor(flags: Int = 0): VhdpResult = VhdpNative.nativeDoctor(flags)

    fun inspectRootfs(path: String): VhdpResult = VhdpNative.nativeInspectRootfs(path.toByteArray(Charsets.UTF_8))

    /**
     * Configure an already-extracted rootfs for first boot (bind mountpoints, DNS/hosts,
     * login profile, and the normal 'dracos' user with passwordless sudo). Pure filesystem work,
     * done natively by libvhdp, so it works in the app process. Idempotent. The returned
     * [VhdpResult.json] reports {input, rootfs, status, actions[]}.
     */
    fun configureRootfs(path: String): VhdpResult = VhdpNative.nativeConfigureRootfs(path.toByteArray(Charsets.UTF_8))

    /**
     * Decide dpkg/apt recovery for a rootfs. VHDP inspects the package-manager state natively and
     * returns a plan in [VhdpResult.json] ({needs_recovery, script, status_db, locks, ...}). Running
     * the returned script still needs an execution backend (VHDP's rootless engine, or proot as
     * the fallback); this call only decides + supplies the recipe.
     */
    fun dpkgPlan(path: String): VhdpResult = VhdpNative.nativeDpkgPlan(path.toByteArray(Charsets.UTF_8))

    /**
     * Decide the guest system/device/arch projection (feature 4). VHDP reports the host
     * arch/kernel/page-size and which host trees (/dev, /proc, /sys) to project into the guest;
     * [VhdpResult.json] holds {host, system_binds[], status}. The backend that runs the session
     * applies the binds.
     */
    fun projectionPlan(): VhdpResult = VhdpNative.nativeProjectionPlan()

    /**
     * Asks VHDP to run [argv] inside [rootfs] and waits for the result. An empty [argv] means
     * the guest's default shell. [timeoutMs] is enforced by the library and must be in
     * 1..[MAX_RUN_TIMEOUT_MS].
     */
    fun run(rootfs: String, argv: List<String>, timeoutMs: Long): VhdpResult {
        require(timeoutMs in 1..MAX_RUN_TIMEOUT_MS) { "timeoutMs must be in 1..$MAX_RUN_TIMEOUT_MS, was $timeoutMs" }
        return VhdpNative.nativeRun(
            rootfs.toByteArray(Charsets.UTF_8),
            argv.map { it.toByteArray(Charsets.UTF_8) }.toTypedArray(),
            timeoutMs
        )
    }
}
