#include "core/engine.hpp"

#include <cerrno>
#include <csignal>

namespace vhdp::core {

int shell_status(const ExitResult& r) noexcept {
    switch (r.reason) {
        case ExitReason::normal:
            return r.exit_code & 0xff;
        case ExitReason::signaled:
            return 128 + r.term_signal;
        case ExitReason::timeout:
            return 124;
        case ExitReason::start_failed:
            if (r.exec_errno == ENOENT) {
                return 127;
            }
            if (r.exec_errno == EACCES || r.exec_errno == ENOEXEC || r.exec_errno == EPERM) {
                return 126;
            }
            return 125;
        case ExitReason::cancelled:
            return 125;
        case ExitReason::none:
            return 125;
    }
    return 125;
}

Result<EngineChoice> select_engine(EngineKind requested,
                                   const std::vector<const Engine*>& engines) {
    auto find = [&](EngineKind k) -> const Engine* {
        for (const auto* e : engines) {
            if (e != nullptr && e->kind() == k) {
                return e;
            }
        }
        return nullptr;
    };

    if (requested == EngineKind::auto_select) {
        std::string why;
        // Only the rootless engine keeps compatibility-isolation semantics that auto
        // may pick without an explicit opt-in (rooted needs --engine rooted, VM and
        // emulator need explicit backends).
        if (const Engine* e = find(EngineKind::rootless)) {
            ProbeReport p = e->probe();
            if (p.available) {
                return EngineChoice{e, "auto selected rootless: " + p.reason};
            }
            why = "rootless: " + p.reason;
        } else {
            why = "rootless: not built";
        }
        for (EngineKind k : {EngineKind::rooted, EngineKind::emulator, EngineKind::vm}) {
            if (const Engine* e = find(k)) {
                why += std::string("; ") + engine_name(k) + ": " + e->probe().reason;
            }
        }
        return Status{Code::engine_unavailable, "no usable engine (" + why + ")"};
    }

    const Engine* e = find(requested);
    if (e == nullptr) {
        return Status{Code::engine_unavailable,
                      std::string("engine '") + engine_name(requested) + "' is not built"};
    }
    ProbeReport p = e->probe();
    if (!p.available) {
        return Status{Code::engine_unavailable, std::string("engine '") + engine_name(requested) +
                                                    "' unavailable: " + p.reason};
    }
    return EngineChoice{e, std::string("explicitly requested ") + engine_name(requested) + ": " +
                               p.reason};
}

} // namespace vhdp::core
