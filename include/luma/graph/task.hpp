#pragma once

#include "luma/graph/task_id.hpp"
#include "luma/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace luma::graph {

/// A single unit of work in the build graph.
///
/// A task is up to date when all outputs exist and either:
///   - output content hashes match the cached keys, or
///   - outputs are newer than inputs (timestamp mode, initial implementation)
/// and the command, environment, toolchain identity, and configuration have not changed.
struct Task {
    TaskId id;

    /// Files or artifacts read by this task.
    std::vector<Path> inputs;

    /// Files or artifacts produced by this task.
    std::vector<Path> outputs;

    /// Other tasks that must complete before this one runs.
    std::vector<TaskId> dependencies;

    /// argv-style command. Empty for aggregate or marker tasks.
    std::vector<std::string> command;

    Environment environment;
    std::optional<Path> working_directory;

    /// Precomputed or lazily derived cache fingerprint.
    std::optional<Hash> cache_key;

    Metadata metadata;

    bool has_command() const { return !command.empty(); }
};

/// Compute a stable cache key from task properties and input content hashes.
Hash compute_cache_key(
    const Task& task,
    const std::vector<Hash>& input_hashes,
    const std::string& toolchain_identity = {},
    const std::string& configuration_fingerprint = {});

} // namespace luma::graph
