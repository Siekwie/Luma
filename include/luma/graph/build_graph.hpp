#pragma once

#include "luma/graph/task.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace luma::graph {

struct GraphError {
    enum class Code {
        DuplicateTaskId,
        UnknownDependency,
        CycleDetected,
    };

    Code code;
    std::string message;
};

/// Immutable directed acyclic graph of build tasks.
///
/// Built during the graph phase after Lua configuration is evaluated.
/// The executor consumes a frozen BuildGraph; configuration code never
/// mutates it during execution.
class BuildGraph {
public:
    /// Register a task. Returns an error if the id already exists.
    std::optional<GraphError> add_task(Task task);

    const Task* find_task(const TaskId& id) const;
    const std::unordered_map<TaskId, Task, TaskIdHash>& tasks() const { return tasks_; }

    /// Validate dependency references and detect cycles.
    std::optional<GraphError> validate() const;

    /// Return tasks in topological order (dependencies before dependents).
    /// Returns nullopt if the graph contains a cycle or invalid references.
    std::optional<std::vector<TaskId>> topological_order() const;

    std::size_t size() const { return tasks_.size(); }
    bool empty() const { return tasks_.empty(); }

private:
    std::unordered_map<TaskId, Task, TaskIdHash> tasks_;
};

} // namespace luma::graph
