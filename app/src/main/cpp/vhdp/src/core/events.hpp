// Event model shared by the core, engines and the C ABI.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vhdp::core {

enum class EventKind : std::uint32_t { lifecycle = 1, diagnostic = 2, log = 3, trace = 4 };
enum class Severity : std::uint32_t { debug = 0, info = 1, warning = 2, error = 3 };

struct Event {
    EventKind kind = EventKind::log;
    Severity severity = Severity::info;
    std::uint64_t session_id = 0;
    std::int64_t pid = 0;
    std::uint64_t timestamp_ns = 0;
    std::int32_t code = 0;
    std::string name;
    std::string message;
    std::string detail_json = "{}";
};

// Receives events; implementations must be thread-safe and must not throw.
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void emit(Event ev) noexcept = 0;
    // Cheap pre-check so hot paths can skip formatting.
    virtual bool wants(Severity s) const noexcept = 0;
};

std::uint64_t monotonic_ns() noexcept;
const char* event_kind_name(EventKind k) noexcept;
const char* severity_name(Severity s) noexcept;

// One JSON object (no trailing newline) describing the event.
std::string event_to_json(const Event& ev);

} // namespace vhdp::core
