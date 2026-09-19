// C ABI implementation. Every entry point:
//  - validates handles, struct_size/abi_version and reserved fields,
//  - converts internal Status to a stable vhdp_status_t and records the message
//    in a thread-local last-error slot,
//  - contains all C++ exceptions (std::bad_alloc -> VHDP_E_NO_MEMORY, anything
//    else -> VHDP_E_INTERNAL); nothing propagates across the boundary.
#include "vhdp/vhdp.h"

#include "common/status.hpp"
#include "core/config.hpp"
#include "core/context.hpp"
#include "core/reports.hpp"
#include "core/session.hpp"
#include "vdr/drivers.hpp"
#include "vhdp_version.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>

namespace {

using vhdp::Code;
using vhdp::Status;

constexpr bool codes_match() {
    return static_cast<vhdp_status_t>(Code::ok) == VHDP_OK &&
           static_cast<vhdp_status_t>(Code::invalid_argument) == VHDP_E_INVALID_ARGUMENT &&
           static_cast<vhdp_status_t>(Code::abi_mismatch) == VHDP_E_ABI_MISMATCH &&
           static_cast<vhdp_status_t>(Code::no_memory) == VHDP_E_NO_MEMORY &&
           static_cast<vhdp_status_t>(Code::invalid_state) == VHDP_E_INVALID_STATE &&
           static_cast<vhdp_status_t>(Code::unsupported) == VHDP_E_UNSUPPORTED &&
           static_cast<vhdp_status_t>(Code::engine_unavailable) == VHDP_E_ENGINE_UNAVAILABLE &&
           static_cast<vhdp_status_t>(Code::rootfs_invalid) == VHDP_E_ROOTFS_INVALID &&
           static_cast<vhdp_status_t>(Code::bind_invalid) == VHDP_E_BIND_INVALID &&
           static_cast<vhdp_status_t>(Code::exec_failed) == VHDP_E_EXEC_FAILED &&
           static_cast<vhdp_status_t>(Code::permission_denied) == VHDP_E_PERMISSION_DENIED &&
           static_cast<vhdp_status_t>(Code::timeout) == VHDP_E_TIMEOUT &&
           static_cast<vhdp_status_t>(Code::buffer_too_small) == VHDP_E_BUFFER_TOO_SMALL &&
           static_cast<vhdp_status_t>(Code::io) == VHDP_E_IO &&
           static_cast<vhdp_status_t>(Code::busy) == VHDP_E_BUSY &&
           static_cast<vhdp_status_t>(Code::cancelled) == VHDP_E_CANCELLED &&
           static_cast<vhdp_status_t>(Code::not_found) == VHDP_E_NOT_FOUND &&
           static_cast<vhdp_status_t>(Code::host_environment) == VHDP_E_HOST_ENVIRONMENT &&
           static_cast<vhdp_status_t>(Code::internal) == VHDP_E_INTERNAL;
}
static_assert(codes_match(), "internal Code values must equal the public VHDP_* constants");
static_assert(static_cast<std::uint32_t>(vhdp::core::EngineKind::vm) == VHDP_ENGINE_VM);
static_assert(static_cast<std::uint32_t>(vhdp::core::State::cancelled) == VHDP_STATE_CANCELLED);
static_assert(static_cast<std::uint32_t>(vhdp::core::ExitReason::start_failed) ==
              VHDP_EXIT_START_FAILED);
static_assert(static_cast<std::uint32_t>(vhdp::core::EventKind::trace) == VHDP_EVENT_TRACE);
static_assert(std::is_standard_layout_v<vhdp_event> && std::is_trivially_copyable_v<vhdp_event>);
static_assert(std::is_standard_layout_v<vhdp_exit_info> &&
              std::is_trivially_copyable_v<vhdp_exit_info>);
static_assert(std::is_standard_layout_v<vhdp_context_options>);

thread_local std::string t_last_error;

vhdp_status_t fail(vhdp_status_t code, std::string msg) noexcept {
    try {
        t_last_error = std::move(msg);
    } catch (...) {
        t_last_error.clear();
    }
    return code;
}

vhdp_status_t from_status(const Status& s) noexcept {
    if (s.is_ok()) {
        return VHDP_OK;
    }
    return fail(static_cast<vhdp_status_t>(s.code()), s.message());
}

template <class Fn>
vhdp_status_t guarded(Fn&& fn) noexcept {
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        return fail(VHDP_E_NO_MEMORY, "out of memory");
    } catch (...) {
        return fail(VHDP_E_INTERNAL, "internal error (exception contained at the C ABI boundary)");
    }
}

