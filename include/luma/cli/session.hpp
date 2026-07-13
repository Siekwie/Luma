#pragma once

#include "luma/config/graph_builder.hpp"
#include "luma/config/model.hpp"
#include "luma/types.hpp"

#include <optional>
#include <string>

namespace luma::cli {

struct SessionOptions {
    Path project_root;
    Path config_path;
    std::string profile;
    std::vector<std::string> targets;
};

struct SessionError {
    std::string message;
};

struct Session {
    config::ProjectConfig config;
    config::BuildPlan plan;
    Path cache_dir;

    static std::optional<SessionError> load(Session& out, const SessionOptions& options);
};

void inspect_target(const config::ProjectConfig& config, const std::string& target_name);
void emit_dot(const graph::BuildGraph& graph);

} // namespace luma::cli
