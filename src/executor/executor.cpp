#include "luma/executor/executor.hpp"

#include "luma/executor/depfile.hpp"
#include "luma/executor/process.hpp"
#include "luma/filesystem/hash.hpp"
#include "luma/filesystem/path_util.hpp"
#include "luma/graph/task.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace luma::executor {

namespace fs = luma::fs;
namespace meta = config::meta;

namespace {

std::string metadata_value(const graph::Task& task, const char* key) {
    const auto it = task.metadata.find(key);
    return it == task.metadata.end() ? std::string{} : it->second;
}

/// Memoizes content hashes of discovered dependencies (headers): many
/// translation units include the same files.
class HashMemo {
public:
    std::optional<Hash> hash(const Path& path) {
        {
            std::lock_guard lock(mutex_);
            const auto it = hashes_.find(path);
            if (it != hashes_.end()) {
                return it->second;
            }
        }
        auto value = fs::hash_file(path);
        std::lock_guard lock(mutex_);
        hashes_[path] = value;
        return value;
    }

    void forget(const Path& path) {
        std::lock_guard lock(mutex_);
        hashes_.erase(path);
    }

private:
    std::mutex mutex_;
    std::unordered_map<Path, std::optional<Hash>> hashes_;
};

struct TaskStatus {
    std::vector<std::string> reasons;
    bool up_to_date = false;
    Hash cache_key;
    std::unordered_map<Path, Hash> input_hashes;
};

TaskStatus evaluate_task(
    const graph::Task& task,
    const cache::TaskCacheEntry* previous,
    const config::BuildPlan& plan,
    const Path& root,
    HashMemo& memo) {
    TaskStatus status;

    std::vector<Hash> input_hashes;
    for (const auto& input : task.inputs) {
        if (const auto hash = fs::hash_file(fs::join(root, input))) {
            status.input_hashes[input] = *hash;
            input_hashes.push_back(*hash);
        } else {
            status.reasons.push_back("missing or unreadable input: " + input);
        }
    }

    status.cache_key = graph::compute_cache_key(
        task, input_hashes, plan.toolchain_identity, plan.configuration_fingerprint);

    for (const auto& output : task.outputs) {
        if (!fs::exists(fs::join(root, output))) {
            status.reasons.push_back("output missing: " + output);
        }
    }

    if (previous) {
        if (previous->cache_key != status.cache_key) {
            status.reasons.push_back("command, toolchain or inputs changed");
        }
        for (const auto& [path, hash] : status.input_hashes) {
            const auto it = previous->input_hashes.find(path);
            if (it == previous->input_hashes.end() || it->second != hash) {
                status.reasons.push_back("input changed: " + path);
            }
        }
        for (const auto& [path, hash] : previous->discovered_hashes) {
            const auto current = memo.hash(fs::join(root, path));
            if (!current) {
                status.reasons.push_back("dependency missing: " + path);
            } else if (*current != hash) {
                status.reasons.push_back("dependency changed: " + path);
            }
        }
    } else if (task.has_command()) {
        status.reasons.push_back("no previous cache entry");
    }

    status.up_to_date = !task.has_command() || status.reasons.empty();
    return status;
}

/// CRLF -> LF, and guarantee a trailing newline for non-empty text.
std::string normalize_output(std::string text) {
    std::string out;
    out.reserve(text.size() + 1);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue;
        }
        out.push_back(text[i]);
    }
    if (!out.empty() && out.back() != '\n') {
        out.push_back('\n');
    }
    return out;
}

std::string msvc_deps_prefix() {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t length = 0;
    std::string prefix;
    if (_dupenv_s(&value, &length, "LUMA_MSVC_DEPS_PREFIX") == 0 && value) {
        prefix = value;
        std::free(value);
    }
#else
    const char* raw = std::getenv("LUMA_MSVC_DEPS_PREFIX");
    std::string prefix = raw ? raw : "";
#endif
    return prefix.empty() ? std::string(kMsvcShowIncludesPrefix) : prefix;
}

struct Scheduler {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> ready;
    std::unordered_map<std::string, std::size_t> remaining_deps;
    std::unordered_map<std::string, std::vector<std::string>> dependents;
    std::size_t in_flight = 0;
    bool failed = false;
};

struct RunContext {
    RunContext(
        const config::ProjectConfig& config_,
        const config::BuildPlan& plan_,
        cache::CacheStore& cache_,
        const ExecutorOptions& options_,
        const std::unordered_map<std::string, bool>& planned_,
        std::size_t planned_total_,
        HashMemo& memo_)
        : config(config_), plan(plan_), cache(cache_), options(options_), planned(planned_),
          planned_total(planned_total_), memo(memo_) {}

    const config::ProjectConfig& config;
    const config::BuildPlan& plan;
    cache::CacheStore& cache;
    const ExecutorOptions& options;
    const std::unordered_map<std::string, bool>& planned;
    std::size_t planned_total = 0;
    HashMemo& memo;