bool reserved_zero(const std::uint64_t* r, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (r[i] != 0) {
            return false;
        }
    }
    return true;
}

class CapiSink final : public vhdp::core::EventSink {
public:
    CapiSink(vhdp_event_fn fn, void* user, vhdp_severity_t min) : fn_(fn), user_(user), min_(min) {}

    bool wants(vhdp::core::Severity s) const noexcept override {
        return fn_ != nullptr && static_cast<vhdp_severity_t>(s) >= min_;
    }

    void emit(vhdp::core::Event ev) noexcept override {
        if (!wants(ev.severity)) {
            return;
        }
        vhdp_event e{};
        e.struct_size = sizeof(e);
        e.abi_version = VHDP_ABI_VERSION;
        e.kind = static_cast<vhdp_event_kind_t>(ev.kind);
        e.severity = static_cast<vhdp_severity_t>(ev.severity);
        e.session_id = ev.session_id;
        e.pid = ev.pid;
        e.timestamp_ns = ev.timestamp_ns;
        e.code = ev.code;
        e.name = ev.name.c_str();
        e.message = ev.message.c_str();
        e.detail_json = ev.detail_json.empty() ? "{}" : ev.detail_json.c_str();
        vhdp::vdr::CallbackScope scope(ev.session_id == 0 ? UINT64_MAX : ev.session_id);
        fn_(user_, &e);
    }

private:
    vhdp_event_fn fn_;
    void* user_;
    vhdp_severity_t min_;
};

} // namespace

struct vhdp_context {
    CapiSink sink;
    vhdp::core::Context core;
    vhdp_context(vhdp_event_fn fn, void* user, vhdp_severity_t min)
        : sink(fn, user, min), core(&sink) {}
};

struct vhdp_config {
    vhdp_context* ctx;
    vhdp::core::SessionConfig cfg;
};

struct vhdp_session {
    vhdp_context* ctx;
    std::unique_ptr<vhdp::core::Session> s;
};

namespace {

vhdp_status_t copy_json(const std::string& doc, char* buf, size_t cap, size_t* needed) noexcept {
    if (needed != nullptr) {
        *needed = doc.size() + 1;
    }
    if (buf == nullptr || cap < doc.size() + 1) {
        if (buf != nullptr && cap > 0) {
            std::memcpy(buf, doc.data(), cap - 1);
            buf[cap - 1] = '\0';
        }
        return fail(VHDP_E_BUFFER_TOO_SMALL, "buffer too small for the JSON document");
    }
    std::memcpy(buf, doc.c_str(), doc.size() + 1);
    return VHDP_OK;
}

#define VHDP_CHECK_CONFIG(c)                                                                       \
    if ((c) == nullptr) {                                                                          \
        return fail(VHDP_E_INVALID_ARGUMENT, "config handle is NULL");                             \
    }

} // namespace

