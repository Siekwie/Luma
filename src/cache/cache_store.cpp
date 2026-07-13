#include "luma/cache/cache_store.hpp"

#include "luma/filesystem/path_util.hpp"

#include <fstream>
#include <sstream>

namespace luma::cache {

namespace fs = luma::fs;

CacheStore::CacheStore(Path cache_dir)
    : cache_dir_(fs::normalize(cache_dir)), manifest_path_(fs::join(cache_dir_, "manifest.txt")) {}

const TaskCacheEntry* CacheStore::get(const graph::TaskId& id) const {
    const auto it = entries_.find(id.value);
    return it == entries_.end() ? nullptr : &it->second;
}

void CacheStore::put(const graph::TaskId& id, TaskCacheEntry entry) {
    entries_[id.value] = std::move(entry);
}

bool CacheStore::save() const {
    fs::create_directories(cache_dir_);
    std::ofstream out(manifest_path_);
    if (!out) {
        return false;
    }

    for (const auto& [task_id, entry] : entries_) {
        out << "[task:" << task_id << "]\n";
        out << "cache_key=" << entry.cache_key << '\n';
        for (const auto& [path, hash] : entry.input_hashes) {
            out << "input=" << path << '|' << hash << '\n';
        }
        out << '\n';
    }
    return true;
}

bool CacheStore::load() {
    entries_.clear();
    std::ifstream in(manifest_path_);
    if (!in) {
        return false;
    }

    std::string line;
    std::string current_task;
    TaskCacheEntry current_entry;

    auto flush = [&]() {
        if (!current_task.empty()) {
            entries_[current_task] = current_entry;
        }
        current_task.clear();
        current_entry = {};
    };

    while (std::getline(in, line)) {
        if (line.empty()) {
            flush();
            continue;
        }
        if (line.starts_with("[task:") && line.ends_with(']')) {
            flush();
            current_task = line.substr(6, line.size() - 7);
            continue;
        }
        if (line.starts_with("cache_key=")) {
            current_entry.cache_key = line.substr(10);
            continue;
        }
        if (line.starts_with("input=")) {
            const auto payload = line.substr(6);
            const auto sep = payload.find('|');
            if (sep != std::string::npos) {
                current_entry.input_hashes[payload.substr(0, sep)] = payload.substr(sep + 1);
            }
        }
    }
    flush();
    return true;
}

} // namespace luma::cache
