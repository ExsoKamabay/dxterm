#include "engines/rootless/supervisor.hpp"

#include "common/json.hpp"
#include "common/unique_fd.hpp"
#include "engines/rootless/bootstrap.h"
#include "engines/rootless/loader/load_request.h"
#include "engines/rootless/seccomp_filter.hpp"
#include "engines/rootless/tracee_mem.hpp"
#include "linux_abi/errno_table.h"
#include "platform/linux/host_probe.hpp"
#include "platform/linux/proc.hpp"
#include "platform/linux/ptrace_defs.hpp"
#include "vdr/drivers.hpp"
#include "vfs/guest_path.hpp"

#include <asm/unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <linux/filter.h>
#include <linux/netlink.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>

namespace vhdp::rootless {

using core::EventKind;
using core::ExitReason;
using core::Severity;
using platform::ptrace_call;

namespace {

constexpr std::uint64_t kMaxStackScratch = std::uint64_t{1024} * 1024;

std::string errno_text(int e) {
    return std::string(vhdp_errno_name(e)) + " (" + vhdp_errno_description(e) + ")";
}

const char* stage_name(int stage) {
    switch (stage) {
        case VHDP_BOOT_STAGE_SIGNALS:
            return "signal reset";
        case VHDP_BOOT_STAGE_SESSION:
            return "session/controlling terminal";
        case VHDP_BOOT_STAGE_STDIO:
            return "stdio setup";
        case VHDP_BOOT_STAGE_SYNC:
            return "tracer synchronisation";
        case VHDP_BOOT_STAGE_CHDIR:
            return "chdir";
        case VHDP_BOOT_STAGE_FDS:
            return "close-on-exec";
        case VHDP_BOOT_STAGE_NO_NEW_PRIVS:
            return "no_new_privs";
        case VHDP_BOOT_STAGE_SECCOMP:
            return "seccomp filter";
        case VHDP_BOOT_STAGE_EXEC:
            return "execve";
        default:
            return "unknown";
    }
}

} // namespace

StackWriter::StackWriter(pid_t tid, std::uint64_t sp) noexcept
    : tid_(tid), base_(sp),
      top_((sp - arch::stack_red_zone() - 64) & ~(arch::stack_alignment() - 1)) {}

int StackWriter::push(const void* data, std::size_t len, std::uint64_t& addr) noexcept {
    if (len > kMaxStackScratch || top_ < len + 4096) {
        return ENOMEM;
    }
    std::uint64_t next = (top_ - len) & ~(arch::stack_alignment() - 1);
    if (base_ - next > kMaxStackScratch) {
        return ENOMEM;
    }
    if (int e = write_mem(tid_, next, data, len); e != 0) {
        return e == ESRCH ? ESRCH : ENOMEM;
    }
    top_ = next;
    addr = next;
    return 0;
}

int StackWriter::push_string(const std::string& s, std::uint64_t& addr) noexcept {
    return push(s.c_str(), s.size() + 1, addr);
}

std::string Supervisor::make_load_request(const ExecPlan& plan, const std::string& execfn) {
    vhdp_load_request hdr{};
    std::string body;
    auto add = [&](const std::string& s) -> std::uint32_t {
        auto off = static_cast<std::uint32_t>(sizeof(hdr) + body.size());
        body.append(s);
        body.push_back('\0');
        return off;
    };
    // Task name, as the kernel derives it: the last component of the path given to execve.
    auto parts = vfs::components(execfn);
    std::string name = parts.empty() ? execfn : std::string(parts.back());
    hdr.program_off = add(plan.guest_program);
    hdr.interp_off = plan.via_loader ? add(plan.guest_loader) : 0;
    hdr.execfn_off = add(execfn);
    hdr.name_off = add(name);
    hdr.magic = VHDP_LOAD_MAGIC;
    hdr.version = VHDP_LOAD_VERSION;
    hdr.total_size = static_cast<std::uint32_t>(sizeof(hdr) + body.size());
    hdr.flags = 0;
    if (hdr.total_size > VHDP_LOAD_MAX_SIZE) {
        return {};
    }
    std::string out(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.append(body);
    return out;
}

Creds& Supervisor::creds_of(Tracee& t) {
    pid_t tg = tgid_of(t);
    auto it = creds_.find(tg);
    if (it != creds_.end()) {
        return it->second;
    }
    const core::SessionConfig& c = cfg_->cfg;
    Creds cr;
    cr.ruid = cr.euid = cr.suid = cr.fsuid = c.uid.value_or(host_uid_);
    cr.rgid = cr.egid = cr.sgid = cr.fsgid = c.gid.value_or(host_gid_);
    cr.groups.push_back(cr.rgid);
    return creds_.emplace(tg, std::move(cr)).first->second;
}

void Supervisor::inherit_process_state(Tracee& parent, pid_t child) {
    pid_t parent_tg = tgid_of(parent);
    Tracee& ct = get_or_add(child);
    pid_t child_tg = tgid_of(ct);
    if (child_tg == parent_tg) {
        return; // a thread: it shares the process's program and credentials
    }
    if (auto it = exe_of_.find(parent_tg); it != exe_of_.end()) {
        exe_of_[child_tg] = it->second;
    }
    if (auto it = creds_.find(parent_tg); it != creds_.end()) {
        creds_[child_tg] = it->second;
    }
    if (guest_filtered_.count(parent_tg) != 0) {
        guest_filtered_.insert(child_tg); // seccomp filters are inherited by children
    }
}

void Supervisor::inject_load_request(pid_t pid, const std::string& request,
                                     const std::string& exe) {
    // The tracee is at its PTRACE_EVENT_EXEC stop: the loader image is in place, sp points
    // at argc, and nothing of the loader has run. The request goes below the kernel-built
    // stack (the initial stack mapping extends ~128 KiB below sp), and its address and size
    // go to the registers load_request.h names.
    arch::RegsAccess regs(pid);
    int e = regs.fetch();
    if (e == 0) {
        std::uint64_t addr = (regs.view().sp - arch::stack_red_zone() - 256 - request.size()) &
                             ~(arch::stack_alignment() - 1);
        e = write_mem(pid, addr, request.data(), request.size());
        if (e == 0) {
            regs.set_arg(1, addr);
            regs.set_arg(2, request.size());
            e = regs.commit();
        }
    }
    if (e != 0) {
        emit(EventKind::diagnostic, Severity::error, "exec.loader_handover_failed",
             "cannot hand the load request to the userland loader: " + errno_text(e), pid, e);
        ::kill(pid, SIGKILL);
        return;
    }
    exe_of_[pid] = exe;
}

Supervisor::Supervisor(std::shared_ptr<const core::ValidatedConfig> cfg, core::EventSink& sink,
                       std::uint64_t session_id, SupervisorOptions opts)
    : cfg_(std::move(cfg)), sink_(sink), session_id_(session_id), opts_(opts),
      table_(vdr::build_mount_table(*cfg_)), exec_(table_), host_uid_(::geteuid()),
      host_gid_(::getegid()) {}

Supervisor::~Supervisor() {
    if (thread_.joinable()) {
        (void)cancel(ExitReason::cancelled);
        thread_.join();
    }
    remove_proc_scratch();
    if (main_pidfd_ >= 0) {
        ::close(main_pidfd_);
    }
    if (err_pipe_rd_ >= 0) {
        ::close(err_pipe_rd_);
    }
}

void Supervisor::emit(EventKind kind, Severity sev, std::string name, std::string message,
                      pid_t pid, int code, std::string detail) {
    if (!sink_.wants(sev)) {
        return;
    }
    core::Event ev;
    ev.kind = kind;
    ev.severity = sev;
    ev.session_id = session_id_;
    ev.pid = pid;
    ev.timestamp_ns = core::monotonic_ns();
    ev.code = code;
    ev.name = std::move(name);
    ev.message = std::move(message);
    ev.detail_json = std::move(detail);
    sink_.emit(std::move(ev));
}

Status Supervisor::start(vdr::PtyDriver* pty) {
    pty_ = pty;
    try {
        thread_ = std::thread([this] { thread_main(); });
    } catch (...) {
        return {Code::no_memory, "cannot create the supervisor thread"};
    }
    std::unique_lock<std::mutex> lock(state_mu_);
    state_cv_.wait(lock, [this] { return start_state_ != StartState::pending; });
    return start_state_ == StartState::ok ? Status::ok() : start_status_;
}

core::ExitResult Supervisor::wait() {
    if (!thread_.joinable()) {
        std::lock_guard<std::mutex> lock(state_mu_);
        if (!done_) {
            core::ExitResult never_started;
            never_started.reason = ExitReason::start_failed;
            never_started.status = {Code::invalid_state, "supervisor was never started"};
            return never_started;
        }
        return result_;
    }
    {
        std::unique_lock<std::mutex> lock(state_mu_);
        state_cv_.wait(lock, [this] { return done_; });
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(state_mu_);
    return result_;
}

Status Supervisor::signal_main(int signo) {
    if (signo < 0 || signo > 64) {
        return {Code::invalid_argument, "invalid signal number"};
    }
    std::lock_guard<std::mutex> lock(known_mu_);
    if (main_pid_ <= 0 || main_reaped_) {
        return {Code::invalid_state, "guest process is not running"};
    }
#ifdef SYS_pidfd_send_signal
    if (main_pidfd_ >= 0) {
        if (::syscall(SYS_pidfd_send_signal, main_pidfd_, signo, nullptr, 0) == 0) {
            return Status::ok();
        }
        if (errno != ENOSYS) {
            return errno_status(Code::io, "pidfd_send_signal", errno);
        }
    }
#endif
    if (::kill(main_pid_, signo) != 0) {
        return errno_status(Code::io, "kill", errno);
    }
    return Status::ok();
}

Status Supervisor::cancel(ExitReason reason) {
    std::uint32_t expected = 0;
    cancel_reason_.compare_exchange_strong(expected, static_cast<std::uint32_t>(reason));
    std::lock_guard<std::mutex> lock(known_mu_);
    // Pids stay in known_ until the supervisor observed their exit, so a pid in
    // this set still names a tracee (see SECURITY.md for the residual reuse window
    // of non-child tracees that were reaped by their real parent).
    for (pid_t p : known_) {
        ::kill(p, SIGKILL);
    }
    return Status::ok();
}

void Supervisor::thread_main() noexcept {
    try {
        run();
    } catch (...) {
        kill_all();
        core::ExitResult r;
        r.reason = ExitReason::start_failed;
        r.status = {Code::internal, "unexpected exception in the rootless supervisor"};
        finish_start(r.status, 0);
        finish(std::move(r));
    }
}

void Supervisor::finish_start(Status s, int exec_errno) {
    std::lock_guard<std::mutex> lock(state_mu_);
    if (start_state_ != StartState::pending) {
        return;
    }
    start_state_ = s.is_ok() ? StartState::ok : StartState::failed;
    start_status_ = std::move(s);
    result_.exec_errno = exec_errno;
    state_cv_.notify_all();
}

void Supervisor::finish(core::ExitResult r) {
    std::lock_guard<std::mutex> lock(state_mu_);
    if (done_) {
        return;
    }
    if (r.exec_errno == 0) {
        r.exec_errno = result_.exec_errno;
    }
    result_ = std::move(r);
    done_ = true;
    state_cv_.notify_all();
}

void Supervisor::run() {
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, nullptr);

    if (!prepare_and_spawn(pty_)) {
        return;
    }
    event_loop();

    core::ExitResult r;
    auto cancel_reason = static_cast<ExitReason>(cancel_reason_.load());
    if (start_state_ != StartState::ok) {
        // The initial process died before its exec completed.
        int32_t msg[2] = {0, 0};
        ssize_t n = err_pipe_rd_ >= 0 ? ::read(err_pipe_rd_, msg, sizeof(msg)) : -1;
        Status s;
        int exec_errno = 0;
        if (n == static_cast<ssize_t>(sizeof(msg))) {
            exec_errno = msg[1];
            s = {msg[0] == VHDP_BOOT_STAGE_EXEC ? Code::exec_failed : Code::internal,
                 std::string("guest bootstrap failed at ") + stage_name(msg[0]) + ": " +
                     errno_text(msg[1])};
        } else if (cancel_reason != ExitReason::none) {
            s = {Code::cancelled, "session cancelled during start"};
        } else {
            s = {Code::exec_failed, "guest process exited before exec completed"};
        }
        emit(EventKind::lifecycle, Severity::error, "session.start_failed", s.message(), main_pid_,
             exec_errno);
        finish_start(s, exec_errno);
        r.reason = cancel_reason != ExitReason::none ? cancel_reason : ExitReason::start_failed;
        r.status = std::move(s);
        r.exec_errno = exec_errno;
        finish(std::move(r));
        return;
    }
    r = main_exit_;
    if (cancel_reason != ExitReason::none) {
        r.reason = cancel_reason;
    }
    emit_summary();
    finish(std::move(r));
}

bool Supervisor::prepare_and_spawn(vdr::PtyDriver* pty) {
    const core::SessionConfig& c = cfg_->cfg;
    auto fail = [&](Code code, std::string msg, int exec_errno, const char* name) {
        emit(EventKind::lifecycle, Severity::error, name, msg, 0, exec_errno);
        Status s{code, std::move(msg)};
        finish_start(s, exec_errno);
        core::ExitResult r;
        r.reason = ExitReason::start_failed;
        r.status = std::move(s);
        r.exec_errno = exec_errno;
        finish(std::move(r));
        return false;
    };

    vfs::ResolveOptions ro;
    vfs::Resolved cwd;
    if (int e = vfs::resolve(table_, c.cwd, ro, cwd); e != 0 || !cwd.exists || !cwd.is_dir) {
        int err = e != 0 ? e : (!cwd.exists ? ENOENT : ENOTDIR);
        return fail(Code::invalid_argument,
                    "guest working directory " + c.cwd + " is not usable: " + errno_text(err), err,
                    "session.cwd_invalid");
    }

    std::string path_list = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    for (const auto& kv : cfg_->final_env) {
        if (kv.rfind("PATH=", 0) == 0) {
            path_list = kv.substr(5);
        }
    }

    std::vector<std::string> argv = c.argv;
    std::string program;
    if (argv.empty()) {
        for (const char* shell : {"/bin/sh", "/bin/bash", "/bin/ash"}) {
            vfs::Resolved r;
            if (vfs::resolve(table_, shell, ro, r) == 0 && r.exists && !r.is_dir) {
                program = shell;
                break;
            }
        }
        if (program.empty()) {
            return fail(
                Code::exec_failed,
                "no command given and no shell (/bin/sh, /bin/bash, /bin/ash) in the rootfs",
                ENOENT, "session.no_shell");
        }
        argv.push_back(program);
        emit(EventKind::log, Severity::info, "session.default_shell",
             "no command given; running " + program, 0, 0);
    } else if (argv[0].find('/') == std::string::npos) {
        if (int e = exec_.search_path(argv[0], path_list, c.cwd, ro, program); e != 0) {
            return fail(Code::exec_failed,
                        "command '" + argv[0] + "' not found in guest PATH (" + path_list + ")", e,
                        "session.command_not_found");
        }
    } else {
        program = argv[0];
    }

    ExecPlan plan;
    ExecDiag diag;
    if (int e = exec_.plan(program, argv, c.cwd, ro, plan, diag); e != 0) {
        std::string msg = "cannot execute " + program + ": " + errno_text(e);
        if (!diag.message.empty()) {
            msg += "; " + diag.message;
            emit(EventKind::diagnostic, Severity::error, diag.name, diag.message, 0, e);
        }
        return fail(Code::exec_failed, msg, e, "session.exec_failed");
    }

    std::string load_request;
    if (loader_mode()) {
        std::string execfn = vfs::is_absolute(program) ? program : vfs::join(c.cwd, program);
        load_request = make_load_request(plan, execfn);
        if (load_request.empty()) {
            return fail(Code::exec_failed, "cannot execute " + program + ": paths too long for the loader request",
                        ENAMETOOLONG, "session.exec_failed");
        }
        plan.host_exec = opts_.loader_path;
        plan.argv = plan.program_argv;
    }

    std::vector<char*> argvp;
    argvp.reserve(plan.argv.size() + 1);
    for (auto& a : plan.argv) {
        argvp.push_back(a.data());
    }
    argvp.push_back(nullptr);
    std::vector<std::string> env = cfg_->final_env;
    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (auto& e : env) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);

    // Hard links: emulated where the host refuses link(2) (Android app storage); objects left by
    // an earlier session are registered so their link counts are presented either way. The probe
    // runs in the host TMPDIR, never in the guest tree: in an app it is the same storage under
    // the same policy, and the rootfs is left untouched for whoever copies or archives it.
    if (!table_.rootfs().read_only) {
        const char* tmp = std::getenv("TMPDIR");
        links_enabled_ = LinkStore::kernel_refuses_link(tmp != nullptr && tmp[0] == '/' ? tmp : "/tmp");
    }
    links_.scan(table_);
    if (links_enabled_) {
        emit(EventKind::log, Severity::info, "fs.hardlink_emulation",
             "the host refuses link(2) in the rootfs; hard links are emulated", 0, 0);
    }

    host_policy_ = platform::seccomp_status() != "0";
    if (int afd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_AUDIT); afd >= 0) {
        ::close(afd);
    } else {
        audit_refused_ = errno == EACCES || errno == EPERM;
    }

