#pragma once

#include "luma/cache/cache_store.hpp"
#include "luma/config/graph_builder.hpp"
#include "luma/config/model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace luma::executor {

struct ExecutorOptions {
    bool dry_run = false;
    /// Also print each executed command after its progress line.
    bool verbose = false;
    /// Parallel jobs; 0 means the number of hardware threads.
    std::size_t jobs = 0;
};

struct TaskExplanation {
    graph::TaskId id;
    bool would_run = false;
    std::vector<std::string> reasons;
};

struct TaskTiming {
    std::string id;
    std::string verb;
    std::string description;
    /// Absolute path of the task's first output.
    Path output;
    std::int64_t ms = 0;
    bool ok = true;
};

struct ExecuteResult {
    int exit_code = 0;
    /// Tasks that were executed (or would be, for a dry run).
    std::size_t tasks_run = 0;
    /// Tasks that were already up to date.
    std::size_t tasks_skipped = 0;
    /// Tasks whose command failed.
    std::size_t tasks_failed = 0;
    /// True when the build was interrupted (SIGINT/SIGTERM/Ctrl+C).
    bool cancelled = false;
    /// One entry per executed task, in completion order.
    std::vector<TaskTiming> timings;
    std::vector<TaskExplanation> explanations;
};

struct ExecuteError {
    std::string message;
    std::optional<graph::TaskId> task;
};

/// Execute every out-of-date task of the plan.
///
/// Progress is written to stdout: one line `[i/N] <verb> <description>` per
/// executed task, immediately followed by the task's combined
/// stdout/stderr. A task's line and its output are written as one block, so
/// parallel tasks never interleave. N is the number of tasks that need to
/// run. Failures are additionally reported on stderr.
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
