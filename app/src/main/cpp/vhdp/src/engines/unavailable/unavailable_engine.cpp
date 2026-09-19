// Descriptors for engines that are part of the architecture but have no backend in
// this build. They are never reported as available and cannot create instances;
// see docs/CAPABILITIES.md (status backend-required).
#include "core/engine.hpp"

namespace vhdp::core {

namespace {

class UnavailableEngine final : public Engine {
public:
    explicit UnavailableEngine(EngineKind kind) : kind_(kind) {}

    EngineKind kind() const noexcept override { return kind_; }

    ProbeReport probe() const override {
        ProbeReport r;
        r.available = false;
        switch (kind_) {
            case EngineKind::rooted:
                r.reason = "backend-required: the rooted namespace/chroot engine is not "
                           "implemented in this build";
                r.details.emplace_back("requires probed mount-namespace, chroot/pivot_root and "
                                       "cgroup capabilities; UID 0 alone is not evidence");
                break;
            case EngineKind::emulator:
                r.reason = "backend-required: no user-mode emulator adapter (e.g. QEMU user) is "
                           "integrated; cross-architecture guests are unsupported";
                break;
            case EngineKind::vm:
                r.reason = "backend-required: no full-system VM backend (AVF/KVM/QEMU system) is "
                           "integrated";
                r.details.emplace_back("AVF custom VMs require system/privileged apps; QEMU TCG is "
                                       "emulation, not acceleration");
                break;
            default:
                r.reason = "backend-required";
                break;
        }
        return r;
    }

    Result<std::unique_ptr<EngineInstance>> create_instance(std::shared_ptr<const ValidatedConfig>,
                                                            EventSink&,
                                                            std::uint64_t) const override {
        return Status{Code::engine_unavailable, probe().reason};
    }

private:
    EngineKind kind_;
};

} // namespace

std::unique_ptr<Engine> make_unavailable_engine(EngineKind kind) {
    return std::make_unique<UnavailableEngine>(kind);
}

} // namespace vhdp::core