    std::vector<sock_filter> filter;
    vhdp_sock_fprog_view fprog{};
    if (opts_.use_seccomp) {
        FilterPolicy fp;
        fp.audit_arch = arch::native_audit_arch();
#if defined(__x86_64__)
        fp.check_x32 = true;
#endif
        bool identity = c.uid.has_value() || c.gid.has_value();
        if (!c.trace) {
            for (const auto& s : abi::all_syscalls()) {
                bool allow = s.cls == abi::SyscallClass::pass_through;
                if (identity && (s.nr == __NR_fstat || s.nr == __NR_getgroups)) {
                    allow = false;
                }
                if ((links_enabled_ || links_.has_objects()) && s.nr == __NR_fstat) {
                    allow = false; // link counts of emulated hard links
                }
                if (links_enabled_ && s.nr == __NR_getdents64) {
                    allow = false; // entry types of emulated hard links
                }
#ifdef __NR_seccomp
                if (s.nr == __NR_seccomp) {
                    allow = false; // a guest installing a filter of its own
                }
#endif
                if (s.nr == __NR_prctl) {
                    allow = false; // PR_SET_SECCOMP
                }
                if (!identity &&
                    (s.handler == abi::Handler::getuid || s.handler == abi::Handler::geteuid ||
                     s.handler == abi::Handler::getgid || s.handler == abi::Handler::getegid ||
                     s.handler == abi::Handler::getresuid || s.handler == abi::Handler::getresgid ||
                     s.handler == abi::Handler::setid)) {
                    allow = true;
                }
                if (s.handler == abi::Handler::socket && c.network == core::NetworkPolicy::host &&
                    !audit_refused_) {
                    allow = true;
                }
                if (allow) {
                    fp.allow.push_back(s.nr);
                }
            }
            fp.network_none = c.network == core::NetworkPolicy::none;
            fp.nr_socket = __NR_socket;
            fp.nr_clone = __NR_clone;
            fp.nr_sendto = __NR_sendto;
        }
        filter = build_seccomp_filter(fp);
        if (filter.size() > 0xffff) {
            return fail(Code::internal, "seccomp filter too large", 0, "session.seccomp_failed");
        }
        fprog.len = static_cast<std::uint16_t>(filter.size());
        fprog.filter = filter.data();
    }

