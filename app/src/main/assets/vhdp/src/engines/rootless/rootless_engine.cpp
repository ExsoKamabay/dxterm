// Rootless engine: ptrace supervisor with optional seccomp acceleration.
#include "core/engine.hpp"
#include "engines/rootless/supervisor.hpp"
#include "platform/linux/host_probe.hpp"
#include "vdr/drivers.hpp"

#include <climits>
#include <csignal>
#include <cstdlib>
#include <mutex>

namespace vhdp::rootless {

namespace {

class RootlessInstance final : public core::EngineInstance {
public:
    RootlessInstance(std::shared_ptr<const core::ValidatedConfig> cfg, core::EventSink& sink,
                     std::uint64_t id, SupervisorOptions opts)
        : cfg_(cfg), id_(id), sup_(std::move(cfg), sink, id, opts) {}

    Status start() override {
        struct sigaction sa{};
        if (::sigaction(SIGCHLD, nullptr, &sa) == 0 &&
            (sa.sa_handler == SIG_IGN || (sa.sa_flags & SA_NOCLDWAIT) != 0)) {
            return {Code::host_environment,
                    "SIGCHLD is ignored (SIG_IGN or SA_NOCLDWAIT) in this process; the supervisor "
                    "cannot wait for guest processes"};
        }
        const core::SessionConfig& c = cfg_->cfg;
        if (c.stdio == core::StdioMode::pty) {
            auto pty = vdr::PtyDriver::open(c.pty_rows, c.pty_cols);
            if (!pty.is_ok()) {
                return std::move(pty).take_status();
            }
            pty_ = std::move(pty.value());
        }
        Status s = sup_.start(pty_.get());
        if (s.is_ok() && pty_ && c.output_fn != nullptr) {
            if (Status ps = pty_->start_pump(c.output_fn, c.output_user, id_); !ps.is_ok()) {
                (void)sup_.cancel(core::ExitReason::cancelled);
                return ps;
            }
        }
        return s;
    }

    core::ExitResult wait() override {
        core::ExitResult r = sup_.wait();
        if (pty_) {
            pty_->finish_pump();
        }
        return r;
    }

    Status signal(int signo) override { return sup_.signal_main(signo); }

    Status resize(std::uint16_t rows, std::uint16_t cols) override {
        if (!pty_) {
            return {Code::invalid_state, "resize requires VHDP_STDIO_PTY; in inherit mode the "
                                         "terminal delivers SIGWINCH itself"};
        }
        return pty_->resize(rows, cols);
    }

    Status cancel(core::ExitReason reason) override { return sup_.cancel(reason); }

    int pty_master() const noexcept override { return pty_ ? pty_->master() : -1; }

    Status write_input(const std::uint8_t* data, std::size_t len, std::size_t& written) override {
        written = 0;
        if (!pty_) {
            return {Code::invalid_state, "write_input requires VHDP_STDIO_PTY"};
        }
        return pty_->write_input(data, len, written);
    }

private:
    std::shared_ptr<const core::ValidatedConfig> cfg_;
    std::uint64_t id_;
    std::unique_ptr<vdr::PtyDriver> pty_;
    Supervisor sup_; // declared last: destroyed (cancelled + joined) before the PTY
};

class RootlessEngine final : public core::Engine {
public:
    core::EngineKind kind() const noexcept override { return core::EngineKind::rootless; }

    core::ProbeReport probe() const override {
        std::call_once(once_, [this] { compute(); });
        return report_;
    }

