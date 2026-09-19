#include "core/config.hpp"

#include "common/strings.hpp"
#include "vfs/guest_path.hpp"

#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <set>

namespace vhdp::core {

const char* engine_name(EngineKind k) noexcept {
    switch (k) {
        case EngineKind::auto_select:
            return "auto";
        case EngineKind::rootless:
            return "rootless";
        case EngineKind::rooted:
            return "rooted";
        case EngineKind::emulator:
            return "emulator";
        case EngineKind::vm:
            return "vm";
    }
    return "unknown";
}

bool valid_env_entry(std::string_view kv) noexcept {
    std::size_t eq = kv.find('=');
    return eq != std::string_view::npos && eq > 0 && !contains_nul(kv);
}

Status check_guest_path(std::string_view p, const char* what) {
    if (p.empty() || contains_nul(p)) {
        return {Code::invalid_argument,
                std::string(what) + " must be a non-empty path without NUL bytes"};
    }
    if (!vfs::is_absolute(p)) {
        return {Code::invalid_argument,
                std::string(what) + " must be an absolute guest path: " + std::string(p)};
    }
    if (p.size() >= vfs::kPathMax) {
        return {Code::invalid_argument, std::string(what) + " is longer than PATH_MAX"};
    }
    if (vfs::has_dotdot(p)) {
        return {Code::invalid_argument,
                std::string(what) + " must not contain '..': " + std::string(p)};
    }
    return Status::ok();
}

namespace {

Result<std::string> canonical_dir(const std::string& path, Code code, const char* what) {
    if (path.empty() || contains_nul(path)) {
        return Status{code, std::string(what) + " path is empty or contains NUL"};
    }
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf) == nullptr) {
        return errno_status(code, std::string(what) + " '" + path + "'", errno);
    }
    return std::string(buf);
}

void set_env(std::vector<std::string>& env, std::string_view kv) {
    std::string_view key = kv.substr(0, kv.find('='));
    for (auto& e : env) {
        if (e.size() > key.size() && e.compare(0, key.size(), key) == 0 && e[key.size()] == '=') {
            e.assign(kv);
            return;
        }
    }
    env.emplace_back(kv);
}

} // namespace

Result<std::shared_ptr<const ValidatedConfig>> validate_config(const SessionConfig& in,
                                                               const HostEnvLookup& host_env) {
    auto out = std::make_shared<ValidatedConfig>();
    SessionConfig& c = out->cfg;
    c = in;

    // rootfs
    if (in.rootfs.empty()) {
        return Status{Code::rootfs_invalid, "no rootfs directory configured"};
    }
    auto root = canonical_dir(in.rootfs, Code::rootfs_invalid, "rootfs");
    if (!root.is_ok()) {
        return std::move(root).take_status();
    }
    c.rootfs = root.value();
    struct stat st{};
    if (::stat(c.rootfs.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return Status{Code::rootfs_invalid, "rootfs is not a directory: " + c.rootfs};
    }
    if (c.rootfs == "/") {
        return Status{
            Code::rootfs_invalid,
            "rootfs resolves to the host root '/'; point phdp at a root filesystem directory"};
    }

    if (auto s = check_guest_path(in.cwd, "cwd"); !s.is_ok()) {
        return s;
    }
    c.cwd = vfs::normalize_lexical(in.cwd);

    // argv
    if (in.argv.size() > kMaxArgs) {
        return Status{Code::invalid_argument, "too many command arguments"};
    }
    std::size_t bytes = 0;
    for (const auto& a : in.argv) {
        if (contains_nul(a)) {
            return Status{Code::invalid_argument, "command argument contains a NUL byte"};
        }
        bytes += a.size() + 1;
    }
    if (!in.argv.empty() && in.argv.front().empty()) {
        return Status{Code::invalid_argument, "command name is empty"};
    }

    // environment
    if (in.env.size() > kMaxEnv) {
        return Status{Code::invalid_argument, "too many environment entries"};
    }
    std::vector<std::string> env;
    if (!in.clear_env) {
        set_env(env, "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
        bool guest_root = in.uid.has_value() && *in.uid == 0;
        set_env(env, guest_root ? "HOME=/root" : "HOME=/");
        if (guest_root) {
            set_env(env, "USER=root");
            set_env(env, "LOGNAME=root");
        }
        if (host_env) {
            for (const char* name : kHostEnvAllowlist) {
                if (auto v = host_env(name); v && !contains_nul(*v)) {
                    set_env(env, std::string(name) + "=" + *v);
                }
            }
        }
    }
    for (const auto& kv : in.env) {
        if (!valid_env_entry(kv)) {
            return Status{Code::invalid_argument,
                          "invalid environment entry (expected KEY=VALUE): " + kv};
        }
        set_env(env, kv);
    }
    for (const auto& e : env) {
        bytes += e.size() + 1;
    }
    if (bytes > kMaxArgBytes) {
        return Status{Code::invalid_argument, "command arguments and environment exceed 256 KiB"};
    }
    out->final_env = std::move(env);

    // binds
    if (in.binds.size() > kMaxBinds) {
        return Status{Code::bind_invalid, "too many binds"};
    }
    std::set<std::string> guests;
    c.binds.clear();
    for (const auto& b : in.binds) {
        if (auto s = check_guest_path(b.guest, "bind guest path"); !s.is_ok()) {
            return Status{Code::bind_invalid, s.message()};
        }
        std::string guest = vfs::normalize_lexical(b.guest);
        if (guest == "/") {
            return Status{Code::bind_invalid, "cannot bind over the guest root '/'"};
        }
        if (in.proc == ProcMode::host && vfs::is_below(guest, "/proc")) {
            return Status{Code::bind_invalid,
                          "bind target " + guest + " conflicts with --proc host"};
        }
        if (!guests.insert(guest).second) {
            return Status{Code::bind_invalid, "duplicate bind target: " + guest};
        }
        auto host = canonical_dir(b.host, Code::bind_invalid, "bind source");
        if (!host.is_ok()) {
            return std::move(host).take_status();
        }
        c.binds.push_back(BindSpec{host.value(), guest, b.read_write});
    }

    if (in.uid && *in.uid == UINT32_MAX) {
        return Status{Code::invalid_argument, "uid 4294967295 is reserved"};
    }
    if (in.gid && *in.gid == UINT32_MAX) {
        return Status{Code::invalid_argument, "gid 4294967295 is reserved"};
    }
    if (in.timeout_ms > kMaxTimeoutMs) {
        return Status{Code::invalid_argument, "timeout is longer than one year"};
    }

    switch (in.stdio) {
        case StdioMode::inherit:
            break;
        case StdioMode::pty:
            if (in.pty_rows == 0 || in.pty_cols == 0) {
                return Status{Code::invalid_argument, "PTY rows and columns must be non-zero"};
            }
            break;
        case StdioMode::fds:
            for (int fd : in.stdio_fds) {
                if (fd < 0 || ::fcntl(fd, F_GETFD) < 0) {
                    return Status{Code::invalid_argument,
                                  "VHDP_STDIO_FDS requires three open file descriptors"};
                }
            }
            break;
        default:
            return Status{Code::invalid_argument, "unknown stdio mode"};
    }
    if (in.output_fn != nullptr && in.stdio != StdioMode::pty) {
        return Status{Code::invalid_argument, "an output callback requires VHDP_STDIO_PTY"};
    }
    return std::shared_ptr<const ValidatedConfig>(std::move(out));
}

} // namespace vhdp::core