    int sync[2];
    int errp[2];
    if (::pipe2(sync, O_CLOEXEC) != 0) {
        return fail(Code::io, "pipe2: " + errno_text(errno), 0, "session.spawn_failed");
    }
    UniqueFd sync_rd(sync[0]);
    UniqueFd sync_wr(sync[1]);
    if (::pipe2(errp, O_CLOEXEC) != 0) {
        return fail(Code::io, "pipe2: " + errno_text(errno), 0, "session.spawn_failed");
    }
    UniqueFd err_rd(errp[0]);
    UniqueFd err_wr(errp[1]);
    int readyp[2];
    if (::pipe2(readyp, O_CLOEXEC) != 0) {
        return fail(Code::io, "pipe2: " + errno_text(errno), 0, "session.spawn_failed");
    }
    UniqueFd ready_rd(readyp[0]);
    UniqueFd ready_wr(readyp[1]);

    vhdp_bootstrap b{};
    b.ready_fd = ready_wr.get();
    b.sync_fd = sync_rd.get();
    b.error_fd = err_wr.get();
    b.stdio_fds[0] = b.stdio_fds[1] = b.stdio_fds[2] = -1;
    b.ctty_fd = -1;
    if (c.stdio == core::StdioMode::pty && pty != nullptr) {
        b.stdio_fds[0] = b.stdio_fds[1] = b.stdio_fds[2] = pty->slave();
        b.ctty_fd = pty->slave();
        b.new_session = 1;
    } else if (c.stdio == core::StdioMode::fds) {
        for (int i = 0; i < 3; ++i) {
            b.stdio_fds[i] = c.stdio_fds[i];
        }
    }
    b.reset_signals = 1;
    rlimit rl{};
    b.max_fd = 65536;
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        b.max_fd = rl.rlim_cur > (1u << 20) ? (1u << 20) : static_cast<std::uint32_t>(rl.rlim_cur);
    }
    b.host_cwd = cwd.host.c_str();
    b.exec_path = plan.host_exec.c_str();
    b.argv = argvp.data();
    b.envp = envp.data();
    b.seccomp = opts_.use_seccomp ? &fprog : nullptr;

    std::int32_t pidfd = -1;
    long pid = vhdp_bootstrap_spawn(&b, &pidfd);
    if (pid < 0) {
        return fail(Code::io, "clone: " + errno_text(static_cast<int>(-pid)), 0,
                    "session.spawn_failed");
    }
    sync_rd.reset();
    err_wr.reset();
    ready_wr.reset();
    {
        // Seize only once the child is traceable; EOF means it died first, which the
        // attach below reports.
        char byte = 0;
        while (::read(ready_rd.get(), &byte, 1) < 0 && errno == EINTR) {
        }
    }
    if (pty != nullptr) {
        pty->close_slave();
    }
    {
        std::lock_guard<std::mutex> lock(known_mu_);
        main_pid_ = static_cast<pid_t>(pid);
        main_pidfd_ = pidfd;
        known_.insert(main_pid_);
    }
    err_pipe_rd_ = err_rd.release();

    unsigned long options = platform::kOptTraceSysGood | platform::kOptTraceFork |
                            platform::kOptTraceVfork | platform::kOptTraceClone |
                            platform::kOptTraceExec | platform::kOptExitKill;
    if (opts_.use_seccomp) {
        options |= platform::kOptTraceSeccomp;
    }
    if (ptrace_call(platform::kPtraceSeize, main_pid_, 0, options) != 0) {
        int e = errno;
        ::kill(main_pid_, SIGKILL);
        int st = 0;
        while (::waitpid(main_pid_, &st, __WALL) < 0 && errno == EINTR) {
        }
        {
            std::lock_guard<std::mutex> lock(known_mu_);
            main_reaped_ = true;
            known_.erase(main_pid_);
        }
        return fail(Code::engine_unavailable, "PTRACE_SEIZE of the guest failed: " + errno_text(e),
                    0, "session.ptrace_denied");
    }
    Tracee& t = get_or_add(main_pid_);
    t.fresh = false;
    t.bootstrap = true;
    t.tgid = main_pid_;
    t.load_request = std::move(load_request);
    t.load_exe = plan.guest_program;

    JsonWriter w;
    w.begin_object();
    w.key("program").value(plan.guest_program);
    w.key("host_exec").value(plan.host_exec);
    w.key("via_loader").value(plan.via_loader);
    if (plan.via_loader) {
        w.key("loader").value(plan.guest_loader);
        w.key("loader_argv0").value(plan.loader_argv0);
    }
    w.key("userland_loader").value(opts_.loader_path);
    w.key("seccomp").value(opts_.use_seccomp);
    w.key("cwd").value(c.cwd);
    w.end_object();
    emit(EventKind::lifecycle, Severity::debug, "session.spawned", "guest process created",
         main_pid_, 0, w.str());

    char go = 'x';
    ssize_t n = 0;
    do {
        n = ::write(sync_wr.get(), &go, 1);
    } while (n < 0 && errno == EINTR);
    return true;
}

