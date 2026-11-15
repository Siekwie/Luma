#pragma once

#include "luma/graph/task_id.hpp"
#include "luma/types.hpp"

#include <fstream>
#include <memory>
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

/// Task fingerprints persisted in `<cache_dir>/manifest.txt`.
///
/// Crash safety: the manifest is only ever replaced atomically. While a
/// build runs, begin_journal() opens `<cache_dir>/journal.txt`, and every
/// invalidate()/put() is appended to it immediately. If luma is killed
/// mid-build, the next load() replays the journal: tasks that were started
/// but never finished have no entry (so they rerun, even if their outputs
/// were left half-written), and tasks that finished keep their new entry.
class CacheStore {
public:
    explicit CacheStore(Path cache_dir);
    ~CacheStore();

    const TaskCacheEntry* get(const graph::TaskId& id) const;
    void put(const graph::TaskId& id, TaskCacheEntry entry);
    /// Forget a task's entry; call before running it.
    void invalidate(const graph::TaskId& id);
    /// Start journaling invalidate()/put() calls to disk.
    bool begin_journal();
    /// Write the manifest atomically and drop the journal.
    bool save();
    bool load();

private:
    Path cache_dir_;
    Path manifest_path_;
    Path journal_path_;
    std::unique_ptr<std::ofstream> journal_;
    std::unordered_map<std::string, TaskCacheEntry> entries_;
};

} // namespace luma::cache
