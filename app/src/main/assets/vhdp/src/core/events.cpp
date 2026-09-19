#include "core/events.hpp"

#include "common/json.hpp"

#include <ctime>

namespace vhdp::core {

std::uint64_t monotonic_ns() noexcept {
    timespec ts{};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

const char* event_kind_name(EventKind k) noexcept {
    switch (k) {
        case EventKind::lifecycle:
            return "lifecycle";
        case EventKind::diagnostic:
            return "diagnostic";
        case EventKind::log:
            return "log";
        case EventKind::trace:
            return "trace";
    }
    return "unknown";
}

const char* severity_name(Severity s) noexcept {
    switch (s) {
        case Severity::debug:
            return "debug";
        case Severity::info:
            return "info";
        case Severity::warning:
            return "warning";
        case Severity::error:
            return "error";
    }
    return "unknown";
}

std::string event_to_json(const Event& ev) {
    JsonWriter w;
    w.begin_object();
    w.key("type").value(event_kind_name(ev.kind));
    w.key("severity").value(severity_name(ev.severity));
    w.key("name").value(ev.name);
    w.key("ts_ns").value(ev.timestamp_ns);
    w.key("session").value(ev.session_id);
    w.key("pid").value(ev.pid);
    w.key("code").value(static_cast<std::int64_t>(ev.code));
    w.key("message").value(ev.message);
    if (json_validate(ev.detail_json)) {
        w.key("detail").raw(ev.detail_json);
    } else {
        w.key("detail").begin_object().end_object();
    }
    w.end_object();
    return std::move(w).take();
}

} // namespace vhdp::core