Tracee& Supervisor::get_or_add(pid_t tid, bool* created) {
    auto [it, inserted] = tracees_.try_emplace(tid);
    if (inserted) {
        it->second.tid = tid;
        if (tid > last_pid_) {
            last_pid_ = tid;
        }
        // A new auto-attached child returns from fork/clone: under PTRACE_SYSCALL its
        // first syscall stop is an exit stop.
        it->second.in_syscall = !opts_.use_seccomp;
        std::lock_guard<std::mutex> lock(known_mu_);
        known_.insert(tid);
        // A task created after cancellation must not survive.
        if (cancel_reason_.load() != 0 || main_exited_) {
            ::kill(tid, SIGKILL);
        }
    }
    if (created != nullptr) {
        *created = inserted;
    }
    return it->second;
}

void Supervisor::remove_tracee(pid_t tid) {
    tracees_.erase(tid);
    exe_of_.erase(tid); // keyed by thread-group id: only a leader's entry can match
    creds_.erase(tid);
    guest_filtered_.erase(tid);
    std::lock_guard<std::mutex> lock(known_mu_);
    known_.erase(tid);
    if (tid == main_pid_) {
        main_reaped_ = true;
    }
}

void Supervisor::kill_all() noexcept {
    std::lock_guard<std::mutex> lock(known_mu_);
    for (pid_t p : known_) {
        ::kill(p, SIGKILL);
    }
}

void Supervisor::event_loop() {
    for (;;) {
        int status = 0;
        pid_t pid = ::waitpid(-1, &status, __WALL | __WNOTHREAD);
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                break;
            }
            emit(EventKind::log, Severity::error, "supervisor.wait_failed",
                 "waitpid: " + errno_text(errno), 0, errno);
            kill_all();
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            handle_exit(pid, status);
        } else if (WIFSTOPPED(status)) {
            handle_stop(pid, status);
        }
        if (main_exited_ && tracees_.empty()) {
            // Remaining non-child tracees (if any) are reported until ECHILD.
            continue;
        }
    }
}

