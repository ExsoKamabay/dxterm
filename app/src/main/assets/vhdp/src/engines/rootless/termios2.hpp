// The termios2 terminal ioctls, for hosts that refuse them.
//
// glibc 2.42 reads and writes terminal settings with TCGETS2 and TCSETS2/TCSETSW2/TCSETSF2
// instead of TCGETS and TCSETS/TCSETSW/TCSETSF, so that arbitrary and split baud rates can be
// expressed. An Android app's SELinux policy lists the tty ioctls it may make on its own
// pseudo-terminals, and the termios2 ones are not on that list: they fail with EACCES while the
// original ones succeed. isatty() is tcgetattr() underneath, so under such a host every program
// built against glibc 2.42 decides it has no terminal -- bash starts non-interactive and never
// prints a prompt, readline, job control and full-screen programs all go without one.
//
// Both structures share their first bytes: termios2 is termios followed by the input and output
// speeds. When the host refuses termios2, the supervisor issues the original request on the
// same buffer and, for a read, fills in the two speeds the kernel would have written, from the
// baud-rate fields of c_cflag, as the kernel itself derives them. Only an arbitrary rate
// (BOTHER) is lost: it reads back as 0. A pseudo-terminal has no line speed to lose, and a host
// that refuses termios2 gives the guest no serial line where one would matter.
//
// Kept apart from the supervisor because the kernel's <asm/termbits.h> and the C library's
// <termios.h> define the same names and cannot share a translation unit.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vhdp::rootless::termios2 {

// Byte offsets inside the kernel's struct termios2.
std::size_t cflag_offset() noexcept;
std::size_t ispeed_offset() noexcept; // c_ispeed, immediately followed by c_ospeed

// TCGETS2, TCSETS2, TCSETSW2 and TCSETSF2 as this architecture numbers them.
std::array<std::uint32_t, 4> requests() noexcept;

// The original request doing the same job as `request`, or 0 when it is not a termios2 one.
std::uint32_t classic_request(std::uint32_t request) noexcept;
bool is_get(std::uint32_t request) noexcept;

// The speeds TCGETS2 reports for a c_cflag, in bits per second; 0 for a code without one.
std::uint32_t input_speed(std::uint32_t cflag) noexcept;
std::uint32_t output_speed(std::uint32_t cflag) noexcept;

// True when the host allows TCGETS but refuses TCGETS2 on a pseudo-terminal of its own, judged
// on a fresh pty pair (it carries the same label as the terminal a session runs on).
bool host_refuses() noexcept;

} // namespace vhdp::rootless::termios2
