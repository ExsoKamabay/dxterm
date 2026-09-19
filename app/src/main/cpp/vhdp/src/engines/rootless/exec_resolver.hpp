// Guest program resolution for execve: shebang scripts, ELF ABI checks and
// dynamic-loader indirection.
//
// Why indirection: the kernel resolves PT_INTERP against the *host* root. For a
// dynamic guest ELF the supervisor therefore executes the guest's own loader
// (resolved inside the rootfs) and passes the program as an argument, e.g.
//   /rootfs/lib/ld-linux-x86-64.so.2 [--argv0 ARGV0] /usr/bin/prog ARGS...
// The loader then opens the program through translated open() calls.
// Consequence (documented): /proc/self/exe of such processes names the loader,
// and set-uid/file capabilities are never honoured (no_new_privs anyway).
#pragma once

#include "vfs/mount_table.hpp"
#include "vfs/resolver.hpp"

#include <sys/types.h>

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace vhdp::rootless {

inline constexpr int kMaxShebangDepth = 4;
inline constexpr std::size_t kShebangMax = 256; // BINPRM_BUF_SIZE

struct ExecPlan {
    std::string host_exec;         // path handed to the kernel
    std::vector<std::string> argv; // argv handed to the kernel
    std::string guest_program;     // resolved guest path of the final program
    std::string guest_loader;      // resolved guest loader path, if any
    // argv as the final program itself sees it: the caller's argv after #! expansion,
    // without the "ld.so [--argv0 A] PROG" prefix that host_exec/argv carry. The userland
    // loader execs with exactly this vector, because it maps program and loader itself.
    std::vector<std::string> program_argv;
    bool via_loader = false;
    bool loader_argv0 = false;
    int shebang_depth = 0;
};

struct ExecDiag {
    std::string name;    // e.g. "exec.abi_mismatch"
    std::string message; // human-readable explanation
};

class ExecResolver {
public:
    explicit ExecResolver(const vfs::MountTable& table) : table_(table) {}

    // guest_path may be relative (joined with guest_cwd). argv[0] is preserved.
    // Returns 0 or errno; diag is filled for failures that deserve an explanation.
    int plan(const std::string& guest_path, const std::vector<std::string>& argv,
             const std::string& guest_cwd, const vfs::ResolveOptions& opts, ExecPlan& out,
             ExecDiag& diag);

    // Looks up `name` in the colon-separated guest PATH (execvp semantics).
    int search_path(const std::string& name, const std::string& path_list,
                    const std::string& guest_cwd, const vfs::ResolveOptions& opts,
                    std::string& guest_out);

private:
    bool loader_supports_argv0(const std::string& host_loader);

    const vfs::MountTable& table_;
    // (dev, ino, size, mtime) -> supports --argv0
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::int64_t, std::int64_t>, bool>
        argv0_cache_;
};

// Parses a "#!interp [arg]" line (Linux semantics: one optional argument holding
// the rest of the line, trimmed). Returns false when the buffer is not a shebang
// or has no interpreter.
bool parse_shebang(const char* buf, std::size_t len, std::string& interp, std::string& arg,
                   bool& has_arg);

} // namespace vhdp::rootless
