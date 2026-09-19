// phdp: thin command-line facade over libvhdp. It uses only the public C++
// wrapper (vhdp.hpp) over the stable C ABI; no engine or lifecycle logic lives here.
#include "cli.hpp"

#include "vhdp/vhdp.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <variant>
#include <vector>

namespace {

int report_query(const vhdp::Result<std::string>& doc, int (*printer)(const std::string&, bool),
                 bool json) {
    if (!doc.ok()) {
        std::fprintf(stderr, "phdp: error: %s: %s\n", doc.status().name(),
                     doc.status().message().c_str());
        return phdp::kExitStartFailure;
    }
    return printer(doc.status().ok() ? const_cast<vhdp::Result<std::string>&>(doc).value()
                                     : std::string(),
                   json);
}

int real_main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    phdp::ParseResult pr = phdp::parse_arguments(args);
    if (auto* err = std::get_if<phdp::ParseError>(&pr)) {
        std::fprintf(stderr, "phdp: %s\nTry 'phdp --help'.\n", err->message.c_str());
        return phdp::kExitUsage;
    }
    const phdp::Parsed& p = std::get<phdp::Parsed>(pr);

    switch (p.command) {
        case phdp::Command::help: {
            const std::string text =
                p.help_topic == "run" ? phdp::run_usage_text() : phdp::usage_text();
            std::fputs(text.c_str(), stdout);
            return 0;
        }
        case phdp::Command::version:
            std::printf("phdp (Virtual Hardware Driver Platform) %s\nlibvhdp ABI %u\n",
                        vhdp_version_string(), vhdp_abi_version());
            return 0;
        case phdp::Command::run:
            return phdp::run_command(p.run);
        case phdp::Command::doctor:
        case phdp::Command::capabilities:
        case phdp::Command::inspect:
            break;
    }

    auto ctx = vhdp::Context::create();
    if (!ctx.ok()) {
        std::fprintf(stderr, "phdp: error: %s\n", ctx.status().message().c_str());
        return phdp::kExitStartFailure;
    }
    if (p.command == phdp::Command::doctor) {
        return report_query(
            ctx.value().doctor_json(p.no_active ? VHDP_DOCTOR_NO_ACTIVE_PROBES : 0u),
            &phdp::print_doctor, p.json);
    }
    if (p.command == phdp::Command::capabilities) {
        return report_query(ctx.value().capabilities_json(), &phdp::print_capabilities, p.json);
    }
    return report_query(ctx.value().inspect_rootfs_json(p.inspect_path), &phdp::print_inspect,
                        p.json);
}

} // namespace

int main(int argc, char** argv) {
    try {
        return real_main(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "phdp: internal error: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "phdp: internal error\n");
    }
    return phdp::kExitStartFailure;
}
