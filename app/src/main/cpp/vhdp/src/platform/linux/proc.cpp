#include "platform/linux/proc.hpp"

#include "common/unique_fd.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdlib>

namespace vhdp::platform {

Result<std::string> read_small_file(const std::string& path, std::size_t max_bytes) {
    UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY));
    if (!fd.valid()) {
        return errno_status(Code::io, "open " + path, errno);
    }
    std::string out;
    char buf[4096];
    while (out.size() < max_bytes) {
        ssize_t n = ::read(fd.get(), buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno_status(Code::io, "read " + path, errno);
        }
        if (n == 0) {
            break;
        }
        std::size_t take = static_cast<std::size_t>(n);
        if (out.size() + take > max_bytes) {
            take = max_bytes - out.size();
        }
        out.append(buf, take);
    }
    return out;
}

Result<std::string> read_link(const std::string& path) {
    char buf[PATH_MAX];
    ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf));
    if (n < 0) {
        return errno_status(Code::io, "readlink " + path, errno);
    }
    if (static_cast<std::size_t>(n) >= sizeof(buf)) {
        return Status{Code::io, "readlink " + path + ": target too long"};
    }
    return std::string(buf, static_cast<std::size_t>(n));
}

std::string trim_right(std::string s) {
    while (!s.empty() &&
           (s.back() == '\n' || s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

std::optional<std::string> proc_status_field(pid_t pid, const std::string& key) {
    std::string path = pid == 0 ? "/proc/self/status" : "/proc/" + std::to_string(pid) + "/status";
    auto content = read_small_file(path);
    if (!content.is_ok()) {
        return std::nullopt;
    }
    const std::string& text = content.value();
    std::string needle = key + ":";
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) {
            eol = text.size();
        }
        if (text.compare(pos, needle.size(), needle) == 0) {
            std::size_t v = pos + needle.size();
            while (v < eol && (text[v] == ' ' || text[v] == '\t')) {
                ++v;
            }
            return text.substr(v, eol - v);
        }
        pos = eol + 1;
    }
    return std::nullopt;
}

int count_open_fds() {
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr) {
        return -1;
    }
    int n = 0;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] != '.') {
            ++n;
        }
    }
    ::closedir(d);
    return n - 1; // the DIR's own descriptor
}

namespace {

// Parses "pid (comm) S ppid ..." robustly (comm may contain spaces/parentheses).
bool parse_stat(const std::string& stat, char& state, pid_t& ppid) {
    std::size_t close = stat.rfind(')');
    if (close == std::string::npos || close + 4 >= stat.size()) {
        return false;
    }
    state = stat[close + 2];
    char* end = nullptr;
    long p = std::strtol(stat.c_str() + close + 4, &end, 10);
    if (end == stat.c_str() + close + 4) {
        return false;
    }
    ppid = static_cast<pid_t>(p);
    return true;
}

} // namespace

std::vector<pid_t> child_pids(pid_t parent) {
    std::vector<pid_t> out;
    DIR* d = ::opendir("/proc");
    if (d == nullptr) {
        return out;
    }
    while (dirent* e = ::readdir(d)) {
        char* end = nullptr;
        long pid = std::strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end != '\0' || pid <= 0) {
            continue;
        }
        auto stat = read_small_file("/proc/" + std::string(e->d_name) + "/stat", 4096);
        if (!stat.is_ok()) {
            continue;
        }
        char state = 0;
        pid_t ppid = 0;
        if (parse_stat(stat.value(), state, ppid) && ppid == parent) {
            out.push_back(static_cast<pid_t>(pid));
        }
    }
    ::closedir(d);
    return out;
}

char process_state(pid_t pid) {
    auto stat = read_small_file("/proc/" + std::to_string(pid) + "/stat", 4096);
    if (!stat.is_ok()) {
        return '\0';
    }
    char state = 0;
    pid_t ppid = 0;
    return parse_stat(stat.value(), state, ppid) ? state : '\0';
}

} // namespace vhdp::platform
