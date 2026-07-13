#pragma once

#include "luma/cache/cache_store.hpp"
#include "luma/config/graph_builder.hpp"
#include "luma/config/model.hpp"

#include <optional>
#include <string>
#include <vector>

namespace luma::executor {

struct ExecutorOptions {
    bool dry_run = false;
    bool verbose = false;
    std::size_t jobs = 0;
};

struct TaskExplanation {
    graph::TaskId id;
    bool would_run = false;
    std::vector<std::string> reasons;
};

struct ExecuteResult {
    int exit_code = 0;
    std::size_t tasks_run = 0;
    std::size_t tasks_skipped = 0;
    std::vector<TaskExplanation> explanations;
};

struct ExecuteError {
    std::string message;
    std::optional<graph::TaskId> task;
};

ExecuteResult execute(
    const config::ProjectConfig& config,
    const config::BuildPlan& plan,
    cache::CacheStore& cache,
    const ExecutorOptions& options = {});

std::vector<TaskExplanation> explain_target(
    const config::BuildPlan& plan,
    cache::CacheStore& cache,
    const std::string& target_name,
    const Path& root);

} // namespace luma::executor
