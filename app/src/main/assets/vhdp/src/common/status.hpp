// Internal status/result model. First-party code never throws; the only
// exceptions that can originate inside libvhdp are std::bad_alloc/length_error
// from the standard library, which are caught at every C ABI entry and thread
// entry point (docs/adr/0003-exceptions-rtti.md).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace vhdp {

// Values are identical to the VHDP_* status constants in include/vhdp/vhdp.h
// (checked with static_assert in src/capi/capi.cpp).
enum class Code : std::uint32_t {
    ok = 0,
    invalid_argument = 1,
    abi_mismatch = 2,
    no_memory = 3,
    invalid_state = 4,
    unsupported = 5,
    engine_unavailable = 6,
    rootfs_invalid = 7,
    bind_invalid = 8,
    exec_failed = 9,
    permission_denied = 10,
    timeout = 11,
    buffer_too_small = 12,
    io = 13,
    busy = 14,
    cancelled = 15,
    not_found = 16,
    host_environment = 17,
    internal = 255,
};

const char* code_name(Code code) noexcept;

class [[nodiscard]] Status {
public:
    Status() = default;
    Status(Code code, std::string message) : code_(code), message_(std::move(message)) {}

    static Status ok() { return {}; }

    bool is_ok() const noexcept { return code_ == Code::ok; }
    Code code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }

private:
    Code code_ = Code::ok;
    std::string message_;
};

// Status for a failed host call: "<what>: ENOENT (No such file or directory)".
Status errno_status(Code code, std::string_view what, int err);

template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}
    Result(Status status) : v_(std::in_place_index<1>, std::move(status)) {}

    bool is_ok() const noexcept { return v_.index() == 0; }
    T& value() & noexcept { return *std::get_if<0>(&v_); }
    const T& value() const& noexcept { return *std::get_if<0>(&v_); }
    T&& value() && noexcept { return std::move(*std::get_if<0>(&v_)); }
    const Status& status() const noexcept {
        static const Status ok_status;
        const Status* s = std::get_if<1>(&v_);
        return s != nullptr ? *s : ok_status;
    }
    Status take_status() && { return is_ok() ? Status{} : std::move(*std::get_if<1>(&v_)); }

private:
    std::variant<T, Status> v_;
};

} // namespace vhdp
