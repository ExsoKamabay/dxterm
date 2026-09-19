#include "vdr/drivers.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>

namespace vhdp::vdr {

namespace {
thread_local std::uint64_t t_callback_session = 0;
thread_local bool t_callback_any = false;
} // namespace

CallbackScope::CallbackScope(std::uint64_t session_id) noexcept
    : previous_(t_callback_session), previous_any_(t_callback_any) {
    t_callback_session = session_id;
    t_callback_any = true;
}

CallbackScope::~CallbackScope() {
    t_callback_session = previous_;
    t_callback_any = previous_any_;
}

std::uint64_t CallbackScope::current() noexcept {
    return t_callback_session;
}

bool CallbackScope::inside_any() noexcept {
    return t_callback_any;
}

const std::vector<DriverDescriptor>& driver_catalogue() {
    static const std::vector<DriverDescriptor> k = {
        {"host-bind", "explicit host directory binds",
         "host read (and write for :rw) access to the bind source",
         "no binds by default; read-only unless :rw; canonicalised source; guest target must not "
         "contain '..'",
         "mapping discarded at session end"},
        {"dev-minimal", "projects /dev/null, zero, full, random, urandom, tty, ptmx and /dev/pts",
         "host device nodes must exist and be accessible to the calling user",
         "enabled by default (--dev minimal); no other device nodes are visible; --dev none "
         "disables",
         "mapping discarded at session end"},
        {"proc-host", "read-only projection of host /proc",
         "host procfs visibility of the calling user",
         "disabled by default (--proc none); /proc/self is remapped to the calling guest process; "
         "magic links to "
         "paths outside the session mounts fail closed with EACCES; exposes host process "
         "information",
         "mapping discarded at session end"},
        {"pty", "pseudo-terminal for the guest (library PTY mode and phdp --pty)", "/dev/ptmx",
         "guest becomes session leader with the PTY as controlling terminal; resize via TIOCSWINSZ",
         "master closed, output pump joined at session destroy"},
        {"network-policy", "host network pass-through or IP socket denial", "none",
         "--network host (default) passes sockets through; --network none refuses non-AF_UNIX "
         "socket() with EACCES "
         "(seccomp-enforced when available); abstract AF_UNIX sockets remain reachable",
         "none"},
    };
    return k;
}

std::vector<vfs::Mount> dev_minimal_mounts() {
    static constexpr const char* kNodes[] = {"/dev/null",   "/dev/zero",    "/dev/full",
                                             "/dev/random", "/dev/urandom", "/dev/tty",
                                             "/dev/ptmx"};
    std::vector<vfs::Mount> out;
    for (const char* n : kNodes) {
        struct stat st{};
        if (::stat(n, &st) == 0 && S_ISCHR(st.st_mode)) {
            out.push_back(vfs::Mount{n, n, false, vfs::MountKind::device, "vdr:dev-minimal"});
        }
    }
    struct stat st{};
    if (::stat("/dev/pts", &st) == 0 && S_ISDIR(st.st_mode)) {
        out.push_back(
            vfs::Mount{"/dev/pts", "/dev/pts", false, vfs::MountKind::device, "vdr:dev-minimal"});
    }
    return out;
}

vfs::Mount proc_host_mount() {
    return vfs::Mount{"/proc", "/proc", true, vfs::MountKind::proc, "vdr:proc-host"};
}

std::vector<vfs::Mount> host_bind_mounts(const std::vector<core::BindSpec>& binds) {
    std::vector<vfs::Mount> out;
    out.reserve(binds.size());
    for (const auto& b : binds) {
        out.push_back(
            vfs::Mount{b.guest, b.host, !b.read_write, vfs::MountKind::bind, "host-bind"});
    }
    return out;
}

vfs::MountTable build_mount_table(const core::ValidatedConfig& vc) {
    vfs::MountTable table(vc.cfg.rootfs, vc.cfg.read_only_rootfs);
    if (vc.cfg.dev == core::DevMode::minimal) {
        for (auto& m : dev_minimal_mounts()) {
            table.add(std::move(m));
        }
    }
    if (vc.cfg.proc == core::ProcMode::host) {
        table.add(proc_host_mount());
    }
    // Explicit binds are added last; longest-prefix matching gives an explicit
    // bind precedence over a projection only when its guest path is longer.
    for (auto& m : host_bind_mounts(vc.cfg.binds)) {
        table.add(std::move(m));
    }
    return table;
}

Result<std::unique_ptr<PtyDriver>> PtyDriver::open(std::uint16_t rows, std::uint16_t cols) {
    std::unique_ptr<PtyDriver> d(new PtyDriver());
    d->master_.reset(::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
    if (!d->master_.valid()) {
        return errno_status(Code::io, "posix_openpt", errno);
    }
    if (::grantpt(d->master_.get()) != 0 || ::unlockpt(d->master_.get()) != 0) {
        return errno_status(Code::io, "grantpt/unlockpt", errno);
    }
    char name[128];
    if (::ptsname_r(d->master_.get(), name, sizeof(name)) != 0) {
        return errno_status(Code::io, "ptsname_r", errno);
    }
    d->slave_.reset(::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC));
    if (!d->slave_.valid()) {
        return errno_status(Code::io, std::string("open ") + name, errno);
    }
    if (auto s = d->resize(rows, cols); !s.is_ok()) {
        return s;
    }
    return d;
}

PtyDriver::~PtyDriver() {
    stopping_.store(true);
    if (wake_.valid()) {
        std::uint64_t one = 1;
        ssize_t wr = ::write(wake_.get(), &one, sizeof(one));
        (void)wr;
    }
    if (pump_.joinable()) {
        pump_.join();
    }
}

Status PtyDriver::resize(std::uint16_t rows, std::uint16_t cols) {
    if (rows == 0 || cols == 0) {
        return {Code::invalid_argument, "PTY rows and columns must be non-zero"};
    }
    winsize ws{};
    ws.ws_row = rows;
    ws.ws_col = cols;
    if (::ioctl(master_.get(), TIOCSWINSZ, &ws) != 0) {
        return errno_status(Code::io, "TIOCSWINSZ", errno);
    }
    return Status::ok();
}

Status PtyDriver::write_input(const std::uint8_t* data, std::size_t len, std::size_t& written) {
    written = 0;
    while (written < len) {
        ssize_t n = ::write(master_.get(), data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN && written > 0) {
                break;
            }
            return errno_status(Code::io, "write to PTY master", errno);
        }
        written += static_cast<std::size_t>(n);
    }
    return Status::ok();
}

