#include "Session.h"
#include <poll.h>
#include <unistd.h>
#include <cerrno>

namespace xterm {

Session::Session(int cols, int rows) : term_(cols, rows) {}

Session::~Session() { stop(); }

bool Session::start(const std::vector<std::string>& argv,
                    const std::vector<std::string>& env,
                    const std::string& cwd) {
    int cols, rows;
    term_.dims(cols, rows);
    if (!pty_.start(argv, env, cwd, cols, rows)) return false;
    // Parser responses (DA/DSR) and mouse reports go straight back to the PTY.
    term_.setResponder([this](const std::string& s) {
        pty_.writeMaster(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    });
    running_.store(true);
    reader_ = std::thread([this] { readerLoop(); });
    return true;
}

void Session::readerLoop() {
    // 32 KiB (was 8 KiB): fewer read() syscalls per large burst (apt/curl/git output),
    // which is the only IO lever inside the app's control. Throughput of the network itself
    // is bound by proot's syscall emulation, DNS and the remote server, not by this buffer.
    uint8_t buf[32768];
    struct pollfd pfd{};
    pfd.fd = pty_.masterFd();
    pfd.events = POLLIN;

    while (running_.load()) {
        int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            // Drain anything still readable, then exit.
            ssize_t n = pty_.readMaster(buf, sizeof(buf));
            if (n > 0) { term_.feed(buf, static_cast<size_t>(n));
                         if (term_.takeDirty()) generation_.fetch_add(1, std::memory_order_relaxed); }
            break;
        }
        if (pfd.revents & POLLIN) {
            ssize_t n = pty_.readMaster(buf, sizeof(buf));
            if (n > 0) {
                term_.feed(buf, static_cast<size_t>(n));
                if (term_.takeDirty()) generation_.fetch_add(1, std::memory_order_relaxed);
            } else if (n == 0) {
                break;              // EOF: shell exited
            } else {
                if (errno == EINTR || errno == EAGAIN) continue;
                break;
            }
        }
    }
    running_.store(false);
    generation_.fetch_add(1, std::memory_order_relaxed);  // wake the UI to show exit
}

void Session::write(const uint8_t* data, size_t len) {
    if (running_.load()) pty_.writeMaster(data, len);
}

void Session::resize(int cols, int rows) {
    term_.resize(cols, rows);
    pty_.resize(cols, rows);
    generation_.fetch_add(1, std::memory_order_relaxed);
}

void Session::stop() {
    bool was = running_.exchange(false);
    if (was || pty_.alive()) {
        // JOIN FIRST, then close. Closing the master fd while the reader thread is inside
        // poll()/read() on it does not wake that poll (closing an fd never does on Linux), so it
        // buys no latency at all; what it does buy is the fd number being free for reuse the
        // instant it is closed. This process opens files on other threads constantly (the
        // download worker, the ollama watcher, ART itself), so the reader could come out of its
        // poll and read() from whatever unrelated descriptor had landed on that number.
        // running_ is already false above, so the loop exits within one poll timeout (200 ms),
        // which is the same wait the old order paid anyway.
        if (reader_.joinable()) reader_.join();
        pty_.close();
    }
}

} // namespace xterm
