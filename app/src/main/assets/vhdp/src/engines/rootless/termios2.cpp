#include "engines/rootless/termios2.hpp"

// Kernel structures and request numbers only; see the header for why <termios.h> stays out.
#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

namespace vhdp::rootless::termios2 {

namespace {

constexpr std::uint32_t kGet2 = static_cast<std::uint32_t>(TCGETS2);
constexpr std::uint32_t kSet2 = static_cast<std::uint32_t>(TCSETS2);
constexpr std::uint32_t kSetW2 = static_cast<std::uint32_t>(TCSETSW2);
constexpr std::uint32_t kSetF2 = static_cast<std::uint32_t>(TCSETSF2);

constexpr std::uint32_t kCbaud = static_cast<std::uint32_t>(CBAUD);
constexpr std::uint32_t kCbaudEx = static_cast<std::uint32_t>(CBAUDEX);
constexpr std::uint32_t kBother = static_cast<std::uint32_t>(BOTHER);

// The kernel's baud table (drivers/tty/tty_baudrate.c): Bxxx codes 0..15, then the CBAUDEX
// codes 1..15 continuing at index 16.
constexpr std::uint32_t kBaud[] = {
    0,       50,      75,      110,     134,     150,     200,     300,     600,     1200, 1800,
    2400,    4800,    9600,    19200,   38400,   57600,   115200,  230400,  460800,  500000,
    576000,  921600,  1000000, 1152000, 1500000, 2000000, 2500000, 3000000, 3500000, 4000000,
};

std::uint32_t code_to_speed(std::uint32_t code) noexcept {
    code &= kCbaud;
    if (code == kBother) {
        return 0; // an arbitrary rate lives only in the termios2 speed fields
    }
    std::uint32_t index = code;
    if ((code & kCbaudEx) != 0) {
        index = (code & ~kCbaudEx) + 15;
    }
    return index < sizeof(kBaud) / sizeof(kBaud[0]) ? kBaud[index] : 0;
}

long ioctl_call(int fd, std::uint32_t request, void* arg) noexcept {
    return ::syscall(SYS_ioctl, fd, static_cast<unsigned long>(request), arg);
}

} // namespace

std::size_t cflag_offset() noexcept {
    return offsetof(struct termios2, c_cflag);
}

std::size_t ispeed_offset() noexcept {
    static_assert(offsetof(struct termios2, c_ospeed) ==
                      offsetof(struct termios2, c_ispeed) + sizeof(speed_t),
                  "c_ospeed must follow c_ispeed");
    static_assert(sizeof(struct termios2) == sizeof(struct termios) + 2 * sizeof(speed_t),
                  "termios2 must be termios followed by the two speeds");
    return offsetof(struct termios2, c_ispeed);
}

std::array<std::uint32_t, 4> requests() noexcept {
    return {kGet2, kSet2, kSetW2, kSetF2};
}

std::uint32_t classic_request(std::uint32_t request) noexcept {
    if (request == kGet2) {
        return static_cast<std::uint32_t>(TCGETS);
    }
    if (request == kSet2) {
        return static_cast<std::uint32_t>(TCSETS);
    }
    if (request == kSetW2) {
        return static_cast<std::uint32_t>(TCSETSW);
    }
    if (request == kSetF2) {
        return static_cast<std::uint32_t>(TCSETSF);
    }
    return 0;
}

bool is_get(std::uint32_t request) noexcept {
    return request == kGet2;
}

std::uint32_t output_speed(std::uint32_t cflag) noexcept {
    return code_to_speed(cflag);
}

std::uint32_t input_speed(std::uint32_t cflag) noexcept {
    // A zero input code means "same as the output speed", as tty_termios_input_baud_rate() has it.
    std::uint32_t code = (cflag >> IBSHIFT) & kCbaud;
    return code == 0 ? output_speed(cflag) : code_to_speed(code);
}

bool host_refuses() noexcept {
    int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        return false;
    }
    bool refused = false;
    char name[128];
    if (::grantpt(master) == 0 && ::unlockpt(master) == 0 &&
        ::ptsname_r(master, name, sizeof(name)) == 0) {
        int slave = ::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (slave >= 0) {
            struct termios classic {};
            struct termios2 extended {};
            if (ioctl_call(slave, static_cast<std::uint32_t>(TCGETS), &classic) == 0 &&
                ioctl_call(slave, kGet2, &extended) != 0) {
                refused = errno == EACCES || errno == EPERM;
            }
            ::close(slave);
        }
    }
    ::close(master);
    return refused;
}

} // namespace vhdp::rootless::termios2