    std::mutex cache_mutex;
    std::mutex log_mutex;
    std::size_t progress = 0;
    std::size_t tasks_run = 0;
    std::size_t tasks_skipped = 0;
    std::size_t tasks_failed = 0;
    std::vector<TaskTiming> timings;
};

/// Print one task's progress block atomically.
void report(RunContext& ctx, const graph::Task& task, const std::string& output) {
    std::string verb = metadata_value(task, meta::kVerb);
    std::string description = metadata_value(task, meta::kDescription);
    if (verb.empty()) {
        verb = "run";
    }
    if (description.empty()) {
        description = task.id.value;
    }

    std::lock_guard lock(ctx.log_mutex);
    ++ctx.progress;
    std::string block = "[" + std::to_string(ctx.progress) + "/" + std::to_string(ctx.planned_total) +
                        "] " + verb + " " + description + "\n";
    if (ctx.options.verbose || ctx.options.dry_run) {
        block += format_command(task.command) + "\n";
    }
    block += output;
    std::cout << block << std::flush;
}

/// Run one out-of-date task. Returns false if its command failed.
bool run_task(RunContext& ctx, const graph::Task& task) {
    const Path& root = ctx.config.root;

    if (ctx.options.dry_run) {
        report(ctx, task, {});
        return true;
    }

    // Re-evaluate now that all dependencies have finished: their outputs are
    // this task's inputs and the cache entry must record their new hashes.
    std::optional<cache::TaskCacheEntry> previous;
    {
        std::lock_guard lock(ctx.cache_mutex);
        if (const cache::TaskCacheEntry* entry = ctx.cache.get(task.id)) {
            previous = *entry;
        }
    }
    const TaskStatus status =
        evaluate_task(task, previous ? &*previous : nullptr, ctx.plan, root, ctx.memo);

    // Record (durably, via the cache journal) that this task's outputs are
    // about to change, so an interrupted build reruns it next time.
    {
        std::lock_guard lock(ctx.cache_mutex);
        ctx.cache.invalidate(task.id);
    }

    for (const auto& output : task.outputs) {
        const Path output_path = fs::join(root, output);
        // Never let a stale output survive a failed or partial run (and keep
        // `ar` from appending to an old archive).
        fs::remove_all(output_path);
        fs::create_directories(fs::parent(output_path));
        ctx.memo.forget(output_path);
    }

    const std::string cwd = task.working_directory ? *task.working_directory : root;
    const auto started = std::chrono::steady_clock::now();
    const ProcessResult process = run_process(task.command, cwd, /*merge_output=*/true);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    const std::string deps_format = metadata_value(task, meta::kDepsFormat);
    std::string output = process.stdout_text;
    std::vector<Path> discovered;

    if (deps_format == "msvc") {
        const std::string source = metadata_value(task, meta::kSource);
        auto scan = scan_msvc_output(output, fs::filename(source), msvc_deps_prefix());
        output = std::move(scan.output);
        discovered = std::move(scan.includes);
    }
    output = normalize_output(std::move(output));

    const bool ok = process.exit_code == 0;
    report(ctx, task, output);
    {
        std::string verb = metadata_value(task, meta::kVerb);
        std::string description = metadata_value(task, meta::kDescription);
        std::lock_guard lock(ctx.log_mutex);
        ctx.timings.push_back(TaskTiming{
            .id = task.id.value,
            .verb = verb.empty() ? "run" : verb,
            .description = description.empty() ? task.id.value : description,
            .output = task.outputs.empty() ? Path{} : fs::join(root, task.outputs.front()),
            .ms = static_cast<std::int64_t>(elapsed_ms),
            .ok = ok,
        });
    }

    if (!ok) {
        if (cancellation_requested()) {
            return false; // killed by us; not worth an error report
        }
        std::lock_guard lock(ctx.log_mutex);
        std::cerr << "luma: error: task '" << task.id.value << "' failed with exit code "
                  << process.exit_code << "\n  command: " << format_command(task.command) << '\n'
                  << std::flush;
        return false;
    }

    if (deps_format == "gcc") {
        const Path depfile = fs::join(root, metadata_value(task, meta::kDepfile));
        std::ifstream in(fs::to_native(depfile), std::ios::binary);
        if (in) {
            const std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            discovered = parse_gcc_depfile(content);
        }
    }

    cache::TaskCacheEntry entry{
        .cache_key = status.cache_key,
        .input_hashes = status.input_hashes,
        .discovered_hashes = {},
    };
    const std::string source = metadata_value(task, meta::kSource);
    const Path source_abs = source.empty() ? Path{} : fs::join(root, source);
    for (const auto& dep : discovered) {
        const Path dep_abs = fs::join(root, dep);
        if (dep_abs == source_abs) {
            continue;
        }
        if (const auto hash = ctx.memo.hash(dep_abs)) {
            entry.discovered_hashes[dep] = *hash;
        }
    }

    std::lock_guard lock(ctx.cache_mutex);
    ctx.cache.put(task.id, std::move(entry));
    return true;
}

