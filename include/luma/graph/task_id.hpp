#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace luma::graph {

/// Stable identifier for a task within a build graph.
struct TaskId {
    std::string value;

    explicit TaskId(std::string id) : value(std::move(id)) {}

    bool operator==(const TaskId& other) const { return value == other.value; }
    bool operator!=(const TaskId& other) const { return !(*this == other); }
};

struct TaskIdHash {
    std::size_t operator()(const TaskId& id) const {
        return std::hash<std::string>{}(id.value);
    }
};

} // namespace luma::graph
