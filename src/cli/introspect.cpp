#include "luma/cli/introspect.hpp"

#include "luma/filesystem/path_util.hpp"
#include "luma/filesystem/sha256.hpp"
#include "luma/util/json.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace luma::cli {

namespace fs = luma::fs;
namespace meta = config::meta;
using util::JsonWriter;

namespace {

std::vector<std::string> absolute_paths(const Path& root, const std::vector<Path>& paths) {
    std::vector<std::string> out;
    out.reserve(paths.size());
    for (const auto& path : paths) {
        out.push_back(fs::join(root, path));
    }
    return out;
}

std::string project_name(const config::ProjectConfig& config) {
    return config.name.empty() ? fs::filename(config.root) : config.name;
}

std::string metadata_value(const graph::Task& task, const char* key) {
    const auto it = task.metadata.find(key);
    return it == task.metadata.end() ? std::string{} : it->second;
}

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& name : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}

bool write_file_atomically(const Path& path, const std::string& content, std::string& error) {
    fs::create_directories(fs::parent(path));
    const Path temp = path + ".tmp";
    {
        std::ofstream out(fs::to_fs_path(temp), std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + temp;
            return false;
        }
        out << content;
        if (!out.flush()) {
            error = "cannot write " + temp;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(fs::to_fs_path(temp), fs::to_fs_path(path), ec);
    if (ec) {
        error = "cannot replace " + path + ": " + ec.message();
        std::filesystem::remove(fs::to_fs_path(temp), ec);
        return false;
    }
    return true;
}

std::string read_file(const Path& path) {
    std::ifstream in(fs::to_fs_path(path), std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<executor::TaskTiming> by_duration(std::vector<executor::TaskTiming> timings) {
    std::stable_sort(timings.begin(), timings.end(), [](const auto& a, const auto& b) {
        return a.ms != b.ms ? a.ms > b.ms : a.id < b.id;
    });
    return timings;
}

void write_timing_tasks(JsonWriter& json, const std::vector<executor::TaskTiming>& timings) {
    json.key("tasks").begin_array();
    for (const auto& timing : by_duration(timings)) {
        json.begin_object();
        json.field("id", timing.id);
        json.field("verb", timing.verb);
        json.field("desc", timing.description);
        json.key("ms").value(timing.ms);
        json.field("output", timing.output);
        json.field("ok", timing.ok);
        json.end_object();
    }
    json.end_array();
}

} // namespace

std::string timings_json(std::int64_t total_ms, const std::vector<executor::TaskTiming>& timings) {
    JsonWriter json;
    json.begin_object();
    json.key("total_ms").value(total_ms);
    write_timing_tasks(json, timings);
    json.end_object();
    return json.str();
}

bool append_timings_log(
    const Path& path,
    std::int64_t unix_timestamp,
    const std::string& profile,
    const std::string& status,
    std::int64_t total_ms,
    const std::vector<executor::TaskTiming>& timings,
    std::size_t keep) {
    JsonWriter json;
    json.begin_object();
    json.key("timestamp").value(unix_timestamp);
    json.field("profile", profile);
    json.field("status", status);
    json.key("total_ms").value(total_ms);
    write_timing_tasks(json, timings);
    json.end_object();

    std::vector<std::string> lines;
    {
        std::istringstream existing(read_file(path));
        std::string line;
        while (std::getline(existing, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
    }
    lines.push_back(json.str());
    const std::size_t first = lines.size() > keep ? lines.size() - keep : 0;

    std::string content;
    for (std::size_t i = first; i < lines.size(); ++i) {
        content += lines[i];
        content += '\n';
    }
    std::string error;
    return write_file_atomically(path, content, error);
}

std::string targets_json(const Session& session) {
    const auto& config = session.config;
    const auto& plan = session.plan;
    const Path& root = config.root;

    JsonWriter json;
    json.begin_object();
    json.field("project", project_name(config));
    json.field("root", root);
    json.field("build_dir", session.build_dir());
    json.field("active_profile", config.profile);
    json.key("profiles").string_array(session.profile_names());
    json.field("toolchain", plan.toolchain_identity);
    json.key("targets").begin_array();
    for (const auto& target : plan.targets) {
        json.begin_object();
        json.field("name", target.name);
        json.field("kind", config::kind_name(target.kind));
        json.field("language", target.language);
        json.field("output", fs::join(root, target.output));
        json.key("sources").string_array(absolute_paths(root, target.sources));
        json.key("include_dirs").string_array(absolute_paths(root, target.include_dirs));
        json.key("defines").string_array(target.defines);
        json.key("dependencies").string_array(target.dependencies);
        json.end_object();
    }
    json.end_array();
    json.end_object();
    return json.str() + "\n";
}

std::string targets_table(const Session& session) {
    const auto& config = session.config;
    const auto& plan = session.plan;

    std::ostringstream out;
    out << "Project:   " << project_name(config) << '\n';
    out << "Root:      " << config.root << '\n';
    out << "Build dir: " << session.build_dir() << '\n';
    out << "Profile:   " << config.profile << " (available: " << join_names(session.profile_names())
        << ")\n";
    out << "Toolchain: " << plan.toolchain_identity << "\n\n";

    struct Row {
        std::string name, kind, language, sources, output, dependencies;
    };
    std::vector<Row> rows;
    rows.push_back({"TARGET", "KIND", "LANG", "SOURCES", "OUTPUT", "DEPENDENCIES"});
    for (const auto& target : plan.targets) {
        rows.push_back({
            target.name,
            config::kind_name(target.kind),
            target.language,
            std::to_string(target.sources.size()),
            target.output,
            target.dependencies.empty() ? "-" : join_names(target.dependencies),
        });
    }

    std::array<std::size_t, 5> widths{};
    for (const auto& row : rows) {
        widths[0] = std::max(widths[0], row.name.size());
        widths[1] = std::max(widths[1], row.kind.size());
        widths[2] = std::max(widths[2], row.language.size());
        widths[3] = std::max(widths[3], row.sources.size());
        widths[4] = std::max(widths[4], row.output.size());
    }
    auto pad = [](const std::string& text, std::size_t width) {
        return text + std::string(width - text.size() + 2, ' ');
    };
    for (const auto& row : rows) {
        out << pad(row.name, widths[0]) << pad(row.kind, widths[1]) << pad(row.language, widths[2])
            << pad(row.sources, widths[3]) << pad(row.output, widths[4]) << row.dependencies << '\n';
    }
    if (plan.targets.empty()) {
        out << "(no targets)\n";
    }
    return out.str();
}

std::string compdb_json(const config::ProjectConfig& config, const config::BuildPlan& plan) {
    const Path& root = config.root;

    std::vector<const graph::Task*> compiles;
    for (const auto& [id, task] : plan.graph.tasks()) {
        if (metadata_value(task, meta::kRule) == "compile") {
            compiles.push_back(&task);
        }
    }
    std::sort(compiles.begin(), compiles.end(), [](const graph::Task* a, const graph::Task* b) {
        return a->id.value < b->id.value;
    });

    JsonWriter json(2);
    json.begin_array();
    for (const graph::Task* task : compiles) {
        json.begin_object();
        json.field("directory", task->working_directory ? *task->working_directory : root);
        json.field("file", fs::join(root, metadata_value(*task, meta::kSource)));
        json.key("arguments").string_array(task->command);
        json.field("output", task->outputs.empty() ? std::string{} : fs::join(root, task->outputs.front()));
        json.end_object();
    }
    json.end_array();
    return json.str() + "\n";
}

std::string compdb_fingerprint(const config::BuildPlan& plan, const std::string& compdb) {
    return fs::sha256_hex(plan.toolchain_identity + "\n" + compdb);
}

CompdbWriteResult write_compdb(
    const config::ProjectConfig& config,
    const config::BuildPlan& plan,
    const Path& output,
    const Path& cache_dir,
    bool only_if_changed) {
    CompdbWriteResult result;
    result.path = output;

    const std::string content = compdb_json(config, plan);
    const std::string stamp = compdb_fingerprint(plan, content) + "\n" + output + "\n";
    const Path stamp_path = fs::join(cache_dir, "compdb.stamp");

    if (only_if_changed && fs::is_regular_file(output) && read_file(stamp_path) == stamp) {
        result.ok = true;
        return result;
    }

    if (!write_file_atomically(output, content, result.error)) {
        return result;
    }
    std::string ignored;
    write_file_atomically(stamp_path, stamp, ignored);
    result.ok = true;
    result.written = true;
    return result;
}

} // namespace luma::cli
