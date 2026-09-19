package com.xdrac.vhdp

/**
 * Outcome of one libvhdp call, built by the JNI bridge through the (I[B[B[I)V constructor.
 *
 * [status] is the vhdp_status_t the library returned, unchanged. [json] is the report for
 * the JSON queries and empty otherwise. For [Vhdp.run], [phase] says where the attempt
 * stopped and the exit fields are meaningful only when [phase] is [PHASE_DONE].
 */
class VhdpResult(
    val status: Int,
    messageUtf8: ByteArray,
    payloadUtf8: ByteArray,
    private val exit: IntArray
) {
    val message: String = messageUtf8.toString(Charsets.UTF_8)
    val json: String = payloadUtf8.toString(Charsets.UTF_8)

    val ok: Boolean get() = status == Vhdp.OK
    val phase: Int get() = exit[0]
    val state: Int get() = exit[1]
    val reason: Int get() = exit[2]
    val exitCode: Int get() = exit[3]
    val termSignal: Int get() = exit[4]
    val shellStatus: Int get() = exit[5]
    val exitStatus: Int get() = exit[6]

    override fun toString(): String =
        "VhdpResult(status=${Vhdp.statusName(status)}, phase=$phase, state=$state, reason=$reason, " +
            "shellStatus=$shellStatus, message=$message, json=${json.length} chars)"

    companion object {
        const val PHASE_CONFIG = 1
        const val PHASE_CREATE = 2
        const val PHASE_START = 3
        const val PHASE_WAIT = 4
        const val PHASE_DONE = 5
    }
}