Status PtyDriver::start_pump(core::OutputFn fn, void* user, std::uint64_t session_id) {
    if (fn == nullptr || pump_.joinable()) {
        return Status::ok();
    }
    wake_.reset(::eventfd(0, EFD_CLOEXEC));
    if (!wake_.valid()) {
        return errno_status(Code::io, "eventfd", errno);
    }
    try {
        pump_ = std::thread([this, fn, user, session_id] { pump_main(fn, user, session_id); });
    } catch (...) {
        return {Code::no_memory, "cannot start PTY output thread"};
    }
    return Status::ok();
}

void PtyDriver::pump_main(core::OutputFn fn, void* user, std::uint64_t session_id) noexcept {
    std::uint8_t buf[16384];
    for (;;) {
        pollfd fds[2] = {{master_.get(), POLLIN, 0}, {wake_.get(), POLLIN, 0}};
        int r = ::poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            ssize_t n = ::read(master_.get(), buf, sizeof(buf));
            if (n > 0) {
                CallbackScope scope(session_id);
                fn(user, 1u, buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            return; // EOF or EIO: every slave descriptor is closed
        }
        if ((fds[1].revents & POLLIN) != 0 && stopping_.load()) {
            return;
        }
    }
}

void PtyDriver::finish_pump() noexcept {
    if (!pump_.joinable()) {
        return;
    }
    // The pump normally ends on EIO once the guest tree is gone; a descendant that
    // leaked the slave (impossible after tree kill, but defensive) is bounded.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd p{master_.get(), POLLIN, 0};
        if (::poll(&p, 1, 0) == 0) {
            break;
        }
        ::usleep(10000);
    }
    stopping_.store(true);
    std::uint64_t one = 1;
    ssize_t wr = ::write(wake_.get(), &one, sizeof(one));
    (void)wr;
    pump_.join();
}

} // namespace vhdp::vdr