void worker_loop(Scheduler& sched, RunContext& ctx) {
    while (true) {
        std::string task_id;
        {
            std::unique_lock lock(sched.mutex);
            sched.cv.wait(lock, [&]() {
                return (!sched.ready.empty() && !sched.failed) || sched.in_flight == 0;
            });
            if (cancellation_requested()) {
                sched.failed = true;
            }
            if (sched.failed || sched.ready.empty()) {
                // Either everything reachable has finished, or a task failed
                // and no new work may start.
                sched.cv.notify_all();
                return;
            }
            task_id = std::move(sched.ready.front());
            sched.ready.pop_front();
            ++sched.in_flight;
        }

        const graph::Task* task = ctx.plan.graph.find_task(graph::TaskId{task_id});
        bool ok = true;
        if (task && ctx.planned.at(task_id)) {
            ok = run_task(ctx, *task);
            std::lock_guard lock(ctx.log_mutex);
            if (ok) {
                ++ctx.tasks_run;
            } else {
                ++ctx.tasks_failed;
            }
        } else {
            std::lock_guard lock(ctx.log_mutex);
            ++ctx.tasks_skipped;
        }

        {
            std::lock_guard lock(sched.mutex);
            --sched.in_flight;
            if (ok) {
                for (const auto& dependent : sched.dependents[task_id]) {
                    if (--sched.remaining_deps[dependent] == 0) {
                        sched.ready.push_back(dependent);
                    }
                }
            } else {
                sched.failed = true;
            }
        }
        sched.cv.notify_all();
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

    const auto order = plan.graph.topological_order();
    if (!order) {
        std::cerr << "luma: error: the build graph contains a cycle\n";
        result.exit_code = 1;
        return result;
    }

    HashMemo memo;

    // A task runs if it is out of date itself or if anything it depends on
    // runs. Deciding this up front gives a fixed task count for progress.
    std::unordered_map<std::string, bool> planned;
    std::size_t planned_total = 0;
    for (const auto& id : *order) {
        const graph::Task& task = *plan.graph.find_task(id);
        const TaskStatus status = evaluate_task(task, cache.get(id), plan, config.root, memo);
        bool runs = task.has_command() && !status.up_to_date;
        std::vector<std::string> reasons = status.reasons;
        for (const auto& dep : task.dependencies) {
            if (planned.at(dep.value)) {
                if (!runs && task.has_command()) {
                    reasons.push_back("dependency will be rebuilt: " + dep.value);
                }
                runs = runs || task.has_command();
            }
        }
        planned[id.value] = runs;
        planned_total += runs ? 1 : 0;
        result.explanations.push_back(TaskExplanation{
            .id = id,
            .would_run = runs,
            .reasons = std::move(reasons),
        });
    }

    Scheduler sched;
    for (const auto& id : *order) {
        const graph::Task& task = *plan.graph.find_task(id);
        sched.remaining_deps[id.value] = task.dependencies.size();
        for (const auto& dep : task.dependencies) {
            sched.dependents[dep.value].push_back(id.value);
        }
        if (task.dependencies.empty()) {
            sched.ready.push_back(id.value);
        }
    }

    const std::size_t jobs = options.jobs == 0
        ? std::max<std::size_t>(1, std::thread::hardware_concurrency())
        : options.jobs;

    RunContext ctx(config, plan, cache, options, planned, planned_total, memo);

    // Turn Ctrl+C / SIGTERM into an orderly shutdown that still saves the
    // cache, and journal cache updates so even SIGKILL cannot corrupt it.
    std::optional<CancellationScope> cancellation;
    if (!options.dry_run) {
        cancellation.emplace();
        cache.begin_journal();
    }

    std::vector<std::thread> workers;
    const std::size_t worker_count = std::min(jobs, std::max<std::size_t>(1, planned_total));
    workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers.emplace_back(worker_loop, std::ref(sched), std::ref(ctx));
    }
    for (auto& worker : workers) {
        worker.join();
    }

    result.tasks_run = ctx.tasks_run;
    result.tasks_skipped = ctx.tasks_skipped;
    result.tasks_failed = ctx.tasks_failed;
    result.timings = std::move(ctx.timings);
    result.cancelled = cancellation_requested();
    result.exit_code = result.cancelled ? cancellation_exit_code() : (sched.failed ? 1 : 0);

    if (!options.dry_run) {
        cache.save();
    }
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

    HashMemo memo;
    for (const auto& task_id : it->second) {
        const graph::Task* task = plan.graph.find_task(task_id);
        if (!task) {
            continue;
        }
        const TaskStatus status = evaluate_task(*task, cache.get(task_id), plan, root, memo);
        explanations.push_back(TaskExplanation{
            .id = task_id,
            .would_run = !status.up_to_date,
            .reasons = status.reasons,
        });
    }
    return explanations;
}

} // namespace luma::executor
