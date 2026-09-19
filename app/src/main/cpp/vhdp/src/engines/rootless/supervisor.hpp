// Rootless ptrace supervisor.
//
// Threading: one dedicated supervisor thread per session performs clone,
// PTRACE_SEIZE, waitpid and every ptrace request (ptrace is bound to the tracer
// thread). waitpid uses __WNOTHREAD so only this thread's children and tracees are
// reaped; children of other host threads are never stolen. Other threads interact
// only through signal_main()/cancel(), which send signals under known_mu_.
//
// Process lifecycle: every descendant is auto-attached (fork/vfork/clone events;
// CLONE_UNTRACED is stripped, clone3 is refused so libc falls back to clone).
// The session ends when the initial process exits; remaining tracees are then
// killed with SIGKILL and reaped/observed until none is left. PTRACE_O_EXITKILL
// kills the tree if the supervisor thread dies.
#pragma once

#include "arch/regs.hpp"
#include "core/engine.hpp"
#include "engines/rootless/exec_resolver.hpp"
#include "engines/rootless/link_store.hpp"
#include "linux_abi/syscall_table.hpp"
#include "vfs/mount_table.hpp"
#include "vfs/resolver.hpp"

#include <sys/types.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vhdp::vdr {
class PtyDriver;
}

namespace vhdp::rootless {

struct SupervisorOptions {
    bool use_seccomp = false;
    bool get_syscall_info = false;
    // Host path of the userland loader (loader/load_request.h). Non-empty selects loader
    // mode: every guest program is started by exec'ing this file and mapping the guest ELF
    // from inside the new process, instead of exec'ing the guest's files directly.
    std::string loader_path;
};

enum class Fixup : std::uint8_t {
    none,
    getcwd,
    readlink_proc,
    stat,
    statx,
    emulated_return,
    rename_links, // a rename involving emulated hard links: update their records on success
    dirents,      // getdents64: emulated hard links are listed as regular files
    termios2_get, // TCGETS2 answered with TCGETS: fill in the speeds after the termios part
};

struct PendingSyscall {
    bool active = false; // an exit stop was requested for this syscall
    long nr = -1;
    std::array<std::uint64_t, 6> orig_args{};
    std::uint32_t restore_mask = 0;
    Fixup fixup = Fixup::none;
    std::uint64_t buf = 0;
    std::uint64_t size = 0;
    long emulated = 0;
    pid_t exe_tgid = 0; // readlink of /proc/<tgid>/exe: substitute the guest program in loader mode
    std::string link_host; // readlink of a /proc link: its host path, read again in full at exit
    int fd = -1;        // getdents64: the directory descriptor
    // rename_links: the mount, the emulated links at the source and destination (ids, empty when
    // not one) and both host paths.
    const vfs::Mount* link_mount = nullptr;
    std::string link_src;
    std::string link_dst;
    std::string link_from;
    std::string link_to;
    bool link_exchange = false;
    std::string scratch; // a /proc stand-in file opened by this syscall, removed at its exit
};

// Emulated credentials of one guest process (thread group), used when the session presents a
// guest identity (--uid/--gid). The host credentials never change; these are what the guest's
// get*id/set*id/getgroups/setgroups see, with the kernel's permission rules applied, so a
// program that drops privileges (apt switching to _apt) observes the switch and can no longer
// switch back -- which such programs verify.
struct Creds {
    std::uint32_t ruid = 0, euid = 0, suid = 0, fsuid = 0;
    std::uint32_t rgid = 0, egid = 0, sgid = 0, fsgid = 0;
    std::vector<std::uint32_t> groups;
};

struct Tracee {
    pid_t tid = 0;
    pid_t tgid = 0;          // 0 until looked up
    bool fresh = true;       // waiting for the initial stop of an auto-attached child
    bool in_syscall = false; // enter/exit toggle when PTRACE_GET_SYSCALL_INFO is unavailable
    bool bootstrap = false;  // initial process before its first successful exec
    bool exec_seen = false;  // PTRACE_EVENT_EXEC seen inside the pending syscall
    PendingSyscall pending;
    // Loader mode: the request to hand the loader at this task's next PTRACE_EVENT_EXEC, and
    // the guest program it will run. Set when an execve is rewritten; dropped at the next
    // syscall entry, which is how a failed execve discards it.
    std::string load_request;
    std::string load_exe;
};

// Allocates scratch space below the tracee stack pointer (beyond the red zone).
class StackWriter {
public:
    StackWriter(pid_t tid, std::uint64_t sp) noexcept;
    int push(const void* data, std::size_t len, std::uint64_t& addr) noexcept;
    int push_string(const std::string& s, std::uint64_t& addr) noexcept;
    // Leaves `len` bytes untouched below the current scratch top.
    void skip(std::size_t len) noexcept { top_ = top_ > len ? top_ - len : 0; }

private:
    pid_t tid_;
    std::uint64_t base_;
    std::uint64_t top_;
};

struct SyscallAction {
    bool skip = false; // do not execute; return `ret`
    long ret = 0;
};

struct PathSpec {
    int path_arg = -1;
    int dirfd_arg = -1; // -1: path is relative to the cwd
    bool follow = true;
    bool erofs_if_exists = false;  // modifies an existing object
    bool erofs_if_missing = false; // creates a new object
    bool empty_ok = false;         // AT_EMPTY_PATH semantics
    bool null_ok = false;          // NULL path operates on dirfd
    bool link_name = false;        // names a directory entry itself: an emulated hard link is
                                   // not followed (unlink, rename, link)
    bool resolve_only = false;     // resolve without rewriting the argument (apply_translation)
};

struct TranslatedPath {
    bool present = false; // a path string was translated (false: fd-based / NULL)
    vfs::Resolved r;
    std::string original; // the guest's string
};

class Supervisor {
public:
    Supervisor(std::shared_ptr<const core::ValidatedConfig> cfg, core::EventSink& sink,
               std::uint64_t session_id, SupervisorOptions opts);
    ~Supervisor();
    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    Status start(vdr::PtyDriver* pty);
    core::ExitResult wait();
    Status signal_main(int signo);
    Status cancel(core::ExitReason reason);

private:
    // lifecycle (supervisor.cpp)
    void thread_main() noexcept;
    void run();
    bool prepare_and_spawn(vdr::PtyDriver* pty);
    void event_loop();
    void handle_stop(pid_t pid, int status);
    void handle_exit(pid_t pid, int status);
    void on_syscall_stop(Tracee& t);
    void on_syscall_enter(Tracee& t);
    void on_syscall_exit(Tracee& t);
    void resume(Tracee& t, int sig);
    Tracee& get_or_add(pid_t tid, bool* created = nullptr);
    void remove_tracee(pid_t tid);
    void kill_all() noexcept;
    void finish_start(Status s, int exec_errno);
    void finish(core::ExitResult r);
    void emit(core::EventKind kind, core::Severity sev, std::string name, std::string message,
              pid_t pid, int code, std::string detail = "{}");
    void emit_summary();
    // Diagnostics a guest can trigger in a loop (every /proc/<pid>/root a tool scans) are
    // reported once per name and subject; true the first time.
    bool first_report(const std::string& name, const std::string& subject);
    bool loader_mode() const noexcept { return !opts_.loader_path.empty(); }
    static std::string make_load_request(const ExecPlan& plan, const std::string& execfn);
    void inject_load_request(pid_t pid, const std::string& request, const std::string& exe);
    // A new task was reported by a fork/vfork/clone event: if it is a new process, it starts
    // with its parent's program (exe) and credentials.
    void inherit_process_state(Tracee& parent, pid_t child);
    // A SIGSYS the host's own seccomp policy raised for a guest syscall (an Android app process
    // runs under one). Returns true when the syscall was answered and the signal suppressed.
    bool handle_host_seccomp_trap(Tracee& t);
    // A legacy syscall the host policy trapped, restarted as its modern equivalent (or answered);
    // false when there is none.
    bool restart_legacy(Tracee& t, arch::RegsAccess& regs, long nr);
    Creds& creds_of(Tracee& t);

