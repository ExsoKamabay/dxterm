// libvhdp - thin header-only C++20 RAII wrapper over the stable C ABI (vhdp.h).
//
// This header is compiled by the consumer: it depends only on the C ABI, so the
// consumer's C++ standard library never has to match the one used to build
// libvhdp. The wrapper does not throw; fallible operations return vhdp::Status or
// vhdp::Result<T>. Exceptions escaping user callbacks are caught at the C
// boundary and reported as a log line to stderr (they cannot propagate into C).
#ifndef VHDP_VHDP_HPP
#define VHDP_VHDP_HPP

#include "vhdp.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vhdp {

class Status {
public:
    Status() = default;
    Status(vhdp_status_t code, std::string message) : code_(code), message_(std::move(message)) {}

    [[nodiscard]] bool ok() const noexcept { return code_ == VHDP_OK; }
    [[nodiscard]] vhdp_status_t code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] const char* name() const noexcept { return vhdp_status_name(code_); }
    explicit operator bool() const noexcept { return ok(); }

    // Builds a Status from a C ABI return value, capturing the thread's last error text.
    static Status from_c(vhdp_status_t code) {
        if (code == VHDP_OK) {
            return {};
        }
        std::string msg(256, '\0');
        size_t needed = 0;
        vhdp_status_t r = vhdp_last_error_message(msg.data(), msg.size(), &needed);
        if (r == VHDP_E_BUFFER_TOO_SMALL && needed > msg.size()) {
            msg.assign(needed, '\0');
            r = vhdp_last_error_message(msg.data(), msg.size(), &needed);
        }
        if (r == VHDP_OK && needed > 0) {
            msg.resize(needed - 1);
        } else {
            msg.clear();
        }
        return {code, std::move(msg)};
    }

private:
    vhdp_status_t code_ = VHDP_OK;
    std::string message_;
};

template <class T>
class Result {
public:
    Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}
    Result(Status status) : v_(std::in_place_index<1>, std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] T& value() & { return *std::get_if<0>(&v_); }
    [[nodiscard]] T&& value() && { return std::move(*std::get_if<0>(&v_)); }
    [[nodiscard]] const Status& status() const {
        static const Status ok_status;
        const Status* s = std::get_if<1>(&v_);
        return s != nullptr ? *s : ok_status;
    }

private:
    std::variant<T, Status> v_;
};

enum class Engine : vhdp_engine_kind_t {
    Auto = VHDP_ENGINE_AUTO,
    Rootless = VHDP_ENGINE_ROOTLESS,
    Rooted = VHDP_ENGINE_ROOTED,
    Emulator = VHDP_ENGINE_EMULATOR,
    Vm = VHDP_ENGINE_VM,
};

struct Event {
    vhdp_event_kind_t kind = 0;
    vhdp_severity_t severity = 0;
    std::uint64_t session_id = 0;
    std::int64_t pid = 0;
    std::uint64_t timestamp_ns = 0;
    std::int32_t code = 0;
    std::string_view name;        // valid only during the callback
    std::string_view message;     // valid only during the callback
    std::string_view detail_json; // valid only during the callback
};

struct ExitInfo {
    vhdp_session_state_t state = VHDP_STATE_CREATED;
    vhdp_exit_reason_t reason = VHDP_EXIT_NONE;
    std::int32_t exit_code = 0;
    std::int32_t term_signal = 0;
    std::int32_t shell_status = 0;
    vhdp_status_t status = VHDP_OK;
};

using EventCallback = std::function<void(const Event&)>;
using OutputCallback = std::function<void(vhdp_stream_t, const std::uint8_t*, std::size_t)>;

namespace detail {

struct ContextState {
    vhdp_context* handle = nullptr;
    EventCallback on_event;
    ~ContextState() {
        if (handle != nullptr) {
            // Sessions keep ContextState alive, so no session can still exist here.
            (void)vhdp_context_destroy(handle);
        }
    }
};

inline void event_trampoline(void* user, const vhdp_event* ev) {
    auto* state = static_cast<ContextState*>(user);
    if (state == nullptr || ev == nullptr || !state->on_event) {
        return;
    }
    Event e;
    e.kind = ev->kind;
    e.severity = ev->severity;
    e.session_id = ev->session_id;
    e.pid = ev->pid;
    e.timestamp_ns = ev->timestamp_ns;
    e.code = ev->code;
    e.name = ev->name != nullptr ? ev->name : "";
    e.message = ev->message != nullptr ? ev->message : "";
    e.detail_json = ev->detail_json != nullptr ? ev->detail_json : "{}";
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try {
        state->on_event(e);
    } catch (...) {
        std::fputs("vhdp.hpp: exception escaped event callback; ignored\n", stderr);
    }
#else
    state->on_event(e);
#endif
}

struct OutputState {
    OutputCallback on_output;
};

inline void output_trampoline(void* user, vhdp_stream_t stream, const std::uint8_t* data,
                              std::size_t len) {
    auto* state = static_cast<OutputState*>(user);
    if (state == nullptr || !state->on_output) {
        return;
    }
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try {
        state->on_output(stream, data, len);
    } catch (...) {
        std::fputs("vhdp.hpp: exception escaped output callback; ignored\n", stderr);
    }
#else
    state->on_output(stream, data, len);
#endif
}

template <class Fn>
Result<std::string> query_json(Fn&& fn) {
    std::string buf(16 * 1024, '\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        size_t needed = 0;
        vhdp_status_t r = fn(buf.data(), buf.size(), &needed);
        if (r == VHDP_OK) {
            buf.resize(needed > 0 ? needed - 1 : 0);
            return buf;
        }
        if (r != VHDP_E_BUFFER_TOO_SMALL) {
            return Status::from_c(r);
        }
        buf.assign(needed, '\0');
    }
    return Status(VHDP_E_INTERNAL, "JSON document size kept growing");
}

} // namespace detail

