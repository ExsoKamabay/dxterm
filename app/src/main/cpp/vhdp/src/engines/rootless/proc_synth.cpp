#include "engines/rootless/proc_synth.hpp"

#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace vhdp::rootless {

namespace {

constexpr std::uint64_t kUserHz = 100; // USER_HZ, the tick unit of /proc/stat

double boottime_seconds() {
    timespec ts{};
    ::clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// A version-4 UUID in the text form /proc/sys/kernel/random/* uses, from 16 bytes.
std::string format_uuid(unsigned char (&b)[16]) {
    b[6] = static_cast<unsigned char>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3f) | 0x80);
    char s[40];
    std::snprintf(s, sizeof(s),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", b[0],
                  b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
                  b[14], b[15]);
    return s;
}

std::uint64_t splitmix64(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// random/uuid: a fresh random UUID on every read, as the kernel gives.
std::string synth_random_uuid() {
    unsigned char b[16] = {};
    long n = ::syscall(SYS_getrandom, b, sizeof(b), 0);
    if (n != static_cast<long>(sizeof(b))) {
        int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0 || ::read(fd, b, sizeof(b)) != static_cast<ssize_t>(sizeof(b))) {
            timespec ts{};
            ::clock_gettime(CLOCK_MONOTONIC, &ts);
            std::uint64_t st = static_cast<std::uint64_t>(ts.tv_nsec) ^
                               (static_cast<std::uint64_t>(ts.tv_sec) << 32) ^
                               static_cast<std::uint64_t>(::getpid());
            for (int i = 0; i < 16; i += 8) {
                std::uint64_t v = splitmix64(st);
                for (int k = 0; k < 8; ++k) {
                    b[i + k] = static_cast<unsigned char>(v >> (8 * k));
                }
            }
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }
    return format_uuid(b);
}

// random/boot_id: one UUID for the whole session, so a program that reads it twice (a shell
// prompt does, on every prompt) is not told the machine rebooted in between. The host keeps its
// own boot_id to itself; the boot instant is what every process can work out (the realtime clock
// minus the time since boot), rounded to a minute. Computed once and kept: the two clock reads
// are not one operation, so a value at a minute boundary would otherwise flip between reads, and
// a clock step (NTP settling after boot) would move it as well.
std::string synth_boot_id() {
    static const std::string id = [] {
        timespec rt{};
        timespec bt{};
        ::clock_gettime(CLOCK_REALTIME, &rt);
        ::clock_gettime(CLOCK_BOOTTIME, &bt);
        std::int64_t boot_minute = (static_cast<std::int64_t>(rt.tv_sec) - bt.tv_sec + 30) / 60;
        std::uint64_t st = static_cast<std::uint64_t>(boot_minute) ^ 0x7668647062696421ull;
        unsigned char b[16];
        for (int i = 0; i < 16; i += 8) {
            std::uint64_t v = splitmix64(st);
            for (int k = 0; k < 8; ++k) {
                b[i + k] = static_cast<unsigned char>(v >> (8 * k));
            }
        }
        return format_uuid(b);
    }();
    return id;
}

std::string read_small(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// "0-3,6" -> {0,1,2,3,6}
std::vector<int> parse_cpu_list(const std::string& text) {
    std::vector<int> out;
    std::stringstream ss(text);
    std::string part;
    while (std::getline(ss, part, ',')) {
        int lo = 0;
        int hi = 0;
        if (std::sscanf(part.c_str(), "%d-%d", &lo, &hi) == 2) {
            for (int c = lo; c <= hi && c < 4096; ++c) {
                out.push_back(c);
            }
        } else if (std::sscanf(part.c_str(), "%d", &lo) == 1) {
            out.push_back(lo);
        }
    }
    return out;
}

std::vector<int> online_cpus() {
    std::vector<int> cpus = parse_cpu_list(read_small("/sys/devices/system/cpu/online"));
    if (cpus.empty()) {
        long n = ::sysconf(_SC_NPROCESSORS_ONLN);
        for (long c = 0; c < (n > 0 ? n : 1); ++c) {
            cpus.push_back(static_cast<int>(c));
        }
    }
    return cpus;
}

// Idle residency of one CPU in microseconds, summed over its cpuidle states; nullopt when
// sysfs does not expose it.
std::optional<std::uint64_t> cpu_idle_us(int cpu) {
    std::uint64_t total = 0;
    bool any = false;
    for (int state = 0; state < 16; ++state) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpuidle/state" +
                           std::to_string(state) + "/time";
        std::ifstream in(path);
        std::uint64_t v = 0;
        if (!(in >> v)) {
            break;
        }
        total += v;
        any = true;
    }
    return any ? std::optional<std::uint64_t>(total) : std::nullopt;
}

std::map<std::string, std::uint64_t> meminfo_kb() {
    std::map<std::string, std::uint64_t> out;
    std::ifstream in("/proc/meminfo");
    std::string line;
    while (std::getline(in, line)) {
        char name[64];
        unsigned long long v = 0;
        if (std::sscanf(line.c_str(), "%63[^:]: %llu", name, &v) == 2) {
            out[name] = v;
        }
    }
    return out;
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    int n = std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    if (n < 0) {
        return {};
    }
    auto len = static_cast<std::size_t>(n);
    return std::string(buf, len < sizeof(buf) ? len : sizeof(buf) - 1);
}

std::string synth_uptime() {
    double up = boottime_seconds();
    double idle = 0;
    for (int cpu : online_cpus()) {
        if (auto us = cpu_idle_us(cpu)) {
            idle += static_cast<double>(*us) / 1e6;
        }
    }
    return fmt("%.2f %.2f\n", up, idle);
}

std::string synth_loadavg(pid_t last_pid) {
    struct sysinfo si{};
    double l[3] = {0, 0, 0};
    unsigned procs = 0;
    if (::sysinfo(&si) == 0) {
        for (int i = 0; i < 3; ++i) {
            l[i] = static_cast<double>(si.loads[i]) / 65536.0;
        }
        procs = si.procs;
    }
    return fmt("%.2f %.2f %.2f 1/%u %d\n", l[0], l[1], l[2], procs, static_cast<int>(last_pid));
}

std::string synth_stat(pid_t last_pid) {
    double up = boottime_seconds();
    auto total_ticks = static_cast<std::uint64_t>(up * static_cast<double>(kUserHz));
    std::vector<int> cpus = online_cpus();
    std::uint64_t sum_user = 0;
    std::uint64_t sum_idle = 0;
    std::string lines;
    for (int cpu : cpus) {
        std::uint64_t idle = total_ticks; // without cpuidle data the CPU is reported idle
        if (auto us = cpu_idle_us(cpu)) {
            idle = *us / (1000000u / kUserHz);
            if (idle > total_ticks) {
                idle = total_ticks;
            }
        }
        std::uint64_t user = total_ticks - idle;
        sum_user += user;
        sum_idle += idle;
        lines += fmt("cpu%d %" PRIu64 " 0 0 %" PRIu64 " 0 0 0 0 0 0\n", cpu, user, idle);
    }
    timespec rt{};
    ::clock_gettime(CLOCK_REALTIME, &rt);
    auto btime = static_cast<long long>(static_cast<double>(rt.tv_sec) - up);
    struct sysinfo si{};
    unsigned procs = ::sysinfo(&si) == 0 ? si.procs : 0;
    std::string out = fmt("cpu  %" PRIu64 " 0 0 %" PRIu64 " 0 0 0 0 0 0\n", sum_user, sum_idle);
    out += lines;
    out += "intr 0\nctxt 0\n";
    out += fmt("btime %lld\nprocesses %d\nprocs_running 1\nprocs_blocked 0\n", btime,
               static_cast<int>(last_pid > 0 ? last_pid : static_cast<pid_t>(procs)));
    out += "softirq 0 0 0 0 0 0 0 0 0 0 0\n";
    return out;
}

std::string synth_version() {
    utsname u{};
    if (::uname(&u) != 0) {
        return "Linux version unknown\n";
    }
    return std::string(u.sysname) + " version " + u.release + " (unknown@unknown) (unknown) " +
           u.version + "\n";
}

std::string synth_filesystems() {
    std::set<std::string> nodev = {"sysfs", "tmpfs", "proc", "devpts", "cgroup", "cgroup2",
                                   "securityfs", "debugfs", "tracefs", "pstore", "bpf",
                                   "configfs", "selinuxfs", "functionfs", "binder", "fuse"};
    std::set<std::string> block = {"ext4", "f2fs", "erofs", "vfat", "exfat"};
    std::ifstream in("/proc/self/mounts");
    std::string src;
    std::string dst;
    std::string type;
    std::string rest;
    while (in >> src >> dst >> type && std::getline(in, rest)) {
        (src.rfind("/dev/", 0) == 0 ? block : nodev).insert(type);
    }
    std::string out;
    for (const auto& t : nodev) {
        if (block.count(t) == 0) {
            out += "nodev\t" + t + "\n";
        }
    }
    for (const auto& t : block) {
        out += "\t" + t + "\n";
    }
    return out;
}

std::string synth_swaps() {
    std::string out = "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n";
    struct sysinfo si{};
    if (::sysinfo(&si) == 0 && si.totalswap > 0) {
        std::uint64_t unit = si.mem_unit > 0 ? si.mem_unit : 1;
        std::uint64_t size_kb = static_cast<std::uint64_t>(si.totalswap) * unit / 1024;
        std::uint64_t used_kb =
            static_cast<std::uint64_t>(si.totalswap - si.freeswap) * unit / 1024;
        out += fmt("/dev/block/zram0\t\t\t\tpartition\t%" PRIu64 "\t\t%" PRIu64 "\t\t-2\n",
                   size_kb, used_kb);
    }
    return out;
}

std::string synth_vmstat() {
    auto mi = meminfo_kb();
    long page_kb = ::sysconf(_SC_PAGESIZE) / 1024;
    if (page_kb <= 0) {
        page_kb = 4;
    }
    auto pages = [&](const char* key) {
        auto it = mi.find(key);
        return it == mi.end() ? std::uint64_t{0} : it->second / static_cast<std::uint64_t>(page_kb);
    };
    std::string out;
    out += fmt("nr_free_pages %" PRIu64 "\n", pages("MemFree"));
    out += fmt("nr_inactive_anon %" PRIu64 "\n", pages("Inactive(anon)"));
    out += fmt("nr_active_anon %" PRIu64 "\n", pages("Active(anon)"));
    out += fmt("nr_inactive_file %" PRIu64 "\n", pages("Inactive(file)"));
    out += fmt("nr_active_file %" PRIu64 "\n", pages("Active(file)"));
    out += fmt("nr_mapped %" PRIu64 "\n", pages("Mapped"));
    out += fmt("nr_dirty %" PRIu64 "\n", pages("Dirty"));
    out += fmt("nr_writeback %" PRIu64 "\n", pages("Writeback"));
    for (const char* k : {"pgpgin", "pgpgout", "pswpin", "pswpout", "pgfault", "pgmajfault"}) {
        out += std::string(k) + " 0\n";
    }
    return out;
}

} // namespace