    // syscall handling (syscall_handlers.cpp)
    SyscallAction dispatch(Tracee& t, arch::RegsAccess& regs, const abi::SyscallInfo& info);
    SyscallAction deny(Tracee& t, const abi::SyscallInfo* info, long nr, int err, const char* name,
                       const std::string& why, core::Severity sev = core::Severity::warning);
    int translate_path(Tracee& t, arch::RegsAccess& regs, StackWriter& sw, const PathSpec& spec,
                       TranslatedPath& out);
    void rewrite_arg(Tracee& t, arch::RegsAccess& regs, int index, std::uint64_t value);
    int guest_cwd(Tracee& t, std::string& out);
    int guest_fd_path(Tracee& t, int fd, std::string& out, bool& is_path);
    vfs::ResolveOptions resolve_options(Tracee& t, bool follow);
    pid_t tgid_of(Tracee& t);
    SyscallAction handle_exec(Tracee& t, arch::RegsAccess& regs, bool at_variant);
    SyscallAction handle_sockaddr(Tracee& t, arch::RegsAccess& regs, int addr_arg, int len_arg,
                                  bool is_bind);
    SyscallAction handle_sendmsg(Tracee& t, arch::RegsAccess& regs, bool multiple);
    SyscallAction handle_signal_target(Tracee& t, const abi::SyscallInfo& info,
                                       arch::RegsAccess& regs);
    SyscallAction handle_fd_meta(Tracee& t, arch::RegsAccess& regs, const abi::SyscallInfo& info,
                                 int fd, bool chown);
    SyscallAction handle_identity(Tracee& t, arch::RegsAccess& regs, const abi::SyscallInfo& info);
    bool guest_is_root(Tracee& t);
    void fixup_exit(Tracee& t, arch::RegsAccess& regs);
    int apply_translation(Tracee& t, arch::RegsAccess& regs, StackWriter& sw, const PathSpec& s,
                          const TranslatedPath& tp);
    // Guest identity and emulated link counts in a kernel stat (or statx) buffer.
    void present_stat(unsigned char* buf, bool statx);
    // Read-only queries run by the supervisor on a translated path (see syscall_handlers.cpp).
    static bool host_queryable(const TranslatedPath& tp);
    long host_stat(Tracee& t, const std::string& host, std::uint64_t buf, bool statx,
                   std::uint64_t flags, std::uint64_t mask);
    long host_readlink(Tracee& t, const std::string& host, std::uint64_t buf, std::uint64_t size);
    long host_xattr(Tracee& t, const abi::SyscallInfo& info, const std::string& host,
                    arch::RegsAccess& regs);
    void present_dirents(Tracee& t, long ret);
    // Whether a name in this session can be an emulated hard link: this session made one, or a
    // store on one of its mounts holds an object (link_store.hpp).
    bool links_visible();
    // /proc stand-ins (proc_synth.hpp): a file with the content to open instead of the host's.
    bool proc_stand_in(Tracee& t, const TranslatedPath& tp, std::string& host_out);
    void remove_proc_scratch() noexcept;

