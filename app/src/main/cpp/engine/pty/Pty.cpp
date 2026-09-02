#include "Pty.h"

#include <pty.h>          // forkpty (bionic, API 23+)
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
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
    // window is real. Everything the child does below, chdir/signal/execve, is async-signal-safe.
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

        // Reset signal dispositions the parent may have touched.
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);

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
