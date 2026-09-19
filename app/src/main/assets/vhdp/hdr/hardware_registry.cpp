#include "hdr/hardware_registry.hpp"

#include "arch/regs.hpp"

#include <unistd.h>

namespace vhdp::hdr {

const char* provision_name(Provision p) noexcept {
    switch (p) {
        case Provision::host_passthrough:
            return "host-passthrough";
        case Provision::projected:
            return "projected";
        case Provision::emulated:
            return "emulated";
        case Provision::absent:
            return "absent";
    }
    return "unknown";
}

std::vector<HardwareDescriptor> hardware_registry() {
    long page = ::sysconf(_SC_PAGESIZE);
    long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
    return {
        {"cpu",
         std::string("guest code runs natively on the host CPU (") + arch::arch_name() + ", " +
             std::to_string(cpus) + " online)",
         Provision::host_passthrough, "engine:rootless",
         "same-architecture only; no CPU feature masking"},
        {"memory", "host virtual memory and page size " + std::to_string(page) + " bytes",
         Provision::host_passthrough, "engine:rootless",
         "no memory limit is applied by this milestone"},
        {"clock", "CLOCK_* and time syscalls", Provision::host_passthrough, "syscall table",
         "guest cannot set the host clock (kernel returns EPERM)"},
        {"entropy", "getrandom and /dev/random, /dev/urandom", Provision::projected,
         "vdr:dev-minimal", "host kernel entropy"},
        {"null-zero-full", "/dev/null, /dev/zero, /dev/full", Provision::projected,
         "vdr:dev-minimal", ""},
        {"terminal", "PTY or inherited terminal", Provision::projected, "vdr:pty",
         "window size forwarded with TIOCSWINSZ"},
        {"identity", "guest-visible uid/gid (--uid/--gid)", Provision::emulated, "syscall handlers",
         "get*id results and stat ownership of host-owned files are rewritten; host credentials "
         "never change"},
        {"uname", "kernel name/release/machine", Provision::host_passthrough, "syscall table",
         "hostname changes are denied (no UTS namespace)"},
        {"network", "host network stack", Provision::host_passthrough, "vdr:network-policy",
         "--network none refuses IP sockets"},
        {"gpu", "GPU/display", Provision::absent, "-", "no GPU passthrough or acceleration"},
        {"usb-block", "USB and raw block devices", Provision::absent, "-", "not projected"},
    };
}

} // namespace vhdp::hdr
