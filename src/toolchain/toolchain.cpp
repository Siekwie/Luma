#include "luma/toolchain/toolchain.hpp"

#include "luma/executor/process.hpp"

namespace luma::toolchain {

namespace {

using config::ToolchainDef;
using config::ToolchainStyle;

bool command_available(const std::string& command) {
    const auto result = executor::run_process({command, "--version"});
    if (result.exit_code == 0) {
        return true;
    }
#ifdef _WIN32
    const auto where = executor::run_process({"where", command});
    return where.exit_code == 0;
#else
    const auto which = executor::run_process({"sh", "-c", "command -v " + command});
    return which.exit_code == 0;
#endif
}

ToolchainDef make_gnu(const std::string& cxx, const std::string& cc) {
    ToolchainDef toolchain;
    toolchain.name = "auto";
    toolchain.style = ToolchainStyle::Gnu;
    toolchain.cxx = cxx;
    toolchain.cc = cc.empty() ? "cc" : cc;
    toolchain.linker = cxx;
    toolchain.archiver = "ar";
    toolchain.languages = {"c", "c++"};
    return toolchain;
}

ToolchainDef make_msvc() {
    ToolchainDef toolchain;
    toolchain.name = "auto";
    toolchain.style = ToolchainStyle::Msvc;
    toolchain.cxx = "cl";
    toolchain.cc = "cl";
    toolchain.linker = "link";
    toolchain.archiver = "lib";
    toolchain.languages = {"c", "c++"};
    return toolchain;
}

} // namespace

config::ToolchainDef detect_default() {
    if (command_available("clang++")) {
        return make_gnu("clang++", "clang");
    }
    if (command_available("g++")) {
        return make_gnu("g++", "gcc");
    }
    if (command_available("cl")) {
        return make_msvc();
    }
    return make_gnu("c++", "cc");
}

std::string identity_string(const config::ToolchainDef& toolchain) {
    if (toolchain.style == ToolchainStyle::Msvc) {
        const auto result = executor::run_process({toolchain.cxx});
        if (!result.stderr_text.empty()) {
            return result.stderr_text.substr(0, result.stderr_text.find('\n'));
        }
        return "msvc-" + toolchain.cxx;
    }

    const auto result = executor::run_process({toolchain.cxx, "--version"});
    if (!result.stdout_text.empty()) {
        return result.stdout_text.substr(0, result.stdout_text.find('\n'));
    }
    if (!result.stderr_text.empty()) {
        return result.stderr_text.substr(0, result.stderr_text.find('\n'));
    }
    return toolchain.cxx;
}

} // namespace luma::toolchain
