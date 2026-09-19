#pragma once

#include <unistd.h>

#include <utility>

namespace vhdp {

// Owning file descriptor. Invariant: fd_ is either -1 or an open descriptor owned
// exclusively by this object; it is closed exactly once.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.fd_, -1));
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    ~UniqueFd() { reset(); }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

} // namespace vhdp
