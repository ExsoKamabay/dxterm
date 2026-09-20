// `phdp run`: builds a libvhdp session from CLI options and relays terminal
// signals and I/O.
//
// Inherit mode (default): the guest shares phdp's terminal and process group, so
// terminal-generated SIGINT/SIGQUIT/SIGTSTP and SIGWINCH reach the guest directly;
// phdp only relays signals sent to phdp itself (SIGTERM, SIGHUP, SIGUSR1/2 and
// user-sent SIGINT/SIGQUIT).
// PTY mode (--pty): the guest gets a new PTY; phdp switches its own terminal to
// raw mode, forwards stdin, writes guest output to stdout and forwards SIGWINCH as
// a PTY resize.
#include "cli.hpp"
#include "json_dom.hpp"

#include "vhdp/vhdp.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>

namespace phdp {

namespace {

class EventPrinter {
public:
    EventPrinter(const RunOptions& o)
        : json_(o.json), verbose_(o.verbose), trace_(o.trace), fd_(o.event_fd.value_or(2)) {}

    void on_event(const vhdp::Event& e) {
        std::string line;
        if (json_) {
            static constexpr const char* kinds[] = {"", "lifecycle", "diagnostic", "log", "trace"};
            static constexpr const char* sev[] = {"debug", "info", "warning", "error"};
            line = "{\"type\":" + json::quote(e.kind < 5 ? kinds[e.kind] : "unknown") +
                   ",\"severity\":" + json::quote(e.severity < 4 ? sev[e.severity] : "unknown") +
                   ",\"name\":" + json::quote(e.name) +
                   ",\"ts_ns\":" + std::to_string(e.timestamp_ns) +
                   ",\"session\":" + std::to_string(e.session_id) +
                   ",\"pid\":" + std::to_string(e.pid) + ",\"code\":" + std::to_string(e.code) +
                   ",\"message\":" + json::quote(e.message) +
                   ",\"detail\":" + std::string(e.detail_json.empty() ? "{}" : e.detail_json) +
                   "}\n";
        } else {
            if (e.kind == VHDP_EVENT_TRACE) {
                if (!trace_) {
                    return;
                }
                line = "phdp: trace: " + std::string(e.message) + " " + std::string(e.detail_json) +
                       "\n";
            } else if (e.kind == VHDP_EVENT_LIFECYCLE && e.severity >= VHDP_SEVERITY_ERROR) {
                return; // start failures are printed once from the returned status
            } else if (e.severity >= VHDP_SEVERITY_WARNING) {
                line = std::string("phdp: ") +
                       (e.severity >= VHDP_SEVERITY_ERROR ? "error: " : "warning: ") +
                       std::string(e.message) + "\n";
            } else if (verbose_ || trace_) {
                line = "phdp: [" + std::string(e.name) + "] " + std::string(e.message) + "\n";
            } else {
                return;
            }
        }
        std::lock_guard<std::mutex> lock(mu_);
        (void)write_all(fd_, line.data(), line.size());
    }

private:
    bool json_;
    bool verbose_;
    bool trace_;
    int fd_;
    std::mutex mu_;
};

sigset_t relay_set() {
    sigset_t s;
    sigemptyset(&s);
    for (int sig : {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2, SIGWINCH}) {
        sigaddset(&s, sig);
    }
    return s;
}

bool terminal_size(int fd, std::uint16_t& rows, std::uint16_t& cols) {
    winsize ws{};
    if (::ioctl(fd, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 || ws.ws_col == 0) {
        return false;
    }
    rows = ws.ws_row;
    cols = ws.ws_col;
    return true;
}

// Blocks relayed signals in the calling (main) thread before any library thread
// exists, then consumes them with sigtimedwait on a dedicated thread.
class SignalRelay {
public:
    explicit SignalRelay(bool pty) : pty_(pty) {
        sigset_t set = relay_set();
        pthread_sigmask(SIG_BLOCK, &set, &old_mask_);
        struct sigaction ign{};
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        sigaction(SIGPIPE, &ign, &old_pipe_);
    }

    ~SignalRelay() {
        stop();
        pthread_sigmask(SIG_SETMASK, &old_mask_, nullptr);
        sigaction(SIGPIPE, &old_pipe_, nullptr);
    }

    void start(vhdp::Session* session) {
        thread_ = std::thread([this, session] { loop(session); });
    }

    void stop() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void loop(vhdp::Session* session) {
        sigset_t set = relay_set();
        timespec tick{0, 100L * 1000 * 1000};
        while (!stop_.load()) {
            siginfo_t info{};
            int sig = sigtimedwait(&set, &info, &tick);
            if (sig < 0) {
                continue;
            }
            if (sig == SIGWINCH) {
                std::uint16_t rows = 0;
                std::uint16_t cols = 0;
                if (pty_ && terminal_size(STDIN_FILENO, rows, cols)) {
                    (void)session->resize(rows, cols);
                }
                continue;
            }
            if ((sig == SIGINT || sig == SIGQUIT) && !pty_ && info.si_code == SI_KERNEL) {
                continue; // generated by the terminal: the guest received it already
            }
            (void)session->signal(sig);
        }
    }

    bool pty_;
    sigset_t old_mask_{};
    struct sigaction old_pipe_{};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

class RawTerminal {
public:
    explicit RawTerminal(bool enable) {
        if (enable && ::isatty(STDIN_FILENO) == 1 && ::tcgetattr(STDIN_FILENO, &saved_) == 0) {
            termios raw = saved_;
            ::cfmakeraw(&raw);
            active_ = ::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
        }
    }
    ~RawTerminal() { restore(); }
    void restore() {
        if (active_) {
            (void)::tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
            active_ = false;
        }
    }

private:
    termios saved_{};
    bool active_ = false;
};

class InputPump {
public:
    explicit InputPump(vhdp::Session* session) : session_(session) {
        if (session_ == nullptr) {
            return;
        }
        wake_ = ::eventfd(0, EFD_CLOEXEC);
        if (wake_ >= 0) {
            thread_ = std::thread([this] { loop(); });
        }
    }
    ~InputPump() {
        if (thread_.joinable()) {
            std::uint64_t one = 1;
            ssize_t wr = ::write(wake_, &one, sizeof(one));
            (void)wr;
            thread_.join();
        }
        if (wake_ >= 0) {
            ::close(wake_);
        }
    }

private:
    void loop() {
        char buf[4096];
        bool eof = false;
        for (;;) {
            pollfd fds[2] = {{eof ? -1 : STDIN_FILENO, POLLIN, 0}, {wake_, POLLIN, 0}};
            if (::poll(fds, 2, -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            if ((fds[1].revents & POLLIN) != 0) {
                return;
            }
            if ((fds[0].revents & (POLLIN | POLLHUP)) != 0) {
                ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
                if (n > 0) {
                    (void)session_->write_input(std::string_view(buf, static_cast<std::size_t>(n)));
                } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                    // End of non-terminal input: send the PTY end-of-file character.
                    (void)session_->write_input("\x04");
                    eof = true;
                }
            }
        }
    }

    vhdp::Session* session_;
    int wake_ = -1;
    std::thread thread_;
};

int fail(const std::string& what, const vhdp::Status& s) {
    std::fprintf(stderr, "phdp: error: %s: %s\n", what.c_str(),
                 s.message().empty() ? s.name() : s.message().c_str());
    return kExitStartFailure;
}

} // namespace

// Reads TracerPid from /proc/self/status: the PID of the process ptracing us, or 0 if none.
// Deliberately a local copy of what platform::tracer_pid() does: the CLI target links only the
// public headers, and one 10-line reader is a smaller price than widening that boundary.
long tracer_pid() {
    long tracer = 0;
    if (FILE* st = std::fopen("/proc/self/status", "re")) {
        char line[256];
        while (std::fgets(line, sizeof(line), st) != nullptr) {
            if (std::sscanf(line, "TracerPid: %ld", &tracer) == 1) {
                break;
            }
        }
        std::fclose(st);
    }
    return tracer;
}

int run_command(const RunOptions& o) {
    if (o.event_fd && ::fcntl(*o.event_fd, F_GETFD) < 0) {
        std::fprintf(stderr, "phdp: error: --event-fd %d is not an open descriptor\n", *o.event_fd);
        return kExitUsage;
    }

    // Preflight for the ptrace-based engines. The rootless engine drives the guest with ptrace,
    // and Linux allows only ONE tracer per process. When phdp is itself already being traced --
    // most often because it runs inside a shell that is already a rootless session (its shell
    // runs under this engine's supervisor), or under a debugger or syscall tracer --
    // PTRACE_SEIZE of the guest fails deep in the engine with a raw "ESRCH/EPERM". Detect it up
    // front and explain it, so `phdp run` fails clearly and SAFELY in a nested session instead
    // of surfacing a cryptic ptrace error. Only the ptrace engines (auto/rootless) are gated;
    // doctor/inspect/capabilities and a normal, non-traced shell (TracerPid 0, e.g. adb shell)
    // are unaffected.
    if (o.engine == "auto" || o.engine == "rootless") {
        long tracer = tracer_pid();
        if (tracer > 0) {
            std::fprintf(stderr,
                         "phdp: error: cannot run the rootless engine here: this process is already "
                         "traced by PID %ld (a ptrace supervisor: an enclosing vhdp session, a "
                         "debugger or a syscall tracer).\nLinux allows only one tracer per "
                         "process, so vhdp cannot ptrace a guest from inside it. Run `phdp run` "
                         "from a shell that is NOT supervised (e.g. adb shell), or just use the "
                         "surrounding session, which is already a Linux environment. `phdp "
                         "doctor`, `inspect` and `capabilities` work here normally.\n",
                         tracer);
            return kExitStartFailure;
        }
    }

    SignalRelay relay(o.pty);
    EventPrinter printer(o);
    vhdp_severity_t min = (o.trace || o.verbose)
                              ? VHDP_SEVERITY_DEBUG
                              : (o.json ? VHDP_SEVERITY_INFO : VHDP_SEVERITY_WARNING);
    auto ctx =
        vhdp::Context::create([&printer](const vhdp::Event& e) { printer.on_event(e); }, min);
    if (!ctx.ok()) {
        return fail("cannot create context", ctx.status());
    }
    auto cfg_r = vhdp::Config::create(ctx.value());
    if (!cfg_r.ok()) {
        return fail("cannot create config", cfg_r.status());
    }
    vhdp::Config& cfg = cfg_r.value();

    auto check = [&](const vhdp::Status& s, const char* what) {
        if (!s.ok()) {
            std::fprintf(stderr, "phdp: error: %s: %s\n", what, s.message().c_str());
            return false;
        }
        return true;
    };
    vhdp::Engine engine = vhdp::Engine::Auto;
    if (o.engine == "rootless") {
        engine = vhdp::Engine::Rootless;
    } else if (o.engine == "rooted") {
        engine = vhdp::Engine::Rooted;
    } else if (o.engine == "emulator") {
        engine = vhdp::Engine::Emulator;
    } else if (o.engine == "vm") {
        engine = vhdp::Engine::Vm;
    }
    bool ok = check(cfg.rootfs(o.rootfs), "rootfs") && check(cfg.engine(engine), "--engine") &&
              check(cfg.argv(o.command), "command") &&
              check(cfg.clear_env(o.clear_env), "--clear-env") &&
              check(cfg.read_only_rootfs(o.read_only_rootfs), "--read-only-rootfs") &&
              check(cfg.network(o.network == "none" ? VHDP_NETWORK_NONE : VHDP_NETWORK_HOST),
                    "--network") &&
              check(cfg.trace(o.trace), "--trace") &&
              check(cfg.seccomp(o.seccomp == "on"
                                    ? VHDP_SECCOMP_ON
                                    : (o.seccomp == "off" ? VHDP_SECCOMP_OFF : VHDP_SECCOMP_AUTO)),
                    "--seccomp") &&
              check(cfg.dev(o.dev == "none" ? VHDP_DEV_NONE : VHDP_DEV_MINIMAL), "--dev") &&
              check(cfg.proc(o.proc == "host" ? VHDP_PROC_HOST : VHDP_PROC_NONE), "--proc");
    if (ok && o.cwd) {
        ok = check(cfg.cwd(*o.cwd), "--cwd");
    }
    for (std::size_t i = 0; ok && i < o.env.size(); ++i) {
        ok = check(cfg.env(o.env[i]), "--env");
    }
    for (std::size_t i = 0; ok && i < o.binds.size(); ++i) {
        ok = check(cfg.bind(o.binds[i].host, o.binds[i].guest, o.binds[i].read_write), "--bind");
    }
    if (ok && o.uid) {
        ok = check(cfg.uid(*o.uid), "--uid");
    }
    if (ok && o.gid) {
        ok = check(cfg.gid(*o.gid), "--gid");
    }
    if (ok && o.timeout_ms) {
        ok = check(cfg.timeout_ms(*o.timeout_ms), "--timeout");
    }
    if (ok && o.pty) {
        std::uint16_t rows = 24;
        std::uint16_t cols = 80;
        (void)terminal_size(STDIN_FILENO, rows, cols);
        ok = check(cfg.stdio(VHDP_STDIO_PTY), "--pty") &&
             check(cfg.pty_size(rows, cols), "--pty") &&
             check(cfg.on_output([](vhdp_stream_t, const std::uint8_t* data, std::size_t len) {
                 (void)write_all(STDOUT_FILENO, reinterpret_cast<const char*>(data), len);
             }),
                   "--pty");
    }
    if (!ok) {
        return kExitStartFailure;
    }

    auto session_r = vhdp::Session::create(ctx.value(), cfg);
    if (!session_r.ok()) {
        return fail("invalid session configuration", session_r.status());
    }
    vhdp::Session& session = session_r.value();

    RawTerminal raw(o.pty);
    vhdp::Status started = session.start();
    if (!started.ok()) {
        raw.restore();
        auto info = session.wait(0);
        int code = info.ok() ? info.value().shell_status : kExitStartFailure;
        std::fprintf(stderr, "phdp: error: %s\n", started.message().c_str());
        return code == 0 ? kExitStartFailure : code;
    }
    relay.start(&session);
    int status = kExitStartFailure;
    {
        InputPump pump(o.pty ? &session : nullptr);
        auto result = session.wait(-1);
        if (result.ok()) {
            status = result.value().shell_status;
        } else {
            std::fprintf(stderr, "phdp: error: wait failed: %s\n",
                         result.status().message().c_str());
        }
    }
    relay.stop();
    raw.restore();
    return status;
}

} // namespace phdp
