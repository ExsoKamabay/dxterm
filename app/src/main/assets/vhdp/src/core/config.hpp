// Session configuration: mutable builder (SessionConfig) and the validated,
// canonicalised, immutable form (ValidatedConfig) that engines consume.
#pragma once

#include "common/status.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vhdp::core {

enum class EngineKind : std::uint32_t {
    auto_select = 0,
    rootless = 1,
    rooted = 2,
    emulator = 3,
    vm = 4
};
enum class NetworkPolicy : std::uint32_t { host = 0, none = 1 };
enum class StdioMode : std::uint32_t { inherit = 0, pty = 1, fds = 2 };
enum class SeccompMode : std::uint32_t { auto_select = 0, on = 1, off = 2 };
enum class DevMode : std::uint32_t { minimal = 0, none = 1 };
enum class ProcMode : std::uint32_t { none = 0, host = 1 };

const char* engine_name(EngineKind k) noexcept;

struct BindSpec {
    std::string host;
    std::string guest;
    bool read_write = false;
};

using OutputFn = void (*)(void* user, std::uint32_t stream, const std::uint8_t* data,
                          std::size_t len);

inline constexpr std::uint64_t kMaxTimeoutMs = 366ull * 24 * 3600 * 1000;
inline constexpr std::size_t kMaxArgs = 4096;
inline constexpr std::size_t kMaxEnv = 4096;
inline constexpr std::size_t kMaxBinds = 256;
inline constexpr std::size_t kMaxArgBytes = 256 * 1024;

struct SessionConfig {
    std::string rootfs;
    EngineKind engine = EngineKind::auto_select;
    std::string cwd = "/";
    std::vector<std::string> argv;
    std::vector<std::string> env; // KEY=VALUE overrides, in order
    bool clear_env = false;
    std::vector<BindSpec> binds;
    bool read_only_rootfs = false;
    NetworkPolicy network = NetworkPolicy::host;
    std::optional<std::uint32_t> uid;
    std::optional<std::uint32_t> gid;
    std::uint64_t timeout_ms = 0;
    bool trace = false;
    StdioMode stdio = StdioMode::inherit;
    int stdio_fds[3] = {-1, -1, -1};
    std::uint16_t pty_rows = 24;
    std::uint16_t pty_cols = 80;
    OutputFn output_fn = nullptr;
    void* output_user = nullptr;
    SeccompMode seccomp = SeccompMode::auto_select;
    DevMode dev = DevMode::minimal;
    ProcMode proc = ProcMode::none;
};

// Environment variables copied from the host when the guest environment is not
// cleared. Nothing else from the host environment reaches the guest.
inline constexpr const char* kHostEnvAllowlist[] = {"TERM", "COLORTERM", "LANG", "LC_ALL", "TZ"};

struct ValidatedConfig {
    SessionConfig cfg;                  // canonical paths
    std::vector<std::string> final_env; // environment handed to the guest
};

// Reads a host environment variable; injectable for tests.
using HostEnvLookup = std::function<std::optional<std::string>(const char*)>;

Result<std::shared_ptr<const ValidatedConfig>> validate_config(const SessionConfig& in,
                                                               const HostEnvLookup& host_env);

// Checks "KEY=VALUE" syntax (non-empty key without '=' or NUL).
bool valid_env_entry(std::string_view kv) noexcept;

// Checks a guest path for bind/cwd use: absolute, no NUL, no "..", < PATH_MAX.
Status check_guest_path(std::string_view p, const char* what);

} // namespace vhdp::core
