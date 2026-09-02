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

private:
    int masterFd_ = -1;
    pid_t pid_ = -1;
};

} // namespace xterm
