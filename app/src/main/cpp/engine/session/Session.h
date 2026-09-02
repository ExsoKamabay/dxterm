#pragma once
#include "pty/Pty.h"
#include "terminal/Terminal.h"
#include <atomic>
#include <thread>
#include <vector>
#include <string>

namespace xterm {

// Ties a PTY to a Terminal and pumps output on a background reader thread.
// The UI polls generation() and re-snapshots only when it changes.
class Session {
public:
    Session(int cols, int rows);
    ~Session();

    bool start(const std::vector<std::string>& argv,
               const std::vector<std::string>& env,
               const std::string& cwd);

    void write(const uint8_t* data, size_t len);
    void resize(int cols, int rows);
    void stop();

    uint64_t generation() const { return generation_.load(std::memory_order_relaxed); }
    Terminal& terminal() { return term_; }
    bool running() const { return running_.load(); }

private:
    void readerLoop();

    Terminal term_;
    Pty pty_;
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> generation_{0};
};

} // namespace xterm
