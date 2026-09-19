// Execution engine interface. Ownership: the core owns an Engine (stateless
// descriptor + probe) per context and one EngineInstance per session. Engines
// never keep global mutable registries; everything is session-scoped.
#pragma once

#include "common/status.hpp"
#include "core/config.hpp"
#include "core/events.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vhdp::core {

enum class ExitReason : std::uint32_t {
    none = 0,
    normal = 1,
    signaled = 2,
    timeout = 3,
    cancelled = 4,
    start_failed = 5
};

struct ExitResult {
    ExitReason reason = ExitReason::none;
    int exit_code = 0;
    int term_signal = 0;
    int exec_errno = 0; // errno of a failed initial exec (start_failed)
    Status status;      // start failure / internal error detail
};

// Shell-style status: exit code, 128+signal, 124 timeout, 125 for start failure/cancel.
int shell_status(const ExitResult& r) noexcept;

struct ProbeReport {
    bool available = false;
    std::string reason;               // human-readable summary
    std::vector<std::string> details; // evidence lines
};

class EngineInstance {
public:
    virtual ~EngineInstance() = default;
    // Spawns the guest. Returns once the initial exec succeeded or failed.
    virtual Status start() = 0;
    // Blocks until the whole guest process tree is gone (idempotent after completion).
    virtual ExitResult wait() = 0;
    virtual Status signal(int signo) = 0;
    virtual Status resize(std::uint16_t rows, std::uint16_t cols) = 0;
    // Kills the process tree; `reason` is reported by wait().
    virtual Status cancel(ExitReason reason) = 0;
    virtual int pty_master() const noexcept = 0;
    virtual Status write_input(const std::uint8_t* data, std::size_t len, std::size_t& written) = 0;
};

class Engine {
public:
    virtual ~Engine() = default;
    virtual EngineKind kind() const noexcept = 0;
    virtual ProbeReport probe() const = 0;
    virtual Result<std::unique_ptr<EngineInstance>>
    create_instance(std::shared_ptr<const ValidatedConfig> cfg, EventSink& sink,
                    std::uint64_t session_id) const = 0;
};

struct EngineChoice {
    const Engine* engine = nullptr;
    std::string reason;
};

// Picks an engine for `requested` among `engines` using their probes. `auto`
// selects rootless when its probe passes. Never falls back silently to an engine
// with different security semantics: an explicitly requested engine that is
// unavailable is an error.
Result<EngineChoice> select_engine(EngineKind requested, const std::vector<const Engine*>& engines);

// Built-in engines.
std::unique_ptr<Engine> make_rootless_engine();
std::unique_ptr<Engine> make_unavailable_engine(EngineKind kind);

} // namespace vhdp::core
