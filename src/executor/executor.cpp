#include "luma/executor/executor.hpp"

#include "luma/executor/process.hpp"
#include "luma/filesystem/hash.hpp"
#include "luma/filesystem/path_util.hpp"
#include "luma/graph/task.hpp"

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
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
    const cache::CacheStore& cache,
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
    const ExecutorOptions& options,
    std::mutex& cache_mutex,
    std::mutex& log_mutex) {
    if (status.up_to_date) {
        if (options.verbose) {
            std::lock_guard lock(log_mutex);
            std::cout << "skip " << task.id.value << '\n';
        }
        return true;
    }

    if (options.dry_run) {
        std::lock_guard lock(log_mutex);
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
        std::lock_guard lock(log_mutex);
        std::cout << "run " << format_command(task.command) << '\n';
    }

    const std::string cwd = task.working_directory ? *task.working_directory : config.root;
    const auto process = run_process(task.command, cwd);
    if (process.exit_code != 0) {
        std::lock_guard lock(log_mutex);
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

    {
        std::lock_guard lock(cache_mutex);
        cache.put(task.id, cache::TaskCacheEntry{
                               .cache_key = status.cache_key,
                               .input_hashes = status.input_hashes,
                           });
    }
    return true;
}

struct ScheduleState {
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<graph::TaskId> ready;
    std::unordered_map<std::string, std::size_t> remaining_deps;
    std::unordered_map<std::string, std::vector<std::string>> dependents;
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<std::size_t> in_flight{0};
    std::size_t total = 0;
};

void worker_loop(
    ScheduleState& state,
    const config::ProjectConfig& config,
    const config::BuildPlan& plan,
    cache::CacheStore& cache,
    const ExecutorOptions& options,
    const std::unordered_map<std::string, TaskStatus>& statuses,
    std::mutex& cache_mutex,
    std::mutex& log_mutex,
    std::atomic<std::size_t>& tasks_run,
    std::atomic<std::size_t>& tasks_skipped) {
    while (true) {
        graph::TaskId task_id{""};

        {
            std::unique_lock lock(state.mutex);
            state.cv.wait(lock, [&]() {
                return state.stop.load() || !state.ready.empty();
            });

            if (state.stop.load() && state.ready.empty()) {
                return;
            }
            if (state.ready.empty()) {
                continue;
            }

            task_id = state.ready.front();
            state.ready.pop();
            ++state.in_flight;
        }

        const graph::Task* task = plan.graph.find_task(task_id);
        if (!task) {
            continue;
        }

        const TaskStatus& status = statuses.at(task_id.value);
        bool ok = true;

        if (!task->has_command()) {
            ++tasks_skipped;
        } else if (status.up_to_date) {
            ++tasks_skipped;
            ok = run_task(config, *task, status, cache, options, cache_mutex, log_mutex);
        } else {
            ok = run_task(config, *task, status, cache, options, cache_mutex, log_mutex);
            if (ok) {
                ++tasks_run;
            }
        }

        if (!ok) {
            state.failed = true;
            state.stop = true;
        }

        {
            std::lock_guard lock(state.mutex);
            if (ok) {
                for (const auto& dependent : state.dependents[task_id.value]) {
                    auto& deps = state.remaining_deps[dependent];
                    if (deps > 0) {
                        --deps;
                        if (deps == 0 && !state.stop.load()) {
                            state.ready.push(graph::TaskId{dependent});
                        }
                    }
                }
            }

            --state.in_flight;
            if (state.ready.empty() && state.in_flight.load() == 0) {
                state.stop = true;
            }
            state.cv.notify_all();
        }
    }
}

} // namespace

ExecuteResult execute(
    const config::ProjectConfig& config,
    const config::BuildPlan& plan,
    cache::CacheStore& cache,
    const ExecutorOptions& options) {
    ExecuteResult result;
    if (plan.graph.empty()) {
        return result;
    }

    if (!plan.graph.topological_order()) {
        result.exit_code = 1;
        return result;
    }

    std::unordered_map<std::string, TaskStatus> statuses;
    statuses.reserve(plan.graph.size());
    for (const auto& [id, task] : plan.graph.tasks()) {
        statuses.emplace(id.value, evaluate_task(task, cache, plan, config.root));
        result.explanations.push_back(TaskExplanation{
            .id = id,
            .would_run = task.has_command() && !statuses[id.value].up_to_date,
            .reasons = statuses[id.value].reasons,
        });
    }

    ScheduleState schedule;
    schedule.total = plan.graph.size();

    for (const auto& [id, task] : plan.graph.tasks()) {
        schedule.remaining_deps[id.value] = task.dependencies.size();
        for (const auto& dep : task.dependencies) {
            schedule.dependents[dep.value].push_back(id.value);
        }
        if (task.dependencies.empty()) {
            schedule.ready.push(id);
        }
    }

    const std::size_t jobs = options.jobs == 0
        ? std::max<std::size_t>(1, std::thread::hardware_concurrency())
        : std::max<std::size_t>(1, options.jobs);

    std::mutex cache_mutex;
    std::mutex log_mutex;
    std::atomic<std::size_t> tasks_run{0};
    std::atomic<std::size_t> tasks_skipped{0};

    std::vector<std::thread> workers;
    workers.reserve(jobs);
    for (std::size_t i = 0; i < jobs; ++i) {
        workers.emplace_back(worker_loop,
            std::ref(schedule),
            std::cref(config),
            std::cref(plan),
            std::ref(cache),
            std::cref(options),
            std::cref(statuses),
            std::ref(cache_mutex),
            std::ref(log_mutex),
            std::ref(tasks_run),
            std::ref(tasks_skipped));
    }

    for (auto& worker : workers) {
        worker.join();
    }

    result.tasks_run = tasks_run.load();
    result.tasks_skipped = tasks_skipped.load();
    result.exit_code = schedule.failed.load() ? 1 : 0;

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
