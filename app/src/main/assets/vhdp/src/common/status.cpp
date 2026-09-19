#include "common/status.hpp"

#include "linux_abi/errno_table.h"

namespace vhdp {

const char* code_name(Code code) noexcept {
    switch (code) {
        case Code::ok:
            return "VHDP_OK";
        case Code::invalid_argument:
            return "VHDP_E_INVALID_ARGUMENT";
        case Code::abi_mismatch:
            return "VHDP_E_ABI_MISMATCH";
        case Code::no_memory:
            return "VHDP_E_NO_MEMORY";
        case Code::invalid_state:
            return "VHDP_E_INVALID_STATE";
        case Code::unsupported:
            return "VHDP_E_UNSUPPORTED";
        case Code::engine_unavailable:
            return "VHDP_E_ENGINE_UNAVAILABLE";
        case Code::rootfs_invalid:
            return "VHDP_E_ROOTFS_INVALID";
        case Code::bind_invalid:
            return "VHDP_E_BIND_INVALID";
        case Code::exec_failed:
            return "VHDP_E_EXEC_FAILED";
        case Code::permission_denied:
            return "VHDP_E_PERMISSION_DENIED";
        case Code::timeout:
            return "VHDP_E_TIMEOUT";
        case Code::buffer_too_small:
            return "VHDP_E_BUFFER_TOO_SMALL";
        case Code::io:
            return "VHDP_E_IO";
        case Code::busy:
            return "VHDP_E_BUSY";
        case Code::cancelled:
            return "VHDP_E_CANCELLED";
        case Code::not_found:
            return "VHDP_E_NOT_FOUND";
        case Code::host_environment:
            return "VHDP_E_HOST_ENVIRONMENT";
        case Code::internal:
            return "VHDP_E_INTERNAL";
    }
    return "VHDP_E_UNKNOWN";
}

Status errno_status(Code code, std::string_view what, int err) {
    std::string msg(what);
    msg += ": ";
    msg += vhdp_errno_name(err);
    msg += " (";
    msg += vhdp_errno_description(err);
    msg += ")";
    return {code, std::move(msg)};
}

} // namespace vhdp
