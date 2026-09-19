#include "cli_args.hpp"

#include <limits>

namespace phdp {

namespace {

constexpr std::uint64_t kMaxTimeoutMs = 366ull * 24 * 3600 * 1000;

bool parse_u32(std::string_view s, std::uint32_t& out) {
    if (s.empty() || s.size() > 10) {
        return false;
    }
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (v >= std::numeric_limits<std::uint32_t>::max()) {
        return false; // 4294967295 is (uid_t)-1
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

// "SECONDS" or "SECONDS.FRACTION" (up to millisecond precision).
bool parse_timeout(std::string_view s, std::uint64_t& ms) {
    std::size_t dot = s.find('.');
    std::string_view whole = s.substr(0, dot);
    std::string_view frac = dot == std::string_view::npos ? std::string_view{} : s.substr(dot + 1);
    if (whole.empty() || whole.size() > 8 ||
        (dot != std::string_view::npos && (frac.empty() || frac.size() > 3))) {
        return false;
    }
    std::uint64_t sec = 0;
    for (char c : whole) {
        if (c < '0' || c > '9') {
            return false;
        }
        sec = sec * 10 + static_cast<std::uint64_t>(c - '0');
    }
    std::uint64_t f = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        f *= 10;
        if (i < frac.size()) {
            if (frac[i] < '0' || frac[i] > '9') {
                return false;
            }
            f += static_cast<std::uint64_t>(frac[i] - '0');
        }
    }
    ms = sec * 1000 + f;
    return ms > 0 && ms <= kMaxTimeoutMs;
}

bool one_of(std::string_view v, std::initializer_list<std::string_view> allowed) {
    for (auto a : allowed) {
        if (v == a) {
            return true;
        }
    }
    return false;
}

std::optional<ParseError> parse_bind(std::string_view spec, BindArg& out) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (;;) {
        std::size_t p = spec.find(':', start);
        parts.push_back(
            spec.substr(start, p == std::string_view::npos ? std::string_view::npos : p - start));
        if (p == std::string_view::npos) {
            break;
        }
        start = p + 1;
    }
    if (parts.size() < 2 || parts.size() > 3) {
        return ParseError{
            "invalid --bind '" + std::string(spec) +
            "': expected HOST:GUEST[:ro|rw] (paths containing ':' are not supported)"};
    }
    if (parts[0].empty() || parts[1].empty()) {
        return ParseError{"invalid --bind '" + std::string(spec) +
                          "': HOST and GUEST must not be empty"};
    }
    out.host = std::string(parts[0]);
    out.guest = std::string(parts[1]);
    out.read_write = false;
    if (parts.size() == 3) {
        if (parts[2] == "rw") {
            out.read_write = true;
        } else if (parts[2] != "ro") {
            return ParseError{"invalid --bind mode '" + std::string(parts[2]) + "': use ro or rw"};
        }
    }
    if (out.guest.front() != '/') {
        return ParseError{"invalid --bind '" + std::string(spec) +
                          "': GUEST must be an absolute path"};
    }
    return std::nullopt;
}

// Splits "--name=value"; returns true when the token had an inline value.
bool split_inline(std::string_view token, std::string_view& name, std::string_view& value) {
    std::size_t eq = token.find('=');
    if (eq == std::string_view::npos) {
        name = token;
        return false;
    }
    name = token.substr(0, eq);
    value = token.substr(eq + 1);
    return true;
}

ParseResult parse_run(const std::vector<std::string>& args, std::size_t i, bool implicit) {
    Parsed p;
    p.command = Command::run;
    p.implicit_run = implicit;
    RunOptions& o = p.run;

    for (; i < args.size(); ++i) {
        std::string_view tok = args[i];
        if (tok == "--") {
            if (o.rootfs.empty()) {
                return ParseError{"ROOTFS_DIR must be given before '--'"};
            }
            for (std::size_t k = i + 1; k < args.size(); ++k) {
                o.command.push_back(args[k]);
            }
            if (i + 1 >= args.size()) {
                return ParseError{"'--' must be followed by a command"};
            }
            break;
        }
        if (!o.rootfs.empty()) {
            if (!tok.empty() && tok.front() == '-') {
                return ParseError{"option '" + std::string(tok) +
                                  "' after ROOTFS_DIR; put options before ROOTFS_DIR"};
            }
            return ParseError{
                "unexpected argument '" + std::string(tok) +
                "' after ROOTFS_DIR; separate the command with '--' (phdp run ROOTFS -- COMMAND)"};
        }
        if (tok.empty()) {
            return ParseError{"empty argument"};
        }
        if (tok.front() != '-') {
            o.rootfs = std::string(tok);
            continue;
        }

        std::string_view name;
        std::string_view inline_value;
        bool has_inline = split_inline(tok, name, inline_value);
        auto flag = [&](bool& target) -> std::optional<ParseError> {
            if (has_inline) {
                return ParseError{"option " + std::string(name) + " does not take a value"};
            }
            target = true;
            return std::nullopt;
        };
        auto value = [&](std::string& target) -> std::optional<ParseError> {
            if (has_inline) {
                target = std::string(inline_value);
                return std::nullopt;
            }
            if (i + 1 >= args.size()) {
                return ParseError{"option " + std::string(name) + " requires a value"};
            }
            target = args[++i];
            return std::nullopt;
        };

        std::optional<ParseError> err;
        std::string v;
        if (name == "--help" || name == "-h") {
            Parsed h;
            h.command = Command::help;
            h.help_topic = "run";
            return h;
        } else if (name == "--engine") {
            if (!(err = value(v))) {
                if (!one_of(v, {"auto", "rootless", "rooted", "emulator", "vm"})) {
                    return ParseError{"invalid --engine '" + v +
                                      "': use auto|rootless|rooted|emulator|vm"};
                }
                o.engine = v;
            }
        } else if (name == "--cwd") {
            if (!(err = value(v))) {
                if (v.empty() || v.front() != '/') {
                    return ParseError{"--cwd requires an absolute guest path"};
                }
                o.cwd = v;
            }
        } else if (name == "--env" || name == "-e") {
            if (!(err = value(v))) {
                std::size_t eq = v.find('=');
                if (eq == std::string::npos || eq == 0) {
                    return ParseError{"invalid --env '" + v + "': expected KEY=VALUE"};
                }
                o.env.push_back(v);
            }
        } else if (name == "--clear-env") {
            err = flag(o.clear_env);
        } else if (name == "--bind" || name == "-b") {
            if (!(err = value(v))) {
                BindArg b;
                if (auto be = parse_bind(v, b)) {
                    return *be;
                }
                o.binds.push_back(std::move(b));
            }
        } else if (name == "--read-only-rootfs") {
            err = flag(o.read_only_rootfs);
        } else if (name == "--network") {
            if (!(err = value(v))) {
                if (!one_of(v, {"host", "none"})) {
                    return ParseError{"invalid --network '" + v + "': use host|none"};
                }
                o.network = v;
            }
        } else if (name == "--uid" || name == "--gid") {
            if (!(err = value(v))) {
                std::uint32_t id = 0;
                if (!parse_u32(v, id)) {
                    return ParseError{"invalid " + std::string(name) + " '" + v +
                                      "': expected 0..4294967294"};
                }
                (name == "--uid" ? o.uid : o.gid) = id;
            }
        } else if (name == "--timeout") {
            if (!(err = value(v))) {
                std::uint64_t ms = 0;
                if (!parse_timeout(v, ms)) {
                    return ParseError{"invalid --timeout '" + v +
                                      "': expected positive seconds, e.g. 30 or 1.5"};
                }
                o.timeout_ms = ms;
            }
        } else if (name == "--trace") {
            err = flag(o.trace);
        } else if (name == "--verbose" || name == "-v") {
            err = flag(o.verbose);
        } else if (name == "--json") {
            err = flag(o.json);
        } else if (name == "--event-fd") {
            if (!(err = value(v))) {
                std::uint32_t fd = 0;
                if (!parse_u32(v, fd) || fd < 2 || fd > 1024 * 1024) {
                    return ParseError{
                        "invalid --event-fd '" + v +
                        "': must be a descriptor >= 2 (stdin/stdout are reserved for the guest)"};
                }
                o.event_fd = static_cast<int>(fd);
            }
        } else if (name == "--pty") {
            err = flag(o.pty);
        } else if (name == "--seccomp") {
            if (!(err = value(v))) {
                if (!one_of(v, {"auto", "on", "off"})) {
                    return ParseError{"invalid --seccomp '" + v + "': use auto|on|off"};
                }
                o.seccomp = v;
            }
        } else if (name == "--dev") {
            if (!(err = value(v))) {
                if (!one_of(v, {"minimal", "none"})) {
                    return ParseError{"invalid --dev '" + v + "': use minimal|none"};
                }
                o.dev = v;
            }
        } else if (name == "--proc") {
            if (!(err = value(v))) {
                if (!one_of(v, {"none", "host"})) {
                    return ParseError{"invalid --proc '" + v + "': use none|host"};
                }
                o.proc = v;
            }
        } else {
            return ParseError{"unknown option '" + std::string(name) + "' (see phdp run --help)"};
        }
        if (err) {
            return *err;
        }
    }
    if (o.rootfs.empty()) {
        return ParseError{"missing ROOTFS_DIR (see phdp --help)"};
    }
    return p;
}

} // namespace

bool is_subcommand_name(std::string_view s) noexcept {
    return s == "doctor" || s == "inspect" || s == "capabilities" || s == "run" || s == "help" ||
           s == "version";
}

ParseResult parse_arguments(const std::vector<std::string>& args) {
    if (args.empty()) {
        return ParseError{"missing ROOTFS_DIR or subcommand (see phdp --help)"};
    }
    const std::string& first = args[0];
    if (first == "--help" || first == "-h" || first == "help") {
        Parsed p;
        p.command = Command::help;
        if (first == "help" && args.size() >= 2) {
            if (args.size() > 2 || !is_subcommand_name(args[1])) {
                return ParseError{"usage: phdp help [doctor|inspect|capabilities|run|version]"};
            }
            p.help_topic = args[1];
        } else if (args.size() > 1) {
            return ParseError{"unexpected arguments after " + first};
        }
        return p;
    }
    if (first == "--version" || first == "-V" || first == "version") {
        if (args.size() > 1) {
            return ParseError{"unexpected arguments after " + first};
        }
        Parsed p;
        p.command = Command::version;
        return p;
    }
    if (first == "doctor" || first == "capabilities") {
        Parsed p;
        p.command = first == "doctor" ? Command::doctor : Command::capabilities;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--json") {
                p.json = true;
            } else if (args[i] == "--no-active" && p.command == Command::doctor) {
                p.no_active = true;
            } else if (args[i] == "--help" || args[i] == "-h") {
                p.command = Command::help;
                p.help_topic = first;
                return p;
            } else {
                return ParseError{"unknown argument for " + first + ": '" + args[i] + "'"};
            }
        }
        return p;
    }
    if (first == "inspect") {
        Parsed p;
        p.command = Command::inspect;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--json") {
                p.json = true;
            } else if (args[i] == "--help" || args[i] == "-h") {
                p.command = Command::help;
                p.help_topic = "inspect";
                return p;
            } else if (!args[i].empty() && args[i].front() == '-') {
                return ParseError{"unknown option for inspect: '" + args[i] + "'"};
            } else if (p.inspect_path.empty()) {
                p.inspect_path = args[i];
            } else {
                return ParseError{"inspect takes exactly one ROOTFS_DIR"};
            }
        }
        if (p.inspect_path.empty()) {
            return ParseError{"inspect requires ROOTFS_DIR"};
        }
        return p;
    }
    if (first == "run") {
        return parse_run(args, 1, false);
    }
    return parse_run(args, 0, true);
}

