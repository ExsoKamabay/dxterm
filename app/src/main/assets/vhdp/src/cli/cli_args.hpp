// phdp command-line parser (pure, no side effects; unit tested).
//
// Grammar:
//   phdp --help | -h | --version | -V
//   phdp help [run] | version
//   phdp doctor [--json] [--no-active]
//   phdp inspect [--json] <ROOTFS_DIR>
//   phdp capabilities [--json]
//   phdp run [OPTIONS] <ROOTFS_DIR> [-- <COMMAND> [ARG...]]
//   phdp [OPTIONS] <ROOTFS_DIR> [-- <COMMAND> [ARG...]]      (alias of run)
//
// A first argument equal to a subcommand name is always the subcommand; a rootfs
// with such a name must be written as a path (./doctor) or after `run`.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace phdp {

enum class Command { run, doctor, inspect, capabilities, help, version };

struct BindArg {
    std::string host;
    std::string guest;
    bool read_write = false;
};

struct RunOptions {
    std::string engine = "auto";
    std::optional<std::string> cwd;
    std::vector<std::string> env;
    bool clear_env = false;
    std::vector<BindArg> binds;
    bool read_only_rootfs = false;
    std::string network = "host";
    std::optional<std::uint32_t> uid;
    std::optional<std::uint32_t> gid;
    std::optional<std::uint64_t> timeout_ms;
    bool trace = false;
    bool verbose = false;
    bool json = false;
    std::optional<int> event_fd;
    bool pty = false;
    std::string seccomp = "auto";
    std::string dev = "minimal";
    std::string proc = "none";
    std::string rootfs;
    std::vector<std::string> command;
};

struct Parsed {
    Command command = Command::help;
    bool implicit_run = false;
    RunOptions run;
    bool json = false;      // doctor/inspect/capabilities
    bool no_active = false; // doctor
    std::string inspect_path;
    std::string help_topic;
};

struct ParseError {
    std::string message;
};

using ParseResult = std::variant<Parsed, ParseError>;

// args excludes argv[0].
ParseResult parse_arguments(const std::vector<std::string>& args);

bool is_subcommand_name(std::string_view s) noexcept;
std::string usage_text();
std::string run_usage_text();

} // namespace phdp
