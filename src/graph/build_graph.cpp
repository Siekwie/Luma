#include "luma/graph/build_graph.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace luma::graph {

namespace {

GraphError make_error(GraphError::Code code, std::string message) {
    return GraphError{code, std::move(message)};
}

} // namespace

std::optional<GraphError> BuildGraph::add_task(Task task) {
    if (tasks_.contains(task.id)) {
        return make_error(
            GraphError::Code::DuplicateTaskId,
            "duplicate task id: " + task.id.value);
    }
    tasks_.emplace(task.id, std::move(task));
    return std::nullopt;
}

const Task* BuildGraph::find_task(const TaskId& id) const {
    const auto it = tasks_.find(id);
    return it == tasks_.end() ? nullptr : &it->second;
}

std::optional<GraphError> BuildGraph::validate() const {
    for (const auto& [id, task] : tasks_) {
        for (const auto& dep : task.dependencies) {
            if (!tasks_.contains(dep)) {
                return make_error(
                    GraphError::Code::UnknownDependency,
                    "task '" + id.value + "' depends on unknown task '" + dep.value + "'");
            }
        }
    }

    if (auto order = topological_order()) {
        (void)order;
        return std::nullopt;
    }

    return make_error(GraphError::Code::CycleDetected, "dependency cycle detected");
}

std::optional<std::vector<TaskId>> BuildGraph::topological_order() const {
    std::unordered_map<TaskId, std::size_t, TaskIdHash> in_degree;
    in_degree.reserve(tasks_.size());

    for (const auto& [id, _] : tasks_) {
        in_degree.emplace(id, 0);
    }

    for (const auto& [_, task] : tasks_) {
        for (const auto& dep : task.dependencies) {
            if (!tasks_.contains(dep)) {
                return std::nullopt;
            }
            ++in_degree.at(task.id);
        }
    }

    // Kahn's algorithm: start with tasks that have no dependents waiting on them.
    // We track in_degree as "number of unprocessed dependencies".
    std::unordered_map<TaskId, std::size_t, TaskIdHash> remaining_deps;
    for (const auto& [id, task] : tasks_) {
        remaining_deps[id] = task.dependencies.size();
    }

    std::vector<TaskId> ready;
    ready.reserve(tasks_.size());
    for (const auto& [id, count] : remaining_deps) {
        if (count == 0) {
            ready.push_back(id);
        }
    }

    std::vector<TaskId> order;
    order.reserve(tasks_.size());

    while (!ready.empty()) {
        const TaskId current = ready.back();
        ready.pop_back();
        order.push_back(current);

        for (const auto& [id, task] : tasks_) {
            if (std::find(task.dependencies.begin(), task.dependencies.end(), current) !=
                task.dependencies.end()) {
                if (--remaining_deps.at(id) == 0) {
                    ready.push_back(id);
                }
            }
        }
    }

    if (order.size() != tasks_.size()) {
        return std::nullopt;
    }

    return order;
}

} // namespace luma::graph
