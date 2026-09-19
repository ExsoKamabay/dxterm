// hdr: virtual hardware registry.
//
// Describes the hardware-like capabilities a guest observes and *how* each one
// is provided. The `provision` field is deliberately honest: host pass-through
// is never labelled as emulation.
#pragma once

#include <string>
#include <vector>

namespace vhdp::hdr {

enum class Provision {
    host_passthrough, // host kernel resource used directly (not emulated)
    projected,        // host resource exposed through a vdr driver under policy
    emulated,         // value synthesised by VHDP
    absent,           // not provided
};

struct HardwareDescriptor {
    std::string id;
    std::string summary;
    Provision provision;
    std::string provided_by; // module or driver id
    std::string notes;
};

const char* provision_name(Provision p) noexcept;

// Registry for the running build/host (page size and CPU arch are probed).
std::vector<HardwareDescriptor> hardware_registry();

} // namespace vhdp::hdr