void Supervisor::handle_exit(pid_t pid, int status) {
    constexpr std::size_t kMaxExited = 4096;
    if (exited_.size() >= kMaxExited) {
        exited_.clear();
    }
    exited_.insert(pid);
    bool was_main = pid == main_pid_;
    remove_tracee(pid);
    if (!was_main) {
        return;
    }
    main_exited_ = true;
    if (WIFEXITED(status)) {
        main_exit_.reason = ExitReason::normal;
        main_exit_.exit_code = WEXITSTATUS(status);
    } else {
        main_exit_.reason = ExitReason::signaled;
        main_exit_.term_signal = WTERMSIG(status);
    }
    if (!tracees_.empty()) {
        emit(EventKind::lifecycle, Severity::debug, "session.reap_remaining",
             "initial process exited; killing " + std::to_string(tracees_.size()) +
                 " remaining guest task(s)",
             pid, 0);
    }
    kill_all();
}

void Supervisor::resume(Tracee& t, int sig) {
    long req =
        (!opts_.use_seccomp || t.pending.active) ? platform::kPtraceSyscall : platform::kPtraceCont;
    if (ptrace_call(req, t.tid, 0, static_cast<std::uintptr_t>(sig)) != 0 && errno != ESRCH) {
        emit(EventKind::log, Severity::warning, "supervisor.resume_failed",
             "ptrace resume: " + errno_text(errno), t.tid, errno);
    }
}

void Supervisor::handle_stop(pid_t pid, int status) {
    bool created = false;
    Tracee& t = get_or_add(pid, &created);
    int sig = WSTOPSIG(status);
    int event = (status >> 16) & 0xff;

    if (sig == (SIGTRAP | 0x80)) {
        on_syscall_stop(t);
        return;
    }
    if (event == platform::kEventStop) {
        // Reported with the stopping signal for a group-stop (SIGTSTP from Ctrl-Z, SIGTTOU for a
        // background process touching the terminal) and with SIGTRAP otherwise. Mistaking a
        // group-stop for a signal-delivery-stop re-injects a signal the kernel ignores there and
        // lets the process run on: it retries, stops again, and spins, flooding its parent with
        // SIGCHLD (`timeout 180 apt-get update` never came back).
        if (t.fresh) {
            t.fresh = false;
            resume(t, 0);
            return;
        }
        if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
            // Keep the tracee stopped but let it be woken by SIGCONT.
            if (ptrace_call(platform::kPtraceListen, pid, 0, 0) != 0 && errno != ESRCH) {
                resume(t, 0);
            }
            return;
        }
        resume(t, 0);
        return;
    }
    if (sig == SIGTRAP && event != 0) {
        unsigned long msg = 0;
        switch (event) {
            case platform::kEventFork:
            case platform::kEventVfork:
            case platform::kEventClone: {
                if (ptrace_call(platform::kPtraceGetEventMsg, pid, 0,
                                reinterpret_cast<std::uintptr_t>(&msg)) == 0) {
                    auto child = static_cast<pid_t>(msg);
                    // The child's own stops can all be reported before this event, its exit
                    // included. Re-adding it then would keep a dead id among the session's
                    // tasks, and the final SIGKILL would reach whatever reuses that id.
                    char state = exited_.count(child) != 0 && tracees_.count(child) == 0
                                     ? platform::process_state(child)
                                     : 'R';
                    bool gone = state == '\0' || state == 'Z' || state == 'X';
                    if (!gone) {
                        exited_.erase(child);
                        (void)get_or_add(child);
                        inherit_process_state(t, child);
                    }
                }
                resume(t, 0);
                return;
            }
            case platform::kEventExec: {
                std::string request;
                std::string exe;
                if (ptrace_call(platform::kPtraceGetEventMsg, pid, 0,
                                reinterpret_cast<std::uintptr_t>(&msg)) == 0 &&
                    static_cast<pid_t>(msg) != pid) {
                    // A non-leader thread exec'd: the old thread id vanished silently. Its
                    // load request belongs to the image that now runs under pid.
                    if (auto it = tracees_.find(static_cast<pid_t>(msg)); it != tracees_.end()) {
                        request = std::move(it->second.load_request);
                        exe = std::move(it->second.load_exe);
                    }
                    remove_tracee(static_cast<pid_t>(msg));
                }
                Tracee& cur = tracees_[pid];
                if (request.empty()) {
                    request = std::move(cur.load_request);
                    exe = std::move(cur.load_exe);
                }
                cur.load_request.clear();
                cur.load_exe.clear();
                if (!request.empty()) {
                    inject_load_request(pid, request, exe);
                }
                cur.exec_seen = true;
                cur.tgid = pid;
                if (!opts_.use_seccomp) {
                    cur.in_syscall = true; // still inside execve; next stop is its exit
                }
                if (cur.bootstrap) {
                    cur.bootstrap = false;
                    if (err_pipe_rd_ >= 0) {
                        ::close(err_pipe_rd_);
                        err_pipe_rd_ = -1;
                    }
                    emit(EventKind::lifecycle, Severity::info, "session.running",
                         "guest program started", pid, 0);
                    finish_start(Status::ok(), 0);
                }
                resume(cur, 0);
                return;
            }
            case platform::kEventSeccomp: {
                if (ptrace_call(platform::kPtraceGetEventMsg, pid, 0,
                                reinterpret_cast<std::uintptr_t>(&msg)) == 0 &&
                    (msg & 0xffffu) == kSeccompTraceMarker) {
                    on_syscall_enter(t);
                } else {
                    resume(t, 0);
                }
                return;
            }
            default:
                resume(t, 0);
                return;
        }
    }
    if (created || t.fresh) {
        // Initial SIGSTOP of an auto-attached child reported as a plain stop.
        if (sig == SIGSTOP) {
            t.fresh = false;
            resume(t, 0);
            return;
        }
        t.fresh = false;
    }
    if (sig == SIGSYS && handle_host_seccomp_trap(t)) {
        return;
    }
    // Signal-delivery-stop: inject the signal unchanged.
    resume(t, sig);
}

