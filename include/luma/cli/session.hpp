#pragma once

#include "luma/config/graph_builder.hpp"
#include "luma/config/model.hpp"
#include "luma/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace luma::cli {

struct SessionOptions {
    /// Directory to start searching for luma.lua from (upwards).
    Path project_root;
    /// Explicit configuration file; skips the search when set.
    Path config_path;
    /// Profile requested on the command line; empty selects the default.
    std::string profile;
    /// Only plan these targets (and their dependencies); empty means all.
    std::vector<std::string> targets;
};

struct SessionError {
    std::string message;
};

struct Session {
    /// Loaded configuration. `config.root` is absolute and lexically normal.
    config::ProjectConfig config;
    config::BuildPlan plan;
    Path cache_dir;

    static std::optional<SessionError> load(Session& out, const SessionOptions& options);

    /// Absolute build directory.
    Path build_dir() const;
    /// Profile names, sorted.
    std::vector<std::string> profile_names() const;
};

void inspect_target(const config::ProjectConfig& config, const std::string& target_name);
void emit_dot(const graph::BuildGraph& graph);

} // namespace luma::cli
