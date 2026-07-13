#pragma once

#include "luma/config/model.hpp"
#include "luma/graph/build_graph.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace luma::config {

struct BuildPlan {
    graph::BuildGraph graph;
    std::unordered_map<std::string, std::vector<graph::TaskId>> target_tasks;
    std::string toolchain_identity;
    std::string configuration_fingerprint;
};

struct BuildPlanError {
    std::string message;
};

std::optional<BuildPlanError> build_plan(
    const ProjectConfig& config,
    BuildPlan& out,
    const std::vector<std::string>& only_targets = {});

} // namespace luma::config
