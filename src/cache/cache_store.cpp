#include "luma/cache/cache_store.hpp"

#include "luma/filesystem/path_util.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

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

    // Write sorted (stable diffs) to a temporary file, then rename it over the
    // manifest so an interrupted save never leaves a truncated cache behind.
    std::vector<std::string> ids;
    ids.reserve(entries_.size());
    for (const auto& [task_id, _] : entries_) {
        ids.push_back(task_id);
    }
    std::sort(ids.begin(), ids.end());

    auto sorted = [](const std::unordered_map<Path, Hash>& map) {
        std::vector<std::pair<Path, Hash>> items(map.begin(), map.end());
        std::sort(items.begin(), items.end());
        return items;
    };

    const Path temp_path = manifest_path_ + ".tmp";
    {
        std::ofstream out(fs::to_native(temp_path), std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        for (const auto& task_id : ids) {
            const TaskCacheEntry& entry = entries_.at(task_id);
            out << "[task:" << task_id << "]\n";
            out << "cache_key=" << entry.cache_key << '\n';
            for (const auto& [path, hash] : sorted(entry.input_hashes)) {
                out << "input=" << path << '|' << hash << '\n';
            }
            for (const auto& [path, hash] : sorted(entry.discovered_hashes)) {
                out << "dep=" << path << '|' << hash << '\n';
            }
            out << '\n';
        }
        if (!out.flush()) {
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(fs::to_native(temp_path), fs::to_native(manifest_path_), ec);
    return !ec;
}

bool CacheStore::load() {
    entries_.clear();
    std::ifstream in(fs::to_native(manifest_path_), std::ios::binary);
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
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
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
            const auto sep = payload.rfind('|');
            if (sep != std::string::npos) {
                current_entry.input_hashes[payload.substr(0, sep)] = payload.substr(sep + 1);
            }
            continue;
        }
        if (line.starts_with("dep=")) {
            const auto payload = line.substr(4);
            const auto sep = payload.rfind('|');
            if (sep != std::string::npos) {
                current_entry.discovered_hashes[payload.substr(0, sep)] = payload.substr(sep + 1);
            }
        }
    }
    flush();
    return true;
}

} // namespace luma::cache
