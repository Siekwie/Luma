#include "luma/cli/session.hpp"

#include "luma/config/graph_builder.hpp"
#include "luma/filesystem/path_util.hpp"
#include "luma/lua/config_loader.hpp"
#include "luma/toolchain/toolchain.hpp"

#include <iostream>
#include <filesystem>

namespace luma::cli {

namespace fs = luma::fs;
namespace stdfs = std::filesystem;

std::optional<SessionError> Session::load(Session& out, const SessionOptions& options) {
    out.config = config::ProjectConfig{};
    out.config.root = fs::normalize(options.project_root);
    out.config.profile = options.profile.empty() ? "debug" : options.profile;

    Path config_path = options.config_path;
    if (config_path.empty()) {
        const auto found = fs::find_upwards(out.config.root, "luma.lua");
        if (!found) {
            return SessionError{"could not find luma.lua"};
        }
        config_path = *found;
    }

    if (const auto err = lua_loader::load_config(config_path, out.config)) {
        return SessionError{err->message};
    }

    out.config.root = stdfs::path(config_path).parent_path().generic_string();
    if (out.config.root.empty()) {
        out.config.root = ".";
    }

    out.cache_dir = fs::join(out.config.root, fs::join(".luma", "cache"));

    if (const auto err = config::build_plan(out.config, out.plan, options.targets)) {
        return SessionError{err->message};
    }

    return std::nullopt;
}

void inspect_target(const config::ProjectConfig& config, const std::string& target_name) {
    const config::TargetDef* target = config::find_target(config, target_name);
    if (!target) {
        std::cerr << "unknown target: " << target_name << '\n';
        return;
    }

    const config::ProfileDef* profile = config::find_profile(config, config.profile);
    const config::ToolchainDef* toolchain = config::find_toolchain(config);

    std::cout << "Target: " << target->name << '\n';
    switch (target->kind) {
    case config::TargetKind::Executable:
        std::cout << "Type: executable\n";
        break;
    case config::TargetKind::StaticLibrary:
        std::cout << "Type: static_library\n";
        break;
    case config::TargetKind::SharedLibrary:
        std::cout << "Type: shared_library\n";
        break;
    }

    std::cout << "Sources: " << target->sources.size() + target->source_patterns.size() << " pattern(s)\n";
    std::cout << "Include directories: " << target->include_dirs.size() << '\n';
    std::cout << "Dependencies:";
    if (target->dependencies.empty()) {
        std::cout << " none\n";
    } else {
        std::cout << ' ';
        for (std::size_t i = 0; i < target->dependencies.size(); ++i) {
            if (i != 0) {
                std::cout << ", ";
            }
            std::cout << target->dependencies[i];
        }
        std::cout << '\n';
    }

    if (toolchain) {
        std::cout << "Compiler: " << toolchain->cxx << '\n';
    }
    if (profile) {
        std::cout << "Profile: " << profile->name << '\n';
        std::cout << "Flags:";
        for (const auto& flag : profile->cxxflags) {
            std::cout << ' ' << flag;
        }
        for (const auto& flag : target->cxxflags) {
            std::cout << ' ' << flag;
        }
        std::cout << '\n';
    }
}

void emit_dot(const graph::BuildGraph& graph) {
    std::cout << "digraph luma {\n";
    for (const auto& [id, task] : graph.tasks()) {
        for (const auto& dep : task.dependencies) {
            std::cout << "  \"" << dep.value << "\" -> \"" << id.value << "\";\n";
        }
        if (task.dependencies.empty()) {
            std::cout << "  \"" << id.value << "\";\n";
        }
    }
    std::cout << "}\n";
}

} // namespace luma::cli
