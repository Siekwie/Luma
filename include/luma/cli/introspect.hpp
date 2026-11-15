#pragma once

#include "luma/cli/session.hpp"
#include "luma/executor/executor.hpp"
#include "luma/types.hpp"

#include <optional>
#include <string>

namespace luma::cli {

/// `luma targets --format json`: one JSON object describing the project.
std::string targets_json(const Session& session);

/// `luma targets`: human-readable table.
std::string targets_table(const Session& session);

/// Clang JSON compilation database for every compile task in the plan.
std::string compdb_json(const config::ProjectConfig& config, const config::BuildPlan& plan);

/// Stable fingerprint of everything that shapes the compilation database.
std::string compdb_fingerprint(const config::BuildPlan& plan, const std::string& compdb);

struct CompdbWriteResult {
    bool ok = false;
    /// False when the file was already current and left untouched.
    bool written = false;
    Path path;
    std::string error;
};

/// Write the compilation database to `output` (absolute path).
/// With `only_if_changed`, skip writing when the fingerprint recorded in
/// `<cache_dir>/compdb.stamp` matches and the file still exists.
CompdbWriteResult write_compdb(
    const config::ProjectConfig& config,
    const config::BuildPlan& plan,
    const Path& output,
    const Path& cache_dir,
    bool only_if_changed);

/// `{"total_ms":N,"tasks":[{"id","verb","desc","ms","output","ok"}...]}` on
/// one line, tasks sorted by duration (longest first).
std::string timings_json(std::int64_t total_ms, const std::vector<executor::TaskTiming>& timings);

/// Append one build record to `path` (JSON Lines), keeping only the last
/// `keep` records. The file is replaced atomically.
bool append_timings_log(
    const Path& path,
    std::int64_t unix_timestamp,
    const std::string& profile,
    const std::string& status,
    std::int64_t total_ms,
    const std::vector<executor::TaskTiming>& timings,
    std::size_t keep = 50);

} // namespace luma::cli