bool Supervisor::handle_host_seccomp_trap(Tracee& t) {
    // The host may run every process under a seccomp policy of its own; an Android app's
    // returns SECCOMP_RET_TRAP for syscalls outside the app allowlist. With several filters the
    // kernel takes the strongest action, and TRAP outranks the RET_TRACE of the supervisor's
    // filter, so such a syscall never reaches the syscall handlers: the guest receives SIGSYS
    // with the syscall rolled back. glibc issues several of these on every start (rseq,
    // set_robust_list) and package managers issue more (setgroups, setresuid), so delivering
    // the signal would kill almost every guest. Answer the syscall here instead: the identity
    // family from the emulated credentials, everything else with ENOSYS -- the answer the
    // kernel gives for a syscall it does not implement, which callers already handle.
    siginfo_t si{};
    if (ptrace_call(platform::kPtraceGetSigInfo, t.tid, 0, reinterpret_cast<std::uintptr_t>(&si)) !=
        0) {
        return false;
    }
    constexpr int kSysSeccomp = 1; // si_code of a seccomp SECCOMP_RET_TRAP
    if (si.si_signo != SIGSYS || si.si_code != kSysSeccomp) {
        return false; // a SIGSYS sent by someone: deliver it
    }
    long nr = si.si_syscall;
    if (nr >= 0 && (!host_policy_ || guest_filtered_.count(tgid_of(t)) != 0)) {
        return false; // the guest's own filter trapped it: its SIGSYS handler decides
    }
    if (nr < 0) {
        // A syscall the supervisor skipped (number -1) and already answered. Without seccomp
        // acceleration the host policy runs after the entry stop and traps the skip marker;
        // the answer set at that stop stands.
        resume(t, 0);
        return true;
    }
    arch::RegsAccess regs(t.tid);
    if (regs.fetch() != 0) {
        return false;
    }
    long ret = -ENOSYS;
    const abi::SyscallInfo* info = static_cast<std::uint32_t>(si.si_arch) == arch::native_audit_arch()
                                       ? abi::lookup_syscall(nr)
                                       : nullptr;
    const core::SessionConfig& c = cfg_->cfg;
    bool identity = c.uid.has_value() || c.gid.has_value();
    if (info != nullptr && restart_legacy(t, regs, nr)) {
        if (regs.commit() != 0) {
            return false;
        }
        if (sink_.wants(Severity::debug)) {
            emit(EventKind::trace, Severity::debug, "syscall.host_policy_restart",
                 std::string("host seccomp policy refused ") + info->name +
                     "; restarted as its modern equivalent",
                 t.tid, 0);
        }
        resume(t, 0);
        return true;
    }
    if (info != nullptr) {
        bool id_call = info->handler == abi::Handler::getuid ||
                       info->handler == abi::Handler::geteuid ||
                       info->handler == abi::Handler::getgid ||
                       info->handler == abi::Handler::getegid ||
                       info->handler == abi::Handler::getresuid ||
                       info->handler == abi::Handler::getresgid ||
                       info->handler == abi::Handler::setid
#ifdef __NR_getgroups
                       || nr == __NR_getgroups
#endif
            ;
        if (id_call && identity) {
            SyscallAction a = handle_identity(t, regs, *info);
            if (a.skip) {
                ret = a.ret;
            }
        } else if (info->handler == abi::Handler::setid) {
            ret = -EPERM; // without an emulated identity the guest simply lacks the privilege
        }
    }
    regs.set_return(ret);
    if (int e = regs.commit(); e != 0) {
        return false;
    }
    if (ret == -ENOSYS) {
        ++denials_[{nr, ENOSYS}];
    }
    if (sink_.wants(Severity::debug)) {
        emit(EventKind::trace, Severity::debug, "syscall.host_policy_trap",
             std::string("host seccomp policy refused ") + (info != nullptr ? info->name : "syscall") +
                 " (" + std::to_string(nr) + "); answered " + std::to_string(ret),
             t.tid, static_cast<int>(-ret));
    }
    resume(t, 0);
    return true;
}

void Supervisor::on_syscall_stop(Tracee& t) {
    bool is_exit = false;
    std::uint8_t op = platform::kSyscallInfoNone;
    // With seccomp acceleration a syscall stop is the exit of a syscall whose entry asked for it;
    // only an unexpected stop is worth a query.
    if (opts_.get_syscall_info && !(opts_.use_seccomp && t.pending.active)) {
        platform::PtraceSyscallInfo info{};
        if (ptrace_call(platform::kPtraceGetSyscallInfo, t.tid, sizeof(info),
                        reinterpret_cast<std::uintptr_t>(&info)) > 0) {
            op = info.op;
        }
    }
    if (op == platform::kSyscallInfoEntry) {
        is_exit = false;
    } else if (op == platform::kSyscallInfoExit) {
        is_exit = true;
    } else if (opts_.use_seccomp) {
        is_exit = true; // syscall stops only occur as requested exit stops
    } else {
        is_exit = t.in_syscall;
    }
    if (!opts_.use_seccomp) {
        t.in_syscall = !is_exit;
    }
    if (is_exit) {
        on_syscall_exit(t);
    } else {
        on_syscall_enter(t);
    }
}

void Supervisor::on_syscall_enter(Tracee& t) {
    t.pending = PendingSyscall{};
    t.exec_seen = false;
    if (!t.bootstrap) {
        // Reaching another syscall means the last rewritten execve did not replace the image.
        t.load_request.clear();
        t.load_exe.clear();
    }
    arch::RegsAccess regs(t.tid);
    if (int e = regs.fetch(); e != 0) {
        if (e != ESRCH) {
            resume(t, 0);
        }
        return;
    }
    const arch::SyscallView& v = regs.view();
    t.pending.nr = v.nr;
    if (t.bootstrap) {
        // Only the supervisor-prepared execve runs before the first exec event.
        resume(t, 0);
        return;
    }
    SyscallAction action;
    const abi::SyscallInfo* info = v.native_abi ? abi::lookup_syscall(v.nr) : nullptr;
    if (!v.native_abi) {
        action =
            deny(t, nullptr, v.nr, ENOSYS, "syscall.foreign_abi",
                 "non-native syscall ABI (compat/x32) is not supported by the rootless engine");
    } else if (info == nullptr) {
        action = deny(t, nullptr, v.nr, ENOSYS, "syscall.unknown",
                      "syscall number " + std::to_string(v.nr) + " is not in the syscall table");
    } else {
        if (cfg_->cfg.trace && sink_.wants(Severity::debug)) {
            JsonWriter w;
            w.begin_object()
                .key("syscall")
                .value(info->name)
                .key("nr")
                .value(static_cast<std::int64_t>(v.nr));
            w.key("class").value(abi::class_name(info->cls)).key("args").begin_array();
            for (auto a : v.args) {
                w.value(a);
            }
            w.end_array().end_object();
            emit(EventKind::trace, Severity::debug, "syscall.enter", info->name, t.tid, 0, w.str());
        }
        action = dispatch(t, regs, *info);
    }

    if (action.skip) {
        // Undo argument rewrites made before the handler decided to refuse the call.
        for (int i = 0; i < 6; ++i) {
            if ((t.pending.restore_mask & (1u << i)) != 0) {
                regs.set_arg(i, t.pending.orig_args[static_cast<std::size_t>(i)]);
            }
        }
        regs.set_nr(-1);
        regs.set_return(action.ret);
        if (!opts_.use_seccomp) {
            t.pending.active = true;
            t.pending.fixup = Fixup::emulated_return;
            t.pending.emulated = action.ret;
            t.pending.restore_mask = 0;
        } else {
            t.pending = PendingSyscall{};
        }
    }
    if (int e = regs.commit(); e != 0 && e != ESRCH) {
        emit(EventKind::log, Severity::warning, "supervisor.regs_failed",
             "register update failed: " + errno_text(e), t.tid, e);
    }
    resume(t, 0);
}

