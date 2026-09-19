#include "cli.hpp"
#include "json_dom.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstdio>

namespace phdp {

bool write_all(int fd, const char* data, std::size_t len) noexcept {
    std::size_t done = 0;
    while (done < len) {
        ssize_t n = ::write(fd, data + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

namespace {

int emit_json(const std::string& text) {
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    return std::fflush(stdout) == 0 ? 0 : 1;
}

std::optional<json::Value> parse_or_complain(const std::string& text) {
    auto v = json::parse(text);
    if (!v) {
        std::fprintf(stderr, "phdp: internal error: library returned invalid JSON\n");
    }
    return v;
}

const char* upper_status(const std::string& s) {
    if (s == "pass")
        return "PASS";
    if (s == "fail")
        return "FAIL";
    if (s == "warn")
        return "WARN";
    if (s == "skip")
        return "SKIP";
    return "INFO";
}

} // namespace

int print_doctor(const std::string& text, bool as_json) {
    auto v = parse_or_complain(text);
    if (!v) {
        return kExitStartFailure;
    }
    bool active = v->flag("active_probes");
    bool ready = v->flag("rootless_ready");
    if (as_json) {
        (void)emit_json(text);
    } else {
        std::printf("VHDP doctor (libvhdp %s)\n\n", v->str("vhdp_version").c_str());
        if (const json::Value* checks = v->get("checks")) {
            for (const auto& c : checks->array) {
                std::printf("  [%s] %-26s %s\n", upper_status(c.str("status")), c.str("id").c_str(),
                            c.str("summary").c_str());
            }
        }
        std::printf("\nEngines:\n");
        if (const json::Value* engines = v->get("engines")) {
            for (const auto& e : engines->array) {
                const json::Value* avail = e.get("available");
                const char* state = avail == nullptr || avail->type != json::Value::Type::boolean
                                        ? "not probed"
                                        : (avail->boolean ? "available" : "unavailable");
                std::printf("  %-9s %-12s %s\n", e.str("engine").c_str(), state,
                            e.str("reason").c_str());
            }
        }
        std::printf("\nDetected profile: %s\n", v->str("detected_profile").c_str());
        if (active) {
            std::printf("Rootless engine ready: %s\n", ready ? "yes" : "no");
        }
    }
    return !active || ready ? 0 : 1;
}

int print_capabilities(const std::string& text, bool as_json) {
    if (as_json) {
        return emit_json(text);
    }
    auto v = parse_or_complain(text);
    if (!v) {
        return kExitStartFailure;
    }
    std::printf("VHDP %s capabilities (C ABI %d)\n", v->str("vhdp_version").c_str(),
                static_cast<int>(v->num("abi_version")));
    if (const json::Value* b = v->get("build")) {
        std::printf("Build: %s/%s %s, assembly raw syscall gateway: %s\n",
                    b->str("platform").c_str(), b->str("arch").c_str(),
                    b->str("build_type").c_str(),
                    b->flag("assembly_raw_syscall") ? "yes" : "no (portable C)");
    }
    std::printf("\nEngines:\n");
    if (const json::Value* engines = v->get("engines")) {
        for (const auto& e : engines->array) {
            std::printf("  %-9s %-12s %s\n", e.str("engine").c_str(),
                        e.flag("available") ? "available" : "unavailable", e.str("reason").c_str());
        }
    }
    std::printf("\nProfiles:\n");
    if (const json::Value* profiles = v->get("profiles")) {
        for (const auto& p : profiles->array) {
            std::printf("  %-24s %-17s %s\n", p.str("id").c_str(), p.str("status").c_str(),
                        p.str("description").c_str());
        }
    }
    std::printf("\nFeatures (linux-x86_64 | terminal-unprivileged | android-app | rooted | vm):\n");
    if (const json::Value* features = v->get("features")) {
        for (const auto& f : features->array) {
            const json::Value* st = f.get("status");
            std::printf("  %-30s %-16s %-16s %-16s %-16s %s\n", f.str("id").c_str(),
                        st != nullptr ? st->str("linux-x86_64").c_str() : "",
                        st != nullptr ? st->str("terminal-unprivileged").c_str() : "",
                        st != nullptr ? st->str("android-app").c_str() : "",
                        st != nullptr ? st->str("rooted").c_str() : "",
                        st != nullptr ? st->str("vm").c_str() : "");
        }
    }
    if (const json::Value* s = v->get("syscalls")) {
        std::printf("\nSyscall table (%s): %d pass-through, %d translated, %d emulated, %d denied, "
                    "%d unsupported; unknown numbers denied with ENOSYS\n",
                    s->str("arch").c_str(), static_cast<int>(s->num("pass-through")),
                    static_cast<int>(s->num("translated")), static_cast<int>(s->num("emulated")),
                    static_cast<int>(s->num("denied")), static_cast<int>(s->num("unsupported")));
    }
    std::printf("\n%s\n", v->str("isolation").c_str());
    return 0;
}

int print_inspect(const std::string& text, bool as_json) {
    auto v = parse_or_complain(text);
    if (!v) {
        return kExitStartFailure;
    }
    std::string status = v->str("status");
    if (as_json) {
        (void)emit_json(text);
        return status == "error" ? 1 : 0;
    }
    std::printf("Rootfs: %s\n", v->str("rootfs", v->str("input")).c_str());
    if (const json::Value* fs = v->get("filesystem")) {
        std::printf("Filesystem: %s, noexec=%s, writable=%s\n", fs->str("type").c_str(),
                    fs->flag("noexec") ? "yes" : "no", fs->flag("writable") ? "yes" : "no");
    }
    if (const json::Value* os = v->get("os_release")) {
        std::string name = os->str("pretty_name");
        std::printf("OS: %s\n", name.empty() ? "(unknown)" : name.c_str());
    }
    if (const json::Value* shells = v->get("shells")) {
        for (const auto& s : shells->array) {
            std::printf("Shell %s -> %s", s.str("path").c_str(), s.str("resolved").c_str());
            if (s.flag("elf")) {
                std::printf(" [%s, %s]", s.str("machine").c_str(),
                            s.flag("abi_match") ? "ABI match" : "ABI mismatch");
                std::string interp = s.str("interpreter");
                if (!interp.empty()) {
                    std::printf(" loader %s (%s)", interp.c_str(),
                                s.flag("interpreter_present") ? "present" : "MISSING");
                }
            }
            std::printf("\n");
        }
    }
    if (const json::Value* findings = v->get("findings")) {
        for (const auto& f : findings->array) {
            std::printf("  %-7s %-26s %s\n", f.str("severity").c_str(), f.str("id").c_str(),
                        f.str("message").c_str());
        }
    }
    std::printf("Status: %s\n", status.c_str());
    return status == "error" ? 1 : 0;
}

} // namespace phdp
