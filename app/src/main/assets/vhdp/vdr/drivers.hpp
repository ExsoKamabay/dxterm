// vdr: virtual drivers / guest services.
//
// A driver exposes a host resource to the guest. Every driver declares what it
// projects, which permissions it needs, how it is probed, its policy and its
// cleanup. Drivers do not claim hardware emulation: they project or broker
// existing host resources under an explicit policy.
#pragma once

#include "common/status.hpp"
#include "common/unique_fd.hpp"
#include "core/config.hpp"
#include "vfs/mount_table.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vhdp::vdr {

enum class DriverStatus { active, inactive, unavailable };

struct DriverDescriptor {
    const char* id;
    const char* summary;
    const char* permissions; // host permissions the driver relies on
    const char* policy;      // default policy
    const char* cleanup;     // what is released at session end
};

// Static catalogue (for capabilities/doctor output).
const std::vector<DriverDescriptor>& driver_catalogue();

// dev-minimal: projects individual host character devices that carry no host
// secrets. Entries whose host node is missing are skipped.
std::vector<vfs::Mount> dev_minimal_mounts();

// proc-host: read-only projection of host /proc; the resolver applies the
// magic-link policy (self/thread-self remap, unmappable targets fail closed).
vfs::Mount proc_host_mount();

// host-bind: explicit user binds (already validated and canonical).
std::vector<vfs::Mount> host_bind_mounts(const std::vector<core::BindSpec>& binds);

// Builds the full mount table for a validated session config.
vfs::MountTable build_mount_table(const core::ValidatedConfig& cfg);

// PTY terminal driver: allocates a pseudo-terminal pair, forwards window size
// changes and optionally pumps master output to a callback on its own thread.
class PtyDriver {
public:
    static Result<std::unique_ptr<PtyDriver>> open(std::uint16_t rows, std::uint16_t cols);
    ~PtyDriver();
    PtyDriver(const PtyDriver&) = delete;
    PtyDriver& operator=(const PtyDriver&) = delete;

    int master() const noexcept { return master_.get(); }
    int slave() const noexcept { return slave_.get(); }
    // Parent's slave copy must be closed after the guest inherited it, otherwise
    // the master never reports EOF/EIO.
    void close_slave() noexcept { slave_.reset(); }

    Status resize(std::uint16_t rows, std::uint16_t cols);
    Status write_input(const std::uint8_t* data, std::size_t len, std::size_t& written);

    // Starts the output pump. fn is invoked from the pump thread.
    Status start_pump(core::OutputFn fn, void* user, std::uint64_t session_id);
    // Waits for the pump to drain (master EIO/EOF) with a bounded grace period,
    // then stops it. Idempotent.
    void finish_pump() noexcept;

private:
    PtyDriver() = default;
    void pump_main(core::OutputFn fn, void* user, std::uint64_t session_id) noexcept;

    UniqueFd master_;
    UniqueFd slave_;
    UniqueFd wake_; // eventfd used to stop the pump
    std::thread pump_;
    std::atomic<bool> stopping_{false};
};

// Thread-local marker set while a session callback runs (used by the C ABI to
// refuse re-entrant wait/destroy with VHDP_E_BUSY).
struct CallbackScope {
    explicit CallbackScope(std::uint64_t session_id) noexcept;
    ~CallbackScope();
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
    static std::uint64_t current() noexcept; // 0 when not inside a callback
    static bool inside_any() noexcept;

private:
    std::uint64_t previous_;
    bool previous_any_;
};

} // namespace vhdp::vdr
