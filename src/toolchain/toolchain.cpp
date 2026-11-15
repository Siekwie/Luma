#include "luma/toolchain/toolchain.hpp"

#include "luma/executor/process.hpp"

#include <mutex>
#include <unordered_map>

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
    // Probing the compiler spawns a process; remember the answer for the
    // lifetime of this process.
    static std::mutex mutex;
    static std::unordered_map<std::string, std::string> known;
    const std::string key = (toolchain.style == ToolchainStyle::Msvc ? "msvc:" : "gnu:") + toolchain.cxx;
    {
        std::lock_guard lock(mutex);
        if (const auto it = known.find(key); it != known.end()) {
            return it->second;
        }
    }

    auto first_line = [](const std::string& text) {
        std::string line = text.substr(0, text.find('\n'));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        return line;
    };

    std::string version;
    if (toolchain.style == ToolchainStyle::Msvc) {
        // cl.exe prints its banner on stderr when run without arguments.
        const auto result = executor::run_process({toolchain.cxx});
        version = first_line(!result.stderr_text.empty() ? result.stderr_text : result.stdout_text);
    } else {
        const auto result = executor::run_process({toolchain.cxx, "--version"});
        if (result.exit_code == 0) {
            version = first_line(!result.stdout_text.empty() ? result.stdout_text : result.stderr_text);
        }
    }

    const std::string identity = version.empty() ? toolchain.cxx : toolchain.cxx + " " + version;
    std::lock_guard lock(mutex);
    known[key] = identity;
    return identity;
}

} // namespace luma::toolchain
