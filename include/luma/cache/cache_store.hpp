#pragma once

#include "luma/graph/task_id.hpp"
#include "luma/types.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace luma::cache {

struct TaskCacheEntry {
    Hash cache_key;
    /// Declared inputs and their content hashes when the task last succeeded.
    std::unordered_map<Path, Hash> input_hashes;
    /// Inputs discovered while running the task (e.g. included headers).
    std::unordered_map<Path, Hash> discovered_hashes;
};

class CacheStore {
public:
    explicit CacheStore(Path cache_dir);

    const TaskCacheEntry* get(const graph::TaskId& id) const;
    void put(const graph::TaskId& id, TaskCacheEntry entry);
    bool save() const;
    bool load();

private:
    Path cache_dir_;
    Path manifest_path_;
    std::unordered_map<std::string, TaskCacheEntry> entries_;
};

} // namespace luma::cache