extern "C" {

uint32_t vhdp_abi_version(void) {
    return VHDP_ABI_VERSION;
}

const char* vhdp_version_string(void) {
    return VHDP_VERSION_STRING;
}

const char* vhdp_status_name(vhdp_status_t status) {
    switch (status) {
        case VHDP_OK:
        case VHDP_E_INVALID_ARGUMENT:
        case VHDP_E_ABI_MISMATCH:
        case VHDP_E_NO_MEMORY:
        case VHDP_E_INVALID_STATE:
        case VHDP_E_UNSUPPORTED:
        case VHDP_E_ENGINE_UNAVAILABLE:
        case VHDP_E_ROOTFS_INVALID:
        case VHDP_E_BIND_INVALID:
        case VHDP_E_EXEC_FAILED:
        case VHDP_E_PERMISSION_DENIED:
        case VHDP_E_TIMEOUT:
        case VHDP_E_BUFFER_TOO_SMALL:
        case VHDP_E_IO:
        case VHDP_E_BUSY:
        case VHDP_E_CANCELLED:
        case VHDP_E_NOT_FOUND:
        case VHDP_E_HOST_ENVIRONMENT:
        case VHDP_E_INTERNAL:
            return vhdp::code_name(static_cast<Code>(status));
        default:
            return "VHDP_E_UNKNOWN";
    }
}

vhdp_status_t vhdp_last_error_message(char* buf, size_t cap, size_t* needed) {
    const std::string& m = t_last_error;
    if (needed != nullptr) {
        *needed = m.size() + 1;
    }
    if (buf == nullptr || cap < m.size() + 1) {
        if (buf != nullptr && cap > 0) {
            std::memcpy(buf, m.data(), cap - 1);
            buf[cap - 1] = '\0';
        }
        return VHDP_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(buf, m.c_str(), m.size() + 1);
    return VHDP_OK;
}

vhdp_status_t vhdp_context_create(const vhdp_context_options* options, vhdp_context** out_context) {
    return guarded([&]() -> vhdp_status_t {
        if (out_context == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "out_context is NULL");
        }
        *out_context = nullptr;
        vhdp_event_fn fn = nullptr;
        void* user = nullptr;
        vhdp_severity_t min = VHDP_SEVERITY_INFO;
        if (options != nullptr) {
            if (options->struct_size < sizeof(vhdp_context_options) ||
                options->abi_version != VHDP_ABI_VERSION) {
                return fail(VHDP_E_ABI_MISMATCH,
                            "vhdp_context_options struct_size/abi_version mismatch");
            }
            if (options->flags != 0 || !reserved_zero(options->reserved, 4)) {
                return fail(VHDP_E_INVALID_ARGUMENT,
                            "vhdp_context_options flags/reserved fields must be zero");
            }
            if (options->min_severity > VHDP_SEVERITY_ERROR) {
                return fail(VHDP_E_INVALID_ARGUMENT, "invalid min_severity");
            }
            fn = options->event_fn;
            user = options->event_user;
            min = options->min_severity;
        }
        *out_context = new vhdp_context(fn, user, min);
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_context_destroy(vhdp_context* context) {
    return guarded([&]() -> vhdp_status_t {
        if (context == nullptr) {
            return VHDP_OK;
        }
        if (vhdp::vdr::CallbackScope::inside_any()) {
            return fail(VHDP_E_BUSY, "vhdp_context_destroy called from inside a callback");
        }
        if (context->core.live_sessions().load() != 0) {
            return fail(VHDP_E_BUSY, "context still has live sessions");
        }
        delete context;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_doctor_json(vhdp_context* context, uint32_t flags, char* buf, size_t cap,
                               size_t* needed) {
    return guarded([&]() -> vhdp_status_t {
        if (context == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "context is NULL");
        }
        if ((flags & ~(VHDP_DOCTOR_NO_ACTIVE_PROBES | VHDP_DOCTOR_REFRESH)) != 0) {
            return fail(VHDP_E_INVALID_ARGUMENT, "unknown doctor flags");
        }
        std::string doc = context->core.doctor_json((flags & VHDP_DOCTOR_NO_ACTIVE_PROBES) == 0,
                                                    (flags & VHDP_DOCTOR_REFRESH) != 0);
        return copy_json(doc, buf, cap, needed);
    });
}

vhdp_status_t vhdp_capabilities_json(vhdp_context* context, char* buf, size_t cap, size_t* needed) {
    return guarded([&]() -> vhdp_status_t {
        if (context == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "context is NULL");
        }
        return copy_json(context->core.capabilities_json(), buf, cap, needed);
    });
}

vhdp_status_t vhdp_inspect_rootfs_json(vhdp_context* context, const char* rootfs_path, char* buf,
                                       size_t cap, size_t* needed) {
    return guarded([&]() -> vhdp_status_t {
        if (context == nullptr || rootfs_path == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "context or rootfs_path is NULL");
        }
        return copy_json(context->core.inspect_json(rootfs_path), buf, cap, needed);
    });
}

vhdp_status_t vhdp_configure_rootfs(vhdp_context* context, const char* rootfs_path, char* buf,
                                    size_t cap, size_t* needed) {
    return guarded([&]() -> vhdp_status_t {
        if (context == nullptr || rootfs_path == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "context or rootfs_path is NULL");
        }
        return copy_json(vhdp::core::build_configure_json(rootfs_path), buf, cap, needed);
    });
}

vhdp_status_t vhdp_dpkg_plan_json(vhdp_context* context, const char* rootfs_path, char* buf,
                                  size_t cap, size_t* needed) {
    return guarded([&]() -> vhdp_status_t {
        if (context == nullptr || rootfs_path == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "context or rootfs_path is NULL");
        }
        return copy_json(vhdp::core::build_dpkg_plan_json(rootfs_path), buf, cap, needed);
    });
}

vhdp_status_t vhdp_projection_plan_json(vhdp_context* context, char* buf, size_t cap,
                                        size_t* needed) {
    return guarded([&]() -> vhdp_status_t {
        if (context == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "context is NULL");
        }
        return copy_json(vhdp::core::build_projection_json(), buf, cap, needed);
    });
}

vhdp_status_t vhdp_config_create(vhdp_context* context, vhdp_config** out_config) {
    return guarded([&]() -> vhdp_status_t {
        if (context == nullptr || out_config == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "context or out_config is NULL");
        }
        *out_config = new vhdp_config{context, {}};
        return VHDP_OK;
    });
}