void Supervisor::on_syscall_exit(Tracee& t) {
    if (!t.pending.active) {
        resume(t, 0);
        return;
    }
    if (!t.pending.scratch.empty()) {
        // The guest holds the stand-in open (or failed to open it); the name is not needed.
        ::unlink(t.pending.scratch.c_str());
    }
    arch::RegsAccess regs(t.tid);
    if (int e = regs.fetch(); e != 0) {
        t.pending = PendingSyscall{};
        if (e != ESRCH) {
            resume(t, 0);
        }
        return;
    }
    if (!t.exec_seen) {
        fixup_exit(t, regs);
        for (int i = 0; i < 6; ++i) {
#if defined(__aarch64__)
            if (i == 0) {
                continue; // x0 carries the return value
            }
#endif
            if ((t.pending.restore_mask & (1u << i)) != 0) {
                regs.set_arg(i, t.pending.orig_args[static_cast<std::size_t>(i)]);
            }
        }
        (void)regs.commit();
    }
    t.pending = PendingSyscall{};
    t.exec_seen = false;
    resume(t, 0);
}

bool Supervisor::restart_legacy(Tracee& t, arch::RegsAccess& regs, long nr) {
    // An Android app's policy allows the syscalls bionic makes. On x86_64 glibc still makes the
    // pre-*at family (open, stat, access, dup2, pipe, poll, ...), which bionic never does, so
    // they trap. The kernel has rolled the call back and left the instruction pointer after
    // the syscall instruction: restarting it as the equivalent call the policy allows gives the
    // guest the same result, and the supervisor sees and translates that call as usual.
    // A restart rewrites argument registers the original call did not use. The C library
    // wrappers making these calls do not read them afterwards; glibc's vfork does, so vfork is
    // not restarted.
    const auto a = regs.view().args;
    constexpr auto kFdcwd = static_cast<std::uint64_t>(-100);
    auto restart = [&](long to, std::initializer_list<std::uint64_t> args) {
        int i = 0;
        for (std::uint64_t v : args) {
            regs.set_arg(i++, v);
        }
        regs.restart_as(to);
        return true;
    };
    auto answer = [&](long value) {
        regs.set_return(value);
        return true;
    };
    // Timeouts converted for the replacement call live below the stack pointer, clear of the
    // scratch the translation of that call will use.
    auto scratch = [&](const void* data, std::size_t len, std::uint64_t& addr) {
        StackWriter sw(t.tid, regs.view().sp);
        sw.skip(std::size_t{64} * 1024);
        return sw.push(data, len, addr);
    };
    // AArch64 has none of these syscalls: every case below compiles away there.
    (void)a;
    (void)kFdcwd;
    (void)restart;
    (void)answer;
    (void)scratch;
    switch (nr) {
#if defined(__NR_open) && defined(__NR_openat)
        case __NR_open:
            return restart(__NR_openat, {kFdcwd, a[0], a[1], a[2]});
#endif
#if defined(__NR_creat) && defined(__NR_openat)
        case __NR_creat:
            return restart(__NR_openat,
                           {kFdcwd, a[0], std::uint64_t{O_CREAT | O_WRONLY | O_TRUNC}, a[1]});
#endif
#if defined(__NR_stat) && defined(__NR_newfstatat)
        case __NR_stat:
            return restart(__NR_newfstatat, {kFdcwd, a[0], a[1], 0});
        case __NR_lstat:
            return restart(__NR_newfstatat, {kFdcwd, a[0], a[1], std::uint64_t{AT_SYMLINK_NOFOLLOW}});
#endif
#if defined(__NR_access) && defined(__NR_faccessat)
        case __NR_access:
            return restart(__NR_faccessat, {kFdcwd, a[0], a[1]});
#endif
#if defined(__NR_dup2) && defined(__NR_dup3)
        case __NR_dup2: {
            if (a[0] != a[1]) {
                return restart(__NR_dup3, {a[0], a[1], 0});
            }
            // dup2(fd, fd) returns fd when it is open; dup3 would refuse with EINVAL.
            std::string link = "/proc/" + std::to_string(t.tid) + "/fd/" +
                               std::to_string(static_cast<int>(a[0]));
            struct stat st{};
            return answer(::lstat(link.c_str(), &st) == 0 ? static_cast<long>(static_cast<int>(a[0]))
                                                           : -EBADF);
        }
#endif
#if defined(__NR_pipe) && defined(__NR_pipe2)
        case __NR_pipe:
            return restart(__NR_pipe2, {a[0], 0});
#endif
#if defined(__NR_poll) && defined(__NR_ppoll)
        case __NR_poll: {
            auto ms = static_cast<std::int32_t>(a[2]);
            std::uint64_t ts_addr = 0;
            if (ms >= 0) {
                std::int64_t ts[2] = {ms / 1000, static_cast<std::int64_t>(ms % 1000) * 1000000};
                if (scratch(ts, sizeof(ts), ts_addr) != 0) {
                    return answer(-ENOMEM);
                }
            }
            return restart(__NR_ppoll, {a[0], a[1], ts_addr, 0, 8});
        }
#endif
#if defined(__NR_select) && defined(__NR_pselect6)
        case __NR_select: {
            std::uint64_t ts_addr = 0;
            if (a[4] != 0) {
                std::int64_t tv[2] = {0, 0};
                if (read_mem(t.tid, a[4], tv, sizeof(tv)) != 0) {
                    return answer(-EFAULT);
                }
                if (tv[0] < 0 || tv[1] < 0) {
                    return answer(-EINVAL);
                }
                // Normalised as the kernel does (microseconds beyond a second carry over). The
                // remaining time is not written back: pselect6 leaves its timeout unchanged.
                std::int64_t ts[2] = {tv[0] + tv[1] / 1000000, (tv[1] % 1000000) * 1000};
                if (scratch(ts, sizeof(ts), ts_addr) != 0) {
                    return answer(-ENOMEM);
                }
            }
            return restart(__NR_pselect6, {a[0], a[1], a[2], a[3], ts_addr, 0});
        }
#endif
#if defined(__NR_epoll_wait) && defined(__NR_epoll_pwait)
        case __NR_epoll_wait:
            return restart(__NR_epoll_pwait, {a[0], a[1], a[2], a[3], 0, 8});
#endif
#if defined(__NR_epoll_create) && defined(__NR_epoll_create1)
        case __NR_epoll_create:
            return static_cast<std::int32_t>(a[0]) <= 0 ? answer(-EINVAL)
                                                        : restart(__NR_epoll_create1, {0});
#endif
#if defined(__NR_inotify_init) && defined(__NR_inotify_init1)
        case __NR_inotify_init:
            return restart(__NR_inotify_init1, {0});
#endif
#if defined(__NR_eventfd) && defined(__NR_eventfd2)
        case __NR_eventfd:
            return restart(__NR_eventfd2, {a[0], 0});
#endif
#if defined(__NR_signalfd) && defined(__NR_signalfd4)
        case __NR_signalfd:
            return restart(__NR_signalfd4, {a[0], a[1], a[2], 0});
#endif
#if defined(__NR_mkdir) && defined(__NR_mkdirat)
        case __NR_mkdir:
            return restart(__NR_mkdirat, {kFdcwd, a[0], a[1]});
        case __NR_mknod:
            return restart(__NR_mknodat, {kFdcwd, a[0], a[1], a[2]});
#endif
#if defined(__NR_rmdir) && defined(__NR_unlinkat)
        case __NR_rmdir:
            return restart(__NR_unlinkat, {kFdcwd, a[0], std::uint64_t{AT_REMOVEDIR}});
        case __NR_unlink:
            return restart(__NR_unlinkat, {kFdcwd, a[0], 0});
#endif
#if defined(__NR_rename) && defined(__NR_renameat)
        case __NR_rename:
            return restart(__NR_renameat, {kFdcwd, a[0], kFdcwd, a[1]});
#endif
#if defined(__NR_link) && defined(__NR_linkat)
        case __NR_link:
            return restart(__NR_linkat, {kFdcwd, a[0], kFdcwd, a[1], 0});
        case __NR_symlink:
            return restart(__NR_symlinkat, {a[0], kFdcwd, a[1]});
        case __NR_readlink:
            return restart(__NR_readlinkat, {kFdcwd, a[0], a[1], a[2]});
#endif
#if defined(__NR_chmod) && defined(__NR_fchmodat)
        case __NR_chmod:
            return restart(__NR_fchmodat, {kFdcwd, a[0], a[1]});
        case __NR_chown:
            return restart(__NR_fchownat, {kFdcwd, a[0], a[1], a[2], 0});
        case __NR_lchown:
            return restart(__NR_fchownat,
                           {kFdcwd, a[0], a[1], a[2], std::uint64_t{AT_SYMLINK_NOFOLLOW}});
#endif
#if defined(__NR_utimes) && defined(__NR_utimensat)
        case __NR_utimes: {
            std::uint64_t ts_addr = 0;
            if (a[1] != 0) {
                std::int64_t tv[4] = {0, 0, 0, 0};
                if (read_mem(t.tid, a[1], tv, sizeof(tv)) != 0) {
                    return answer(-EFAULT);
                }
                std::int64_t ts[4] = {tv[0], tv[1] * 1000, tv[2], tv[3] * 1000};
                if (scratch(ts, sizeof(ts), ts_addr) != 0) {
                    return answer(-ENOMEM);
                }
            }
            return restart(__NR_utimensat, {kFdcwd, a[0], ts_addr, 0});
        }
        case __NR_utime: {
            std::uint64_t ts_addr = 0;
            if (a[1] != 0) {
                std::int64_t ut[2] = {0, 0}; // actime, modtime
                if (read_mem(t.tid, a[1], ut, sizeof(ut)) != 0) {
                    return answer(-EFAULT);
                }
                std::int64_t ts[4] = {ut[0], 0, ut[1], 0};
                if (scratch(ts, sizeof(ts), ts_addr) != 0) {
                    return answer(-ENOMEM);
                }
            }
            return restart(__NR_utimensat, {kFdcwd, a[0], ts_addr, 0});
        }
#endif
#if defined(__NR_getpgrp) && defined(__NR_getpgid)
        case __NR_getpgrp:
            return restart(__NR_getpgid, {0});
#endif
#if defined(__NR_fork) && defined(__NR_clone)
        case __NR_fork:
            return restart(__NR_clone, {std::uint64_t{SIGCHLD}, 0, 0, 0, 0});
        // vfork is not restarted: glibc keeps its return address in rdi across the syscall
        // (pop %rdi; syscall; push %rdi), which clone's first argument would overwrite.
#endif
#ifdef __NR_time
        case __NR_time: {
            auto now = static_cast<std::int64_t>(::time(nullptr));
            if (a[0] != 0 && write_mem(t.tid, a[0], &now, sizeof(now)) != 0) {
                return answer(-EFAULT);
            }
            return answer(static_cast<long>(now));
        }
#endif
        default:
            return false;
    }
}

