#include "luma/cli/session.hpp"

#include "luma/config/graph_builder.hpp"
#include "luma/filesystem/path_util.hpp"
#include "luma/lua/config_loader.hpp"
#include "luma/toolchain/toolchain.hpp"

#include <algorithm>
#include <iostream>

namespace luma::cli {

namespace fs = luma::fs;

std::optional<SessionError> Session::load(Session& out, const SessionOptions& options) {
    out.config = config::ProjectConfig{};
    out.plan = config::BuildPlan{};
    out.config.profile.clear();

    Path config_path = options.config_path;
    if (config_path.empty()) {
        const Path start = fs::absolute(options.project_root.empty() ? Path{"."} : options.project_root);
        const auto found = fs::find_upwards(start, "luma.lua");
        if (!found) {
            return SessionError{"could not find luma.lua in " + start + " or any parent directory"};
        }
        config_path = *found;
    }
    config_path = fs::absolute(config_path);
    out.config.root = fs::parent(config_path);

    if (const auto err = lua_loader::load_config(config_path, out.config)) {
        return SessionError{err->message};
    }
    // The root is the directory holding luma.lua, regardless of the working
    // directory luma was started from.
    out.config.root = fs::parent(config_path);

    // Profile selection: --profile wins, then set_profile() from luma.lua,
    // then "debug", then the first profile by name.
    const std::string configured = out.config.profile;
    const auto names = out.profile_names();
    if (!options.profile.empty()) {
        if (!config::find_profile(out.config, options.profile)) {
            std::string available;
            for (const auto& name : names) {
                available += available.empty() ? name : ", " + name;
            }
            return SessionError{
                "unknown profile '" + options.profile + "' (available: " + available + ")"};
        }
        out.config.profile = options.profile;
    } else if (!configured.empty() && config::find_profile(out.config, configured)) {
        out.config.profile = configured;
    } else if (config::find_profile(out.config, "debug") || names.empty()) {
        out.config.profile = "debug";
    } else {
        out.config.profile = names.front();
    }

    out.cache_dir = fs::join(out.config.root, ".luma/cache");

    if (const auto err = config::build_plan(out.config, out.plan, options.targets)) {
        return SessionError{err->message};
    }

    return std::nullopt;
}

Path Session::build_dir() const {
    return fs::join(config.root, config.build_dir);
}

std::vector<std::string> Session::profile_names() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : config.profiles) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
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
    std::cout << "Type: " << config::kind_name(target->kind) << '\n';
    std::cout << "Language: " << config::canonical_language(target->language) << '\n';

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
        std::cout << "C++ compiler: " << toolchain->cxx << "\nC compiler: " << toolchain->cc << '\n';
    }
    if (profile) {
        std::cout << "Profile: " << profile->name << '\n';
        std::cout << "C++ flags:";
        for (const auto& flag : profile->cxxflags) {
            std::cout << ' ' << flag;
        }
        for (const auto& flag : target->cxxflags) {
            std::cout << ' ' << flag;
        }
        std::cout << '\n';
        std::cout << "C flags:";
        for (const auto& flag : profile->cflags) {
            std::cout << ' ' << flag;
        }
        for (const auto& flag : target->cflags) {
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