    Result<std::unique_ptr<core::EngineInstance>>
    create_instance(std::shared_ptr<const core::ValidatedConfig> cfg, core::EventSink& sink,
                    std::uint64_t session_id) const override {
        core::ProbeReport p = probe();
        if (!p.available) {
            return Status{Code::engine_unavailable, "rootless engine unavailable: " + p.reason};
        }
        SupervisorOptions o;
        o.get_syscall_info = syscall_info_;
        o.loader_path = loader_path_;
        if (!loader_path_.empty() && platform::is_android_host()) {
            // The loader maps guest programs PROT_EXEC from the rootfs. Check that permission
            // where it will actually be used, so a refusal is a clear start failure rather than
            // every guest exec dying inside the loader.
            platform::ExecMappingProbe xm = platform::probe_exec_mapping(cfg->cfg.rootfs);
            if (xm.probed && !xm.allowed) {
                return Status{Code::engine_unavailable,
                              "rootless engine unavailable: the userland loader maps guest programs "
                              "PROT_EXEC, but " + xm.detail};
            }
        }
        switch (cfg->cfg.seccomp) {
            case core::SeccompMode::on:
                if (!seccomp_) {
                    return Status{Code::engine_unavailable,
                                  "--seccomp on requested but seccomp is unavailable: " +
                                      seccomp_detail_};
                }
                o.use_seccomp = true;
                break;
            case core::SeccompMode::off:
                o.use_seccomp = false;
                break;
            case core::SeccompMode::auto_select:
                o.use_seccomp = seccomp_;
                break;
        }
        try {
            std::unique_ptr<core::EngineInstance> inst =
                std::make_unique<RootlessInstance>(std::move(cfg), sink, session_id, o);
            return inst;
        } catch (...) {
            return Status{Code::no_memory, "cannot allocate the rootless engine instance"};
        }
    }

private:
    void compute() const {
#if !defined(__x86_64__) && !defined(__aarch64__)
        report_.available = false;
        report_.reason = "unsupported host architecture for the rootless engine";
        return;
#else
        bool explicit_loader = false;
        std::string loader = platform::find_userland_loader(&explicit_loader);
        if (!loader.empty()) {
            // /proc/<pid>/exe reports the canonical path; compare against the same form.
            if (char* real = ::realpath(loader.c_str(), nullptr); real != nullptr) {
                loader = real;
                std::free(real);
            }
        }
        if (platform::android_app_context()) {
            // An app targeting API 29+ may not execve() anything from its writable storage,
            // where every guest program lives. The userland loader, executed from
            // nativeLibraryDir, maps guest programs instead; without it there is no way in.
            report_.details.push_back("SELinux context: " + platform::selinux_context());
            if (loader.empty()) {
                report_.available = false;
                report_.reason =
                    std::string("android-app profile: execve() from writable app storage is "
                                "blocked for targetSdk >= 29, and the userland loader ") +
                    platform::kUserlandLoaderName +
                    (explicit_loader ? " named by VHDP_LOADER is not an executable file"
                                     : " is not installed next to libvhdp");
                return;
            }
            loader_path_ = loader;
        } else if (explicit_loader) {
            if (loader.empty()) {
                report_.available = false;
                report_.reason = "VHDP_LOADER is set but does not name an executable file";
                return;
            }
            loader_path_ = loader;
        } else if (!loader.empty() && platform::is_android_host()) {
            // Installed with the library on an Android host whose policy does not require it (an
            // emulator or container without the app domain): used anyway, so every Android host
            // runs guests the same way, with /proc/self/exe naming the guest program.
            loader_path_ = loader;
        }
        platform::PtraceProbe pt = platform::probe_ptrace_child();
        report_.details.push_back(pt.detail);
        if (!pt.ok) {
            report_.available = false;
            report_.reason = pt.detail;
            if (auto scope = platform::yama_ptrace_scope(); scope && *scope >= 2) {
                report_.reason +=
                    " (Yama ptrace_scope=" + std::to_string(*scope) + " forbids tracing)";
            }
            return;
        }
        platform::SeccompProbe sc = platform::probe_seccomp_filter();
        seccomp_ = sc.ok;
        seccomp_detail_ = sc.detail;
        syscall_info_ = pt.get_syscall_info;
        report_.details.push_back(sc.detail);
        report_.available = true;
        report_.reason = std::string("child ptrace permitted; seccomp acceleration ") +
                         (seccomp_ ? "available" : "unavailable") + "; PTRACE_GET_SYSCALL_INFO " +
                         (syscall_info_ ? "available" : "unavailable");
        if (!loader_path_.empty()) {
            report_.reason += "; guest programs start through the userland loader " + loader_path_;
            report_.details.push_back("userland loader: " + loader_path_);
        }
#endif
    }

    mutable std::once_flag once_;
    mutable core::ProbeReport report_;
    mutable bool seccomp_ = false;
    mutable bool syscall_info_ = false;
    mutable std::string seccomp_detail_;
    mutable std::string loader_path_;
};

} // namespace
} // namespace vhdp::rootless

namespace vhdp::core {

std::unique_ptr<Engine> make_rootless_engine() {
    return std::make_unique<rootless::RootlessEngine>();
}

} // namespace vhdp::core