void Supervisor::remove_proc_scratch() noexcept {
    if (proc_scratch_.empty()) {
        return;
    }
    if (DIR* d = ::opendir(proc_scratch_.c_str())) {
        while (dirent* ent = ::readdir(d)) {
            if (ent->d_name[0] != '.') {
                std::string path = proc_scratch_ + "/" + ent->d_name;
                ::unlink(path.c_str());
            }
        }
        ::closedir(d);
    }
    ::rmdir(proc_scratch_.c_str());
    proc_scratch_.clear();
}

bool Supervisor::first_report(const std::string& name, const std::string& subject) {
    constexpr std::size_t kMaxReported = 512;
    if (reported_.size() >= kMaxReported) {
        return false;
    }
    return reported_.insert(name + '\0' + subject).second;
}

void Supervisor::emit_summary() {
    if (denials_.empty() || !sink_.wants(Severity::info)) {
        return;
    }
    JsonWriter w;
    w.begin_object().key("denials").begin_array();
    for (const auto& [key, count] : denials_) {
        const abi::SyscallInfo* info = abi::lookup_syscall(key.first);
        w.begin_object();
        w.key("syscall").value(info != nullptr ? info->name : "unknown");
        w.key("nr").value(static_cast<std::int64_t>(key.first));
        w.key("errno").value(vhdp_errno_name(key.second));
        w.key("count").value(count);
        w.end_object();
    }
    w.end_array().end_object();
    emit(EventKind::diagnostic, Severity::info, "syscall.summary",
         "refused syscalls during the session", 0, 0, w.str());
}

} // namespace vhdp::rootless
