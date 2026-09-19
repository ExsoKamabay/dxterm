package com.dracxterm.vhdp

/**
 * Raw JNI surface of libvhdpjni.so (app/src/main/cpp/vhdp/vhdp_jni.cpp). Natives are bound
 * with RegisterNatives in JNI_OnLoad, so the names and signatures here must match kMethods
 * there. Strings cross as UTF-8 byte arrays. Use [Vhdp] instead of calling this directly.
 */
internal object VhdpNative {
    init { System.loadLibrary("vhdpjni") }

    @JvmStatic external fun nativeAbiVersion(): Int
    @JvmStatic external fun nativeVersion(): ByteArray
    @JvmStatic external fun nativeStatusName(status: Int): ByteArray
    @JvmStatic external fun nativeCapabilities(): VhdpResult
    @JvmStatic external fun nativeDoctor(flags: Int): VhdpResult
    @JvmStatic external fun nativeInspectRootfs(path: ByteArray): VhdpResult
    @JvmStatic external fun nativeConfigureRootfs(path: ByteArray): VhdpResult
    @JvmStatic external fun nativeDpkgPlan(path: ByteArray): VhdpResult
    @JvmStatic external fun nativeProjectionPlan(): VhdpResult
    @JvmStatic external fun nativeRun(rootfs: ByteArray, argv: Array<ByteArray>, timeoutMs: Long): VhdpResult
}