    std::shared_ptr<const core::ValidatedConfig> cfg_;
    core::EventSink& sink_;
    std::uint64_t session_id_;
    SupervisorOptions opts_;
    vfs::MountTable table_;
    ExecResolver exec_;
    uid_t host_uid_;
    gid_t host_gid_;

    std::unordered_map<pid_t, Tracee> tracees_;
    // Loader mode: guest program per thread group. The kernel's /proc/<pid>/exe names the
    // loader, which is outside every guest mount; readlink of it answers with this instead.
    std::unordered_map<pid_t, std::string> exe_of_;
    // Emulated credentials per thread group (identity sessions only), see Creds.
    std::unordered_map<pid_t, Creds> creds_;
    std::map<std::pair<long, int>, std::uint64_t> denials_;
    std::unordered_set<std::string> reported_;
    // Hard-link emulation (link_store.hpp). Enabled when the host refuses link(2) in the rootfs;
    // links already emulated in an earlier session are honoured either way.
    LinkStore links_;
    bool links_enabled_ = false;
    vfs::DirCache dir_cache_;
    std::vector<char> xattr_buf_; // scratch for host_xattr
    std::string proc_scratch_;    // host directory of /proc stand-ins, created on first use
    std::uint64_t proc_scratch_seq_ = 0;
    pid_t last_pid_ = 0; // highest task id seen, for loadavg/stat
    // The host refuses NETLINK_AUDIT sockets (an Android app domain). Guests then get the answer
    // of a kernel built without audit: shadow's useradd/groupadd abort on any other error.
    bool audit_refused_ = false;
    // The host mounts selinuxfs but refuses it to this process (an Android app domain). The
    // guest then sees no /sys/fs/selinux at all, which is what libselinux tests for.
    bool selinuxfs_hidden_ = false;
    // The host allows TCGETS but refuses the termios2 ioctls on its pseudo-terminals (an Android
    // app domain): guests' TCGETS2/TCSETS*2 are then made as TCGETS/TCSETS* (termios2.hpp).
    bool termios2_refused_ = false;
    // The supervisor's own process runs under a seccomp filter (an Android app's policy), which
    // every guest inherits: only then can a seccomp SIGSYS come from the host.
    bool host_policy_ = false;
    // Thread groups that installed a seccomp filter of their own (a sandboxing guest). A seccomp
    // SIGSYS in them may be their filter's, and is delivered to them unchanged.
    std::unordered_set<pid_t> guest_filtered_;
    // Tasks whose exit was reported, so a fork event that reaches the supervisor after the
    // child is gone does not bring it back (bounded; a live task of the same id is new).
    std::unordered_set<pid_t> exited_;

    // Shared with other threads.
    mutable std::mutex known_mu_;
    std::unordered_set<pid_t> known_;
    pid_t main_pid_ = -1;
    int main_pidfd_ = -1;
    bool main_reaped_ = false;
    std::atomic<std::uint32_t> cancel_reason_{0};

    std::mutex state_mu_;
    std::condition_variable state_cv_;
    enum class StartState { pending, ok, failed } start_state_ = StartState::pending;
    Status start_status_;
    bool done_ = false;
    core::ExitResult result_;

    int err_pipe_rd_ = -1;
    bool main_exited_ = false;
    core::ExitResult main_exit_;
    std::thread thread_;
    vdr::PtyDriver* pty_ = nullptr;
};

} // namespace vhdp::rootless