void vhdp_config_destroy(vhdp_config* config) {
    delete config;
}

vhdp_status_t vhdp_config_set_rootfs(vhdp_config* config, const char* host_path) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (host_path == nullptr || host_path[0] == '\0') {
            return fail(VHDP_E_INVALID_ARGUMENT, "rootfs path is NULL or empty");
        }
        config->cfg.rootfs = host_path;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_engine(vhdp_config* config, vhdp_engine_kind_t engine) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (engine > VHDP_ENGINE_VM) {
            return fail(VHDP_E_INVALID_ARGUMENT, "unknown engine kind");
        }
        config->cfg.engine = static_cast<vhdp::core::EngineKind>(engine);
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_cwd(vhdp_config* config, const char* guest_path) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (guest_path == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "cwd is NULL");
        }
        if (Status s = vhdp::core::check_guest_path(guest_path, "cwd"); !s.is_ok()) {
            return from_status(s);
        }
        config->cfg.cwd = guest_path;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_argv(vhdp_config* config, size_t argc, const char* const* argv) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (argc > vhdp::core::kMaxArgs || (argc > 0 && argv == nullptr)) {
            return fail(VHDP_E_INVALID_ARGUMENT, "invalid argc/argv");
        }
        std::vector<std::string> v;
        v.reserve(argc);
        for (size_t i = 0; i < argc; ++i) {
            if (argv[i] == nullptr) {
                return fail(VHDP_E_INVALID_ARGUMENT, "argv contains a NULL entry");
            }
            v.emplace_back(argv[i]);
        }
        config->cfg.argv = std::move(v);
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_add_env(vhdp_config* config, const char* key_value) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (key_value == nullptr || !vhdp::core::valid_env_entry(key_value)) {
            return fail(VHDP_E_INVALID_ARGUMENT, "environment entry must be KEY=VALUE");
        }
        if (config->cfg.env.size() >= vhdp::core::kMaxEnv) {
            return fail(VHDP_E_INVALID_ARGUMENT, "too many environment entries");
        }
        config->cfg.env.emplace_back(key_value);
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_clear_env(vhdp_config* config, uint32_t clear) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (clear > 1) {
            return fail(VHDP_E_INVALID_ARGUMENT, "clear must be 0 or 1");
        }
        config->cfg.clear_env = clear == 1;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_add_bind(vhdp_config* config, const char* host_path,
                                   const char* guest_path, vhdp_bind_flags_t flags) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (host_path == nullptr || guest_path == nullptr || host_path[0] == '\0') {
            return fail(VHDP_E_BIND_INVALID, "bind host/guest path is NULL or empty");
        }
        if (flags > VHDP_BIND_READ_WRITE) {
            return fail(VHDP_E_INVALID_ARGUMENT, "unknown bind flags");
        }
        if (Status s = vhdp::core::check_guest_path(guest_path, "bind guest path"); !s.is_ok()) {
            return fail(VHDP_E_BIND_INVALID, s.message());
        }
        if (config->cfg.binds.size() >= vhdp::core::kMaxBinds) {
            return fail(VHDP_E_BIND_INVALID, "too many binds");
        }
        config->cfg.binds.push_back({host_path, guest_path, flags == VHDP_BIND_READ_WRITE});
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_read_only_rootfs(vhdp_config* config, uint32_t read_only) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (read_only > 1) {
            return fail(VHDP_E_INVALID_ARGUMENT, "read_only must be 0 or 1");
        }
        config->cfg.read_only_rootfs = read_only == 1;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_network(vhdp_config* config, vhdp_network_t network) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (network > VHDP_NETWORK_NONE) {
            return fail(VHDP_E_INVALID_ARGUMENT, "unknown network policy");
        }
        config->cfg.network = static_cast<vhdp::core::NetworkPolicy>(network);
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_uid(vhdp_config* config, uint32_t uid) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (uid == UINT32_MAX) {
            return fail(VHDP_E_INVALID_ARGUMENT, "uid 4294967295 is reserved");
        }
        config->cfg.uid = uid;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_gid(vhdp_config* config, uint32_t gid) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (gid == UINT32_MAX) {
            return fail(VHDP_E_INVALID_ARGUMENT, "gid 4294967295 is reserved");
        }
        config->cfg.gid = gid;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_timeout_ms(vhdp_config* config, uint64_t timeout_ms) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (timeout_ms > vhdp::core::kMaxTimeoutMs) {
            return fail(VHDP_E_INVALID_ARGUMENT, "timeout is longer than one year");
        }
        config->cfg.timeout_ms = timeout_ms;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_trace(vhdp_config* config, uint32_t trace) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (trace > 1) {
            return fail(VHDP_E_INVALID_ARGUMENT, "trace must be 0 or 1");
        }
        config->cfg.trace = trace == 1;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_stdio(vhdp_config* config, vhdp_stdio_mode_t mode, int32_t stdin_fd,
                                    int32_t stdout_fd, int32_t stderr_fd) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (mode > VHDP_STDIO_FDS) {
            return fail(VHDP_E_INVALID_ARGUMENT, "unknown stdio mode");
        }
        if (mode != VHDP_STDIO_FDS && (stdin_fd != -1 || stdout_fd != -1 || stderr_fd != -1)) {
            return fail(VHDP_E_INVALID_ARGUMENT,
                        "file descriptors are only accepted with VHDP_STDIO_FDS (pass -1)");
        }
        config->cfg.stdio = static_cast<vhdp::core::StdioMode>(mode);
        config->cfg.stdio_fds[0] = stdin_fd;
        config->cfg.stdio_fds[1] = stdout_fd;
        config->cfg.stdio_fds[2] = stderr_fd;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_pty_size(vhdp_config* config, uint16_t rows, uint16_t cols) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (rows == 0 || cols == 0) {
            return fail(VHDP_E_INVALID_ARGUMENT, "rows and cols must be non-zero");
        }
        config->cfg.pty_rows = rows;
        config->cfg.pty_cols = cols;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_output_callback(vhdp_config* config, vhdp_output_fn fn, void* user) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        config->cfg.output_fn = fn;
        config->cfg.output_user = user;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_seccomp(vhdp_config* config, vhdp_seccomp_mode_t mode) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (mode > VHDP_SECCOMP_OFF) {
            return fail(VHDP_E_INVALID_ARGUMENT, "unknown seccomp mode");
        }
        config->cfg.seccomp = static_cast<vhdp::core::SeccompMode>(mode);
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_dev(vhdp_config* config, vhdp_dev_mode_t mode) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (mode > VHDP_DEV_NONE) {
            return fail(VHDP_E_INVALID_ARGUMENT, "unknown dev mode");
        }
        config->cfg.dev = static_cast<vhdp::core::DevMode>(mode);
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_config_set_proc(vhdp_config* config, vhdp_proc_mode_t mode) {
    return guarded([&]() -> vhdp_status_t {
        VHDP_CHECK_CONFIG(config);
        if (mode > VHDP_PROC_HOST) {
            return fail(VHDP_E_INVALID_ARGUMENT, "unknown proc mode");
        }
        config->cfg.proc = static_cast<vhdp::core::ProcMode>(mode);
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_session_create(vhdp_context* context, const vhdp_config* config,
                                  vhdp_session** out_session) {
    return guarded([&]() -> vhdp_status_t {
        if (context == nullptr || config == nullptr || out_session == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "context, config or out_session is NULL");
        }
        *out_session = nullptr;
        if (config->ctx != context) {
            return fail(VHDP_E_INVALID_ARGUMENT, "config was created by a different context");
        }
        auto validated = vhdp::core::validate_config(
            config->cfg, [](const char* name) -> std::optional<std::string> {
                const char* v = std::getenv(name);
                if (v == nullptr) {
                    return std::nullopt;
                }
                return std::string(v);
            });
        if (!validated.is_ok()) {
            return from_status(validated.status());
        }
        auto session = std::make_unique<vhdp_session>();
        session->ctx = context;
        session->s =
            std::make_unique<vhdp::core::Session>(context->core, std::move(validated.value()));
        *out_session = session.release();
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_session_start(vhdp_session* session) {
    return guarded([&]() -> vhdp_status_t {
        if (session == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "session is NULL");
        }
        return from_status(session->s->start());
    });
}

vhdp_status_t vhdp_session_wait(vhdp_session* session, int64_t timeout_ms, vhdp_exit_info* out) {
    return guarded([&]() -> vhdp_status_t {
        if (session == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "session is NULL");
        }
        if (out != nullptr &&
            (out->struct_size < sizeof(vhdp_exit_info) || out->abi_version != VHDP_ABI_VERSION)) {
            return fail(VHDP_E_ABI_MISMATCH, "vhdp_exit_info struct_size/abi_version mismatch");
        }
        if (vhdp::vdr::CallbackScope::current() == session->s->id()) {
            return fail(VHDP_E_BUSY,
                        "vhdp_session_wait called from a callback of the same session");
        }
        if (session->s->state() == vhdp::core::State::created) {
            return fail(VHDP_E_INVALID_STATE, "session has not been started");
        }
        vhdp::core::ExitResult r;
        vhdp::core::State st = vhdp::core::State::created;
        auto timeout = std::chrono::milliseconds(timeout_ms < 0 ? -1 : timeout_ms);
        if (!session->s->wait(timeout, r, st)) {
            return fail(VHDP_E_TIMEOUT, "session is still running");
        }
        if (out != nullptr) {
            std::memset(out, 0, sizeof(*out));
            out->struct_size = sizeof(*out);
            out->abi_version = VHDP_ABI_VERSION;
            out->state = static_cast<vhdp_session_state_t>(st);
            out->reason = static_cast<vhdp_exit_reason_t>(r.reason);
            out->exit_code = r.exit_code;
            out->term_signal = r.term_signal;
            out->shell_status = vhdp::core::shell_status(r);
            out->status = static_cast<vhdp_status_t>(r.status.code());
        }
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_session_state(vhdp_session* session, vhdp_session_state_t* out_state) {
    return guarded([&]() -> vhdp_status_t {
        if (session == nullptr || out_state == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "session or out_state is NULL");
        }
        *out_state = static_cast<vhdp_session_state_t>(session->s->state());
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_session_engine(vhdp_session* session, vhdp_engine_kind_t* out_engine) {
    return guarded([&]() -> vhdp_status_t {
        if (session == nullptr || out_engine == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "session or out_engine is NULL");
        }
        *out_engine = static_cast<vhdp_engine_kind_t>(session->s->engine_kind());
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_session_signal(vhdp_session* session, int32_t signo) {
    return guarded([&]() -> vhdp_status_t {
        if (session == nullptr || signo < 0 || signo > 64) {
            return fail(VHDP_E_INVALID_ARGUMENT, "session is NULL or signal out of range");
        }
        return from_status(session->s->signal(signo));
    });
}

vhdp_status_t vhdp_session_resize(vhdp_session* session, uint16_t rows, uint16_t cols) {
    return guarded([&]() -> vhdp_status_t {
        if (session == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "session is NULL");
        }
        return from_status(session->s->resize(rows, cols));
    });
}

vhdp_status_t vhdp_session_write_input(vhdp_session* session, const uint8_t* data, size_t len,
                                       size_t* out_written) {
    return guarded([&]() -> vhdp_status_t {
        if (session == nullptr || (data == nullptr && len > 0)) {
            return fail(VHDP_E_INVALID_ARGUMENT, "session or data is NULL");
        }
        size_t written = 0;
        vhdp_status_t r = from_status(session->s->write_input(data, len, written));
        if (out_written != nullptr) {
            *out_written = written;
        }
        return r;
    });
}

vhdp_status_t vhdp_session_pty_master(vhdp_session* session, int32_t* out_fd) {
    return guarded([&]() -> vhdp_status_t {
        if (session == nullptr || out_fd == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "session or out_fd is NULL");
        }
        int fd = session->s->pty_master();
        if (fd < 0) {
            *out_fd = -1;
            return fail(VHDP_E_INVALID_STATE,
                        "no PTY: session not started or not in VHDP_STDIO_PTY mode");
        }
        *out_fd = fd;
        return VHDP_OK;
    });
}

vhdp_status_t vhdp_session_cancel(vhdp_session* session) {
    return guarded([&]() -> vhdp_status_t {
        if (session == nullptr) {
            return fail(VHDP_E_INVALID_ARGUMENT, "session is NULL");
        }
        return from_status(session->s->cancel());
    });
}

vhdp_status_t vhdp_session_destroy(vhdp_session* session) {
    return guarded([&]() -> vhdp_status_t {
        if (session == nullptr) {
            return VHDP_OK;
        }
        if (vhdp::vdr::CallbackScope::current() == session->s->id()) {
            return fail(VHDP_E_BUSY,
                        "vhdp_session_destroy called from a callback of the same session");
        }
        delete session;
        return VHDP_OK;
    });
}

} // extern "C"