bool has_proc_stand_in(std::string_view rel) noexcept {
    for (std::string_view name : {"uptime", "loadavg", "stat", "version", "filesystems", "swaps",
                                  "vmstat", "sys/kernel/hostname", "sys/kernel/osrelease",
                                  "sys/kernel/ostype", "sys/kernel/pid_max",
                                  "sys/kernel/random/boot_id", "sys/kernel/random/uuid"}) {
        if (rel == name) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> synth_proc_global(std::string_view rel, pid_t last_pid) {
    if (rel == "uptime") {
        return synth_uptime();
    }
    if (rel == "loadavg") {
        return synth_loadavg(last_pid);
    }
    if (rel == "stat") {
        return synth_stat(last_pid);
    }
    if (rel == "version") {
        return synth_version();
    }
    if (rel == "filesystems") {
        return synth_filesystems();
    }
    if (rel == "swaps") {
        return synth_swaps();
    }
    if (rel == "vmstat") {
        return synth_vmstat();
    }
    utsname u{};
    bool have_uname = ::uname(&u) == 0;
    if (rel == "sys/kernel/hostname") {
        return std::string(have_uname ? u.nodename : "localhost") + "\n";
    }
    if (rel == "sys/kernel/osrelease") {
        return std::string(have_uname ? u.release : "unknown") + "\n";
    }
    if (rel == "sys/kernel/ostype") {
        return std::string("Linux\n");
    }
    if (rel == "sys/kernel/random/boot_id") {
        return synth_boot_id();
    }
    if (rel == "sys/kernel/random/uuid") {
        return synth_random_uuid();
    }
    if (rel == "sys/kernel/pid_max") {
        return std::string(last_pid >= 32768 ? "4194304\n" : "32768\n");
    }
    return std::nullopt;
}

std::string rewrite_status_ids(const std::string& status, const StatusIds& ids) {
    std::string out;
    out.reserve(status.size() + 32);
    std::size_t pos = 0;
    while (pos < status.size()) {
        std::size_t nl = status.find('\n', pos);
        std::size_t end = nl == std::string::npos ? status.size() : nl;
        std::string_view line(status.data() + pos, end - pos);
        if (line.rfind("Uid:", 0) == 0) {
            out += fmt("Uid:\t%u\t%u\t%u\t%u", ids.uid[0], ids.uid[1], ids.uid[2], ids.uid[3]);
        } else if (line.rfind("Gid:", 0) == 0) {
            out += fmt("Gid:\t%u\t%u\t%u\t%u", ids.gid[0], ids.gid[1], ids.gid[2], ids.gid[3]);
        } else if (line.rfind("Groups:", 0) == 0) {
            out += "Groups:\t";
            for (std::uint32_t g : ids.groups) {
                out += std::to_string(g) + " ";
            }
        } else {
            out.append(line);
        }
        if (nl != std::string::npos) {
            out.push_back('\n');
        }
        pos = end + 1;
    }
    return out;
}

} // namespace vhdp::rootless