class Context {
public:
    static Result<Context> create(EventCallback on_event = {},
                                  vhdp_severity_t min_severity = VHDP_SEVERITY_INFO) {
        auto state = std::make_shared<detail::ContextState>();
        state->on_event = std::move(on_event);
        vhdp_context_options opts{};
        opts.struct_size = sizeof(opts);
        opts.abi_version = VHDP_ABI_VERSION;
        opts.min_severity = min_severity;
        if (state->on_event) {
            opts.event_fn = &detail::event_trampoline;
            opts.event_user = state.get();
        }
        vhdp_status_t r = vhdp_context_create(&opts, &state->handle);
        if (r != VHDP_OK) {
            return Status::from_c(r);
        }
        return Context(std::move(state));
    }

    [[nodiscard]] vhdp_context* get() const noexcept { return state_->handle; }

    [[nodiscard]] Result<std::string> capabilities_json() const {
        return detail::query_json([h = get()](char* b, size_t c, size_t* n) {
            return vhdp_capabilities_json(h, b, c, n);
        });
    }
    [[nodiscard]] Result<std::string> doctor_json(std::uint32_t flags = 0) const {
        bool first = true;
        return detail::query_json([h = get(), flags, first](char* b, size_t c, size_t* n) mutable {
            // Retries reuse the cached report so the probes run only once.
            std::uint32_t f = first ? flags : (flags & ~VHDP_DOCTOR_REFRESH);
            first = false;
            return vhdp_doctor_json(h, f, b, c, n);
        });
    }
    [[nodiscard]] Result<std::string> inspect_rootfs_json(const std::string& rootfs) const {
        return detail::query_json([h = get(), &rootfs](char* b, size_t c, size_t* n) {
            return vhdp_inspect_rootfs_json(h, rootfs.c_str(), b, c, n);
        });
    }

private:
    friend class Session;
    explicit Context(std::shared_ptr<detail::ContextState> s) : state_(std::move(s)) {}
    std::shared_ptr<detail::ContextState> state_;
};

class Config {
public:
    static Result<Config> create(const Context& ctx) {
        vhdp_config* h = nullptr;
        vhdp_status_t r = vhdp_config_create(ctx.get(), &h);
        if (r != VHDP_OK) {
            return Status::from_c(r);
        }
        return Config(h);
    }

    Config(Config&& o) noexcept : h_(std::exchange(o.h_, nullptr)), output_(std::move(o.output_)) {}
    Config& operator=(Config&& o) noexcept {
        if (this != &o) {
            vhdp_config_destroy(h_);
            h_ = std::exchange(o.h_, nullptr);
            output_ = std::move(o.output_);
        }
        return *this;
    }
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    ~Config() { vhdp_config_destroy(h_); }

    Status rootfs(const std::string& path) {
        return Status::from_c(vhdp_config_set_rootfs(h_, path.c_str()));
    }
    Status engine(Engine e) {
        return Status::from_c(vhdp_config_set_engine(h_, static_cast<vhdp_engine_kind_t>(e)));
    }
    Status cwd(const std::string& guest) {
        return Status::from_c(vhdp_config_set_cwd(h_, guest.c_str()));
    }
    Status argv(const std::vector<std::string>& args) {
        std::vector<const char*> ptrs;
        ptrs.reserve(args.size());
        for (const auto& a : args) {
            ptrs.push_back(a.c_str());
        }
        return Status::from_c(vhdp_config_set_argv(h_, ptrs.size(), ptrs.data()));
    }
    Status env(const std::string& key_value) {
        return Status::from_c(vhdp_config_add_env(h_, key_value.c_str()));
    }
    Status clear_env(bool on) {
        return Status::from_c(vhdp_config_set_clear_env(h_, on ? 1u : 0u));
    }
    Status bind(const std::string& host, const std::string& guest, bool read_write = false) {
        return Status::from_c(
            vhdp_config_add_bind(h_, host.c_str(), guest.c_str(),
                                 read_write ? VHDP_BIND_READ_WRITE : VHDP_BIND_READ_ONLY));
    }
    Status read_only_rootfs(bool on) {
        return Status::from_c(vhdp_config_set_read_only_rootfs(h_, on ? 1u : 0u));
    }
    Status network(vhdp_network_t n) { return Status::from_c(vhdp_config_set_network(h_, n)); }
    Status uid(std::uint32_t v) { return Status::from_c(vhdp_config_set_uid(h_, v)); }
    Status gid(std::uint32_t v) { return Status::from_c(vhdp_config_set_gid(h_, v)); }
    Status timeout_ms(std::uint64_t ms) {
        return Status::from_c(vhdp_config_set_timeout_ms(h_, ms));
    }
    Status trace(bool on) { return Status::from_c(vhdp_config_set_trace(h_, on ? 1u : 0u)); }
    Status stdio(vhdp_stdio_mode_t mode, int in = -1, int out = -1, int err = -1) {
        return Status::from_c(vhdp_config_set_stdio(h_, mode, in, out, err));
    }
    Status pty_size(std::uint16_t rows, std::uint16_t cols) {
        return Status::from_c(vhdp_config_set_pty_size(h_, rows, cols));
    }
    Status on_output(OutputCallback cb) {
        output_ = std::make_shared<detail::OutputState>();
        output_->on_output = std::move(cb);
        return Status::from_c(
            vhdp_config_set_output_callback(h_, &detail::output_trampoline, output_.get()));
    }
    Status seccomp(vhdp_seccomp_mode_t m) { return Status::from_c(vhdp_config_set_seccomp(h_, m)); }
    Status dev(vhdp_dev_mode_t m) { return Status::from_c(vhdp_config_set_dev(h_, m)); }
    Status proc(vhdp_proc_mode_t m) { return Status::from_c(vhdp_config_set_proc(h_, m)); }

