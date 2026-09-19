#include "core/context.hpp"

#include "core/reports.hpp"

namespace vhdp::core {

Context::Context(EventSink* sink) : sink_(sink) {
    owned_.push_back(make_rootless_engine());
    owned_.push_back(make_unavailable_engine(EngineKind::rooted));
    owned_.push_back(make_unavailable_engine(EngineKind::emulator));
    owned_.push_back(make_unavailable_engine(EngineKind::vm));
    for (const auto& e : owned_) {
        engines_.push_back(e.get());
    }
}

Context::Context(EventSink* sink, std::vector<std::unique_ptr<Engine>> engines)
    : owned_(std::move(engines)), sink_(sink) {
    for (const auto& e : owned_) {
        engines_.push_back(e.get());
    }
}

EventSink& Context::sink() noexcept {
    return sink_ != nullptr ? *sink_ : null_sink_;
}

std::string Context::doctor_json(bool active_probes, bool refresh) {
    std::lock_guard<std::mutex> lock(cache_mu_);
    if (!doctor_valid_ || refresh || doctor_active_ != active_probes) {
        doctor_cache_ = build_doctor_json(*this, active_probes);
        doctor_valid_ = true;
        doctor_active_ = active_probes;
    }
    return doctor_cache_;
}

std::string Context::capabilities_json() {
    return build_capabilities_json(*this);
}

std::string Context::inspect_json(const std::string& rootfs) {
    std::lock_guard<std::mutex> lock(cache_mu_);
    if (inspect_path_ != rootfs || inspect_cache_.empty()) {
        inspect_cache_ = build_inspect_json(rootfs);
        inspect_path_ = rootfs;
    }
    return inspect_cache_;
}

} // namespace vhdp::core
