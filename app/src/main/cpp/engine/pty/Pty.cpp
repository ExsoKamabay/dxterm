#include "Pty.h"

#include <pty.h>          // forkpty (bionic, API 23+)
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>     // umask
#include <sys/wait.h>
#include <cstdlib>
#include <cerrno>

namespace xterm {

Pty::~Pty() { close(); }

bool Pty::start(const std::vector<std::string>& argv,
                const std::vector<std::string>& env,
                const std::string& cwd,
                int cols, int rows) {
    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);
    ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);

    // Built BEFORE the fork, on purpose. Only the calling thread survives into the child, so a
    // malloc arena another thread happened to hold at fork time is locked there for ever: the
    // child hangs before exec and the terminal opens to nothing. This app always has other
    // threads running (a reader per workspace, the download and provisioning workers), so the
    // window is real. Everything the child does below, chdir/signal/sigprocmask/umask/execve, is async-signal-safe.
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    std::vector<char*> cenv;
    cenv.reserve(env.size() + 1);
    for (const auto& e : env) cenv.push_back(const_cast<char*>(e.c_str()));
    cenv.push_back(nullptr);

    int master = -1;
    pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) return false;

    if (pid == 0) {
        // ---- child ----
        if (!cwd.empty()) { if (chdir(cwd.c_str()) != 0) { /* fall through */ } }

        // Hand the guest a clean signal state. Both halves matter, and neither is
        // undone by execve:
        //
        //   * Dispositions set to SIG_IGN survive an exec (only caught handlers are
        //     reset), and ART leaves SIGHUP ignored, so without this every process in
        //     the terminal silently ignored SIGHUP.
        //   * The signal MASK survives an exec outright, and ART blocks SIGQUIT,
        //     SIGUSR1 and SIGPIPE for its own use (stack dumps, GC). Inherited, that
        //     made those three undeliverable everywhere in the guest: `trap ... USR1`
        //     never fired, the Ctrl-backslash quit key did nothing, and a writer whose pipe closed got an
        //     EPIPE error instead of dying quietly.
        //
        // Reset every signal rather than the handful we know about: the parent is a
        // whole Android runtime, and which signals it claims is not ours to track.
        for (int s = 1; s < NSIG; ++s) {
            if (s == SIGKILL || s == SIGSTOP) continue;   // cannot be changed
            signal(s, SIG_DFL);
        }
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);

        // The file-creation mask of a Linux login (login.defs, pam_umask) instead of the 077 every
        // Android app process runs with. Under 077 each file the guest creates is private to its
        // owner: `dpkg-deb --build` refuses the 0700 DEBIAN directory that results, and files a
        // package script writes cannot be read by the service users it writes them for.
        umask(022);

        execve(cargv[0], cargv.data(), cenv.data());
        _exit(127);   // exec failed
    }

    // ---- parent ----
    masterFd_ = master;
    pid_ = pid;
    // Non-blocking would complicate the read loop; keep blocking + poll in Session.
    return true;
}

ssize_t Pty::readMaster(uint8_t* buf, size_t len) {
    if (masterFd_ < 0) return -1;
    return ::read(masterFd_, buf, len);
}

ssize_t Pty::writeMaster(const uint8_t* buf, size_t len) {
    if (masterFd_ < 0) return -1;
    return ::write(masterFd_, buf, len);
}

void Pty::resize(int cols, int rows) {
    if (masterFd_ < 0) return;
    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);
    ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);
    ioctl(masterFd_, TIOCSWINSZ, &ws);
}

void Pty::close() {
    if (masterFd_ >= 0) { ::close(masterFd_); masterFd_ = -1; }
    if (pid_ > 0) {
        // Ask the child to exit (SIGHUP), poll briefly for it to reap, then force
        // it with SIGKILL and block until reaped so no zombie is left behind.
        kill(pid_, SIGHUP);
        int status = 0;
        for (int i = 0; i < 20; ++i) {          // up to ~100ms
            pid_t r = waitpid(pid_, &status, WNOHANG);
            if (r == pid_ || (r < 0 && errno == ECHILD)) { pid_ = -1; return; }
            usleep(5000);
        }
        kill(pid_, SIGKILL);
        waitpid(pid_, &status, 0);              // blocking reap
        pid_ = -1;
    }
}

} // namespace xterm
