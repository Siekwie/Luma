#include "luma/cache/cache_store.hpp"

#include "luma/filesystem/path_util.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace luma::cache {

namespace fs = luma::fs;

namespace {

constexpr const char* kDirtyPrefix = "dirty=";
constexpr const char* kEndMarker = "end";

std::vector<std::pair<Path, Hash>> sorted(const std::unordered_map<Path, Hash>& map) {
    std::vector<std::pair<Path, Hash>> items(map.begin(), map.end());
    std::sort(items.begin(), items.end());
    return items;
}

void write_entry(std::ostream& out, const std::string& task_id, const TaskCacheEntry& entry) {
    out << "[task:" << task_id << "]\n";
    out << "cache_key=" << entry.cache_key << '\n';
    for (const auto& [path, hash] : sorted(entry.input_hashes)) {
        out << "input=" << path << '|' << hash << '\n';
    }
    for (const auto& [path, hash] : sorted(entry.discovered_hashes)) {
        out << "dep=" << path << '|' << hash << '\n';
    }
}

bool split_record(const std::string& payload, Path& path, Hash& hash) {
    const auto sep = payload.rfind('|');
    if (sep == std::string::npos) {
        return false;
    }
    path = payload.substr(0, sep);
    hash = payload.substr(sep + 1);
    return true;
}

/// Parse manifest/journal text.
///
/// Manifest: "[task:id]" blocks separated by blank lines.
/// Journal: "dirty=id" lines and "[task:id] ... end" blocks, applied in
/// order. A journal block is only applied once its "end" line was written;
/// the last line counts only if it is newline-terminated, so a record cut
/// short by a crash is ignored.
void parse(const std::string& text, bool journal, std::unordered_map<std::string, TaskCacheEntry>& entries) {
    std::string current_task;
    TaskCacheEntry current_entry;

    auto reset = [&]() {
        current_task.clear();
        current_entry = {};
    };
    auto commit = [&]() {
        if (!current_task.empty()) {
            entries[current_task] = current_entry;
        }
        reset();
    };

    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t newline = text.find('\n', start);
        if (newline == std::string::npos && journal) {
            break; // incomplete final record
        }
        const std::size_t end = newline == std::string::npos ? text.size() : newline;
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            if (!journal) {
                commit();
            }
            continue;
        }
        if (journal && line.starts_with(kDirtyPrefix)) {
            reset();
            entries.erase(line.substr(std::string(kDirtyPrefix).size()));
            continue;
        }
        if (journal && line == kEndMarker) {
            commit();
            continue;
        }
        if (line.starts_with("[task:") && line.ends_with(']')) {
            if (journal) {
                reset(); // a block without "end" is discarded
            } else {
                commit();
            }
            current_task = line.substr(6, line.size() - 7);
            continue;
        }
        Path path;
        Hash hash;
        if (line.starts_with("cache_key=")) {
            current_entry.cache_key = line.substr(10);
        } else if (line.starts_with("input=") && split_record(line.substr(6), path, hash)) {
            current_entry.input_hashes[path] = hash;
        } else if (line.starts_with("dep=") && split_record(line.substr(4), path, hash)) {
            current_entry.discovered_hashes[path] = hash;
        }
    }
    if (!journal) {
        commit();
    }
}

std::string read_file(const Path& path) {
    std::ifstream in(fs::to_native(path), std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

} // namespace

CacheStore::CacheStore(Path cache_dir)
    : cache_dir_(fs::normalize(cache_dir)),
      manifest_path_(fs::join(cache_dir_, "manifest.txt")),
      journal_path_(fs::join(cache_dir_, "journal.txt")) {}

CacheStore::~CacheStore() = default;

const TaskCacheEntry* CacheStore::get(const graph::TaskId& id) const {
    const auto it = entries_.find(id.value);
    return it == entries_.end() ? nullptr : &it->second;
}

void CacheStore::put(const graph::TaskId& id, TaskCacheEntry entry) {
    if (journal_) {
        std::ostringstream record;
        write_entry(record, id.value, entry);
        record << kEndMarker << '\n';
        *journal_ << record.str() << std::flush;
    }
    entries_[id.value] = std::move(entry);
}

void CacheStore::invalidate(const graph::TaskId& id) {
    if (journal_) {
        *journal_ << kDirtyPrefix << id.value << '\n' << std::flush;
    }
    entries_.erase(id.value);
}

bool CacheStore::begin_journal() {
    fs::create_directories(cache_dir_);
    journal_ = std::make_unique<std::ofstream>(
        fs::to_native(journal_path_), std::ios::binary | std::ios::app);
    if (!*journal_) {
        journal_.reset();
        return false;
    }
    return true;
}

bool CacheStore::save() {
    fs::create_directories(cache_dir_);

    // Write sorted (stable diffs) to a temporary file, then rename it over the
    // manifest so an interrupted save never leaves a truncated cache behind.
    std::vector<std::string> ids;
    ids.reserve(entries_.size());
    for (const auto& [task_id, _] : entries_) {
        ids.push_back(task_id);
    }
    std::sort(ids.begin(), ids.end());

    const Path temp_path = manifest_path_ + ".tmp";
    {
        std::ofstream out(fs::to_native(temp_path), std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        for (const auto& task_id : ids) {
            write_entry(out, task_id, entries_.at(task_id));
            out << '\n';
        }
        if (!out.flush()) {
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(fs::to_native(temp_path), fs::to_native(manifest_path_), ec);
    if (ec) {
        return false;
    }

    // Everything in the journal is now part of the manifest. Replaying the
    // journal again (if we die before removing it) is harmless.
    journal_.reset();
    std::filesystem::remove(fs::to_native(journal_path_), ec);
    return true;
}

bool CacheStore::load() {
    entries_.clear();
    const bool has_manifest = fs::is_regular_file(manifest_path_);
    if (has_manifest) {
        parse(read_file(manifest_path_), false, entries_);
    }
    // A journal left behind by an interrupted build records which tasks were
    // started (their outputs may be partial) and which finished.
    if (fs::is_regular_file(journal_path_)) {
        parse(read_file(journal_path_), true, entries_);
    }
    return has_manifest;
}

} // namespace luma::cache
