#pragma once
#include <sys/types.h>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

namespace xterm {

// Owns a pseudo-terminal master fd and the child process running the shell.
class Pty {
public:
    Pty() = default;
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    // Fork a child on a new PTY and execve(argv[0], argv, env). Returns true on
    // success. cwd/argv/env are plain vectors of C-strings owned by the caller.
    bool start(const std::vector<std::string>& argv,
               const std::vector<std::string>& env,
               const std::string& cwd,
               int cols, int rows);

    ssize_t readMaster(uint8_t* buf, size_t len);
    ssize_t writeMaster(const uint8_t* buf, size_t len);
    void resize(int cols, int rows);
    void close();

    int masterFd() const { return masterFd_; }
    bool alive() const { return masterFd_ >= 0; }

    // How the shell ended, for the log line the session writes when the terminal closes
    // itself. Reaps the child without blocking; -1 while it is still running or already
    // reaped. 127 is what the failed-exec child exits with, and that number is the whole
    // difference between "the user typed exit" and "the shell could never start".
    int reapStatus();

private:
    int masterFd_ = -1;
    pid_t pid_ = -1;
};

} // namespace xterm