    [[nodiscard]] const vhdp_config* get() const noexcept { return h_; }

private:
    friend class Session;
    explicit Config(vhdp_config* h) : h_(h) {}
    vhdp_config* h_ = nullptr;
    std::shared_ptr<detail::OutputState> output_;
};

class Session {
public:
    static Result<Session> create(const Context& ctx, const Config& cfg) {
        vhdp_session* h = nullptr;
        vhdp_status_t r = vhdp_session_create(ctx.get(), cfg.get(), &h);
        if (r != VHDP_OK) {
            return Status::from_c(r);
        }
        return Session(h, ctx.state_, cfg.output_);
    }

    Session(Session&& o) noexcept
        : h_(std::exchange(o.h_, nullptr)), ctx_(std::move(o.ctx_)), output_(std::move(o.output_)) {
    }
    Session& operator=(Session&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = std::exchange(o.h_, nullptr);
            ctx_ = std::move(o.ctx_);
            output_ = std::move(o.output_);
        }
        return *this;
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() { reset(); }

    Status start() { return Status::from_c(vhdp_session_start(h_)); }

    // timeout_ms < 0 waits forever; returns nullopt-like Status VHDP_E_TIMEOUT while running.
    Result<ExitInfo> wait(std::int64_t timeout_ms = -1) {
        vhdp_exit_info info{};
        info.struct_size = sizeof(info);
        info.abi_version = VHDP_ABI_VERSION;
        vhdp_status_t r = vhdp_session_wait(h_, timeout_ms, &info);
        if (r != VHDP_OK) {
            return Status::from_c(r);
        }
        ExitInfo e;
        e.state = info.state;
        e.reason = info.reason;
        e.exit_code = info.exit_code;
        e.term_signal = info.term_signal;
        e.shell_status = info.shell_status;
        e.status = info.status;
        return e;
    }
    Status signal(int signo) { return Status::from_c(vhdp_session_signal(h_, signo)); }
    Status resize(std::uint16_t rows, std::uint16_t cols) {
        return Status::from_c(vhdp_session_resize(h_, rows, cols));
    }
    Status write_input(std::string_view data) {
        size_t written = 0;
        return Status::from_c(vhdp_session_write_input(
            h_, reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), &written));
    }
    Status cancel() { return Status::from_c(vhdp_session_cancel(h_)); }
    [[nodiscard]] std::optional<int> pty_master() const {
        std::int32_t fd = -1;
        if (vhdp_session_pty_master(h_, &fd) != VHDP_OK) {
            return std::nullopt;
        }
        return fd;
    }
    [[nodiscard]] vhdp_session_state_t state() const {
        vhdp_session_state_t s = VHDP_STATE_FAILED;
        (void)vhdp_session_state(h_, &s);
        return s;
    }
    [[nodiscard]] vhdp_session* get() const noexcept { return h_; }

private:
    Session(vhdp_session* h, std::shared_ptr<detail::ContextState> ctx,
            std::shared_ptr<detail::OutputState> out)
        : h_(h), ctx_(std::move(ctx)), output_(std::move(out)) {}
    void reset() noexcept {
        if (h_ != nullptr) {
            (void)vhdp_session_destroy(h_);
            h_ = nullptr;
        }
        output_.reset();
        ctx_.reset();
    }
    vhdp_session* h_ = nullptr;
    std::shared_ptr<detail::ContextState> ctx_;   // context outlives the session
    std::shared_ptr<detail::OutputState> output_; // output callback outlives the session
};

} // namespace vhdp

#endif // VHDP_VHDP_HPP
