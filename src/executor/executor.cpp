#include "luma/executor/executor.hpp"

#include "luma/executor/process.hpp"
#include "luma/filesystem/hash.hpp"
#include "luma/filesystem/path_util.hpp"
#include "luma/graph/task.hpp"

#include <iostream>
#include <unordered_map>

namespace luma::executor {

namespace fs = luma::fs;

namespace {

struct TaskStatus {
    std::vector<std::string> reasons;
    bool up_to_date = false;
    Hash cache_key;
    std::unordered_map<Path, Hash> input_hashes;
};

Path resolve_input_path(const Path& root, const Path& input) {
    if (fs::exists(input)) {
        return input;
    }
    const Path joined = fs::join(root, input);
    if (fs::exists(joined)) {
        return joined;
    }
    return input;
}

TaskStatus evaluate_task(
    const graph::Task& task,
    cache::CacheStore& cache,
    const config::BuildPlan& plan,
    const Path& root) {
    TaskStatus status;

    std::vector<Hash> input_hashes;
    for (const auto& input : task.inputs) {
        const Path path = resolve_input_path(root, input);
        if (const auto hash = fs::hash_file(path)) {
            status.input_hashes[input] = *hash;
            input_hashes.push_back(*hash);
        } else {
            status.reasons.push_back("missing or unreadable input: " + input);
        }
    }

    status.cache_key = graph::compute_cache_key(
        task, input_hashes, plan.toolchain_identity, plan.configuration_fingerprint);

    for (const auto& output : task.outputs) {
        const Path output_path = fs::join(root, output);
        if (!fs::exists(output_path)) {
            status.reasons.push_back("output missing: " + output);
        }
    }

    if (const cache::TaskCacheEntry* previous = cache.get(task.id)) {
        if (previous->cache_key != status.cache_key) {
            status.reasons.push_back("cache key changed");
        }
        for (const auto& [path, hash] : status.input_hashes) {
            const auto it = previous->input_hashes.find(path);
            if (it == previous->input_hashes.end() || it->second != hash) {
                status.reasons.push_back("input changed: " + path);
            }
        }
    } else if (task.has_command()) {
        status.reasons.push_back("no previous cache entry");
    }

    status.up_to_date = task.has_command() && status.reasons.empty();
    if (!task.has_command()) {
        status.up_to_date = true;
    }

    return status;
}

bool run_task(
    const config::ProjectConfig& config,
    const graph::Task& task,
    const TaskStatus& status,
    cache::CacheStore& cache,
    const ExecutorOptions& options) {
    if (status.up_to_date) {
        if (options.verbose) {
            std::cout << "skip " << task.id.value << '\n';
        }
        return true;
    }

    if (options.dry_run) {
        std::cout << "would run: " << format_command(task.command) << '\n';
        return true;
    }

    for (const auto& output : task.outputs) {
        const Path output_path = fs::join(config.root, output);
        const auto slash = output_path.find_last_of('/');
        if (slash != Path::npos) {
            fs::create_directories(output_path.substr(0, slash));
        }
    }

    if (options.verbose) {
        std::cout << "run " << format_command(task.command) << '\n';
    }

    const std::string cwd = task.working_directory ? *task.working_directory : config.root;
    const auto process = run_process(task.command, cwd);
    if (process.exit_code != 0) {
        std::cerr << "error[LUMA001]: failed to run task " << task.id.value << '\n';
        std::cerr << "  command: " << format_command(task.command) << '\n';
        if (!process.stdout_text.empty()) {
            std::cerr << process.stdout_text << '\n';
        }
        if (!process.stderr_text.empty()) {
            std::cerr << process.stderr_text << '\n';
        }
        return false;
    }

    cache.put(task.id, cache::TaskCacheEntry{
                           .cache_key = status.cache_key,
                           .input_hashes = status.input_hashes,
                       });
    return true;
}

} // namespace

ExecuteResult execute(
    const config::ProjectConfig& config,
    const config::BuildPlan& plan,
    cache::CacheStore& cache,
    const ExecutorOptions& options) {
    ExecuteResult result;
    const auto order = plan.graph.topological_order();
    if (!order) {
        result.exit_code = 1;
        return result;
    }

    for (const auto& task_id : *order) {
        const graph::Task* task = plan.graph.find_task(task_id);
        if (!task) {
            continue;
        }

        const TaskStatus status = evaluate_task(*task, cache, plan, config.root);
        result.explanations.push_back(TaskExplanation{
            .id = task_id,
            .would_run = task->has_command() && !status.up_to_date,
            .reasons = status.reasons,
        });

        if (!task->has_command()) {
            ++result.tasks_skipped;
            continue;
        }

        if (status.up_to_date) {
            ++result.tasks_skipped;
            continue;
        }

        if (!run_task(config, *task, status, cache, options)) {
            result.exit_code = 1;
            break;
        }
        ++result.tasks_run;
    }

    cache.save();
    return result;
}

std::vector<TaskExplanation> explain_target(
    const config::BuildPlan& plan,
    cache::CacheStore& cache,
    const std::string& target_name,
    const Path& root) {
    std::vector<TaskExplanation> explanations;
    const auto it = plan.target_tasks.find(target_name);
    if (it == plan.target_tasks.end()) {
        return explanations;
    }

    for (const auto& task_id : it->second) {
        const graph::Task* task = plan.graph.find_task(task_id);
        if (!task) {
            continue;
        }
        const TaskStatus status = evaluate_task(*task, cache, plan, root);
        explanations.push_back(TaskExplanation{
            .id = task_id,
            .would_run = !status.up_to_date,
            .reasons = status.reasons,
        });
    }
    return explanations;
}

} // namespace luma::executor