std::string usage_text() {
    return R"(Usage:
  phdp run [OPTIONS] <ROOTFS_DIR> [-- <COMMAND> [ARG...]]
  phdp [OPTIONS] <ROOTFS_DIR> [-- <COMMAND> [ARG...]]     (alias of run)
  phdp doctor [--json] [--no-active]
  phdp inspect [--json] <ROOTFS_DIR>
  phdp capabilities [--json]
  phdp --help | --version

Subcommand names (doctor, inspect, capabilities, run, help, version) in the first
position are always subcommands. Use ./doctor or `phdp run doctor` for a rootfs
directory with such a name. Without a command, /bin/sh is started.

Exit status: the guest's exit status; 128+N when the guest was killed by signal N;
124 on --timeout; 126/127 when the command cannot be executed/found; 125 when phdp
could not start the session; 2 for usage errors.

VHDP's rootless engine provides compatibility isolation (path translation), not a
security sandbox. Do not run untrusted root filesystems with it.

Run `phdp run --help` for run options.
)";
}

std::string run_usage_text() {
    return R"(Usage: phdp run [OPTIONS] <ROOTFS_DIR> [-- <COMMAND> [ARG...]]

Options:
  --engine auto|rootless|rooted|emulator|vm   engine selection (default auto)
  --cwd GUEST_PATH             guest working directory (default /)
  --env KEY=VALUE, -e          add/override a guest environment variable
  --clear-env                  start from an empty guest environment
  --bind HOST:GUEST[:ro|rw]    bind a host directory (read-only unless :rw)
  --read-only-rootfs           writes to the rootfs fail with EROFS
  --network host|none          host network (default) or refuse IP sockets
  --uid N, --gid N             guest-visible identity only (never host privilege)
  --timeout SECONDS            kill the guest tree after SECONDS (exit 124)
  --pty                        run the guest on a new pseudo-terminal
  --seccomp auto|on|off        seccomp acceleration of the ptrace supervisor
  --dev minimal|none           device projection (default minimal)
  --proc none|host             /proc projection (default none)
  --trace                      emit per-syscall trace events
  --verbose, -v                print informational events
  --json                       events as JSON Lines on stderr (or --event-fd)
  --event-fd N                 write events to descriptor N (N >= 2)
  --help, -h                   show this help

The guest environment contains PATH, HOME and the host TERM/COLORTERM/LANG/LC_ALL/TZ
unless --clear-env is given; no other host variable is passed.
)";
}

} // namespace phdp
