// Library context: owns engine descriptors, the event sink and cached reports.
#pragma once

#include "core/engine.hpp"
#include "core/events.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vhdp::core {

class Context {
public:
    // sink may be nullptr; it is not owned and must outlive the context.
    explicit Context(EventSink* sink);
    // Test seam: a context with an explicit engine set (e.g. mock engines).
    Context(EventSink* sink, std::vector<std::unique_ptr<Engine>> engines);
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    const std::vector<const Engine*>& engines() const noexcept { return engines_; }
    EventSink& sink() noexcept;
    std::uint64_t next_session_id() noexcept { return next_id_.fetch_add(1); }

    std::atomic<int>& live_sessions() noexcept { return live_sessions_; }

    // Cached unless refresh is set (probes can be slow and fork children).
    std::string doctor_json(bool active_probes, bool refresh);
    std::string capabilities_json();
    std::string inspect_json(const std::string& rootfs);

private:
    class NullSink final : public EventSink {
    public:
        void emit(Event) noexcept override {}
        bool wants(Severity) const noexcept override { return false; }
    };

    std::vector<std::unique_ptr<Engine>> owned_;
    std::vector<const Engine*> engines_;
    EventSink* sink_;
    NullSink null_sink_;
    std::atomic<std::uint64_t> next_id_{1};
    std::atomic<int> live_sessions_{0};

    std::mutex cache_mu_;
    bool doctor_valid_ = false;
    bool doctor_active_ = false;
    std::string doctor_cache_;
    std::string inspect_path_;
    std::string inspect_cache_;
};

} // namespace vhdp::core
