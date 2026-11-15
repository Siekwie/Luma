#include "luma/cli/commands.hpp"

#include "luma/cache/cache_store.hpp"
#include "luma/cli/introspect.hpp"
#include "luma/cli/session.hpp"
#include "luma/executor/executor.hpp"
#include "luma/executor/process.hpp"
#include "luma/filesystem/path_util.hpp"

#include "luma/util/json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <mutex>
#include <thread>
#include <vector>

#ifndef LUMA_VERSION
#define LUMA_VERSION "0.0.0"
#endif

namespace luma::cli {

namespace fs = luma::fs;

namespace {

void print_usage(std::ostream& out) {
    out << "Luma - a build graph engine with Lua configuration\n\n"
        << "Usage:\n"
        << "  luma build [target...] [--profile P] [-j N] [--dry-run] [--verbose] [--no-compdb] [--timings]\n"
        << "  luma run <target> [--profile P] [-j N] [--no-compdb] [--timings] [-- args...]\n"
        << "  luma test [target...] [--profile P] [-j N] [--timeout S] [--format text|json] [--verbose] [-- args...]\n"
        << "  luma targets [--format table|json] [--profile P]\n"
        << "  luma compdb [--profile P] [--output PATH]\n"
        << "  luma init [name]\n"
        << "  luma clean\n"
        << "  luma graph [--format dot] [--profile P]\n"
        << "  luma explain <target> [--profile P]\n"
        << "  luma inspect <target> [--profile P]\n"
        << "  luma --version\n"
        << "  luma --help\n";
}

struct ParsedArgs {
    std::string command;
    std::string profile;
    std::string format;
    std::string output;
    std::string timeout;
    std::vector<std::string> positionals;
    std::vector<std::string> passthrough;
    bool dry_run = false;
    bool verbose = false;
    bool no_compdb = false;
    bool timings = false;
    std::size_t jobs = 0;
    std::string error;
};

bool parse_jobs(std::string_view text, std::size_t& jobs) {
    if (text.empty()) {
        return false;
    }
    std::size_t value = 0;
    for (const char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
        value = value * 10 + static_cast<std::size_t>(c - '0');
        if (value > 4096) {
            return false;
        }
    }
    jobs = value;
    return true;
}

ParsedArgs parse_args(int argc, char** argv) {
    ParsedArgs args;
    if (argc < 2) {
        return args;
    }
    args.command = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];

        // Options taking a value accept both "--opt value" and "--opt=value".
        auto take_value = [&](std::string_view name, std::string& out) {
            if (arg == name) {
                if (i + 1 >= argc) {
                    args.error = "missing value for " + std::string(name);
                    return true;
                }
                out = argv[++i];
                return true;
            }
            if (arg.size() > name.size() && arg.starts_with(name) && arg[name.size()] == '=') {
                out = std::string(arg.substr(name.size() + 1));
                return true;
            }
            return false;
        };

        std::string jobs_text;
        if (arg == "--") {
            for (int k = i + 1; k < argc; ++k) {
                args.passthrough.emplace_back(argv[k]);
            }
            break;
        }
        if (take_value("--profile", args.profile) || take_value("--format", args.format) ||
            take_value("--output", args.output) || take_value("-o", args.output) ||
            take_value("--timeout", args.timeout)) {
        } else if (take_value("--jobs", jobs_text) || take_value("-j", jobs_text) ||
                   (arg.starts_with("-j") && arg.size() > 2 && (jobs_text = arg.substr(2), true))) {
            if (args.error.empty() && !parse_jobs(jobs_text, args.jobs)) {
                args.error = "invalid job count: " + jobs_text;
            }
        } else if (arg == "--dry-run" || arg == "-n") {
            args.dry_run = true;
        } else if (arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "--no-compdb") {
            args.no_compdb = true;
        } else if (arg == "--timings") {
            args.timings = true;
        } else if (arg.starts_with("-") && arg.size() > 1) {
            args.error = "unknown option: " + std::string(arg);
        } else {
            args.positionals.emplace_back(arg);
        }
        if (!args.error.empty()) {
            break;
        }
    }
    return args;
}

int fail(const std::string& message) {
    std::cout.flush();
    std::cerr << "luma: error: " << message << '\n';
    return 1;
}

std::optional<SessionError> load_session(
    Session& session, const ParsedArgs& args, const std::vector<std::string>& targets = {}) {
    SessionOptions options;
    options.project_root = ".";
    options.profile = args.profile;
    options.targets = targets;
    return Session::load(session, options);
}

/// Refresh <root>/compile_commands.json when its fingerprint changed.
void refresh_compdb(const Session& session, bool verbose) {
    const config::BuildPlan* plan = &session.plan;
    config::BuildPlan full_plan;
    if (session.plan.targets.size() != session.config.targets.size()) {
        // A partial build still records every target's compile commands.
        if (config::build_plan(session.config, full_plan)) {
            return;
        }
        plan = &full_plan;
    }
    const Path output = fs::join(session.config.root, "compile_commands.json");
    const auto result = write_compdb(session.config, *plan, output, session.cache_dir, true);
    if (!result.ok) {
        std::cerr << "luma: warning: could not update compile_commands.json: " << result.error << '\n';
    } else if (result.written && verbose) {
        std::cout << "luma: wrote " << result.path << '\n';
    }
}

std::string format_seconds(std::chrono::steady_clock::duration elapsed) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2fs", seconds);
    return buffer;
}

/// Shared by `build` and `run`. Prints the final status line.
int build_session(Session& session, const ParsedArgs& args,
                  std::chrono::steady_clock::time_point start) {
    if (!args.no_compdb && !args.dry_run) {
        refresh_compdb(session, args.verbose);
    }

    cache::CacheStore cache(session.cache_dir);
    cache.load();

    const executor::ExecutorOptions options{
        .dry_run = args.dry_run,
        .verbose = args.verbose,
        .jobs = args.jobs,
    };
    const auto result = executor::execute(session.config, session.plan, cache, options);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const std::int64_t total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    if (args.dry_run) {
        if (result.exit_code != 0) {
            std::cout << "luma: build failed" << std::endl;
            return result.exit_code;
        }
        std::cout << "luma: dry run (" << result.tasks_run << " would run, " << result.tasks_skipped
                  << " up to date)" << std::endl;
        return 0;
    }

    const std::string status = result.cancelled ? "cancelled" : (result.exit_code != 0 ? "failed" : "succeeded");
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    append_timings_log(
        fs::join(session.config.root, ".luma/timings.jsonl"),
        now, session.config.profile, status, total_ms, result.timings);

    if (result.cancelled) {
        std::cerr << "luma: build cancelled\n";
    }
    if (result.exit_code != 0) {
        std::cout << "luma: build failed" << std::endl;
    } else {
        std::cout << "luma: build succeeded (" << result.tasks_run << " run, " << result.tasks_skipped
                  << " up to date) in " << format_seconds(elapsed) << std::endl;
    }
    if (args.timings) {
        std::cout << "luma: timings " << timings_json(total_ms, result.timings) << std::endl;
    }
    return result.exit_code;
}

/// For builds that fail before any task runs (configuration errors).
void print_failed_build(const ParsedArgs& args, std::chrono::steady_clock::time_point start) {
    std::cout << "luma: build failed" << std::endl;
    if (args.timings) {
        const std::int64_t total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "luma: timings " << timings_json(total_ms, {}) << std::endl;
    }
}

int cmd_build(const ParsedArgs& args) {
    const auto start = std::chrono::steady_clock::now();
    if (!args.passthrough.empty()) {
        return fail("'luma build' does not take arguments after --");
    }
    Session session;
    if (const auto err = load_session(session, args, args.positionals)) {
        fail(err->message);
        print_failed_build(args, start);
        return 1;
    }
    return build_session(session, args, start);
}

int cmd_run(const ParsedArgs& args) {
    const auto start = std::chrono::steady_clock::now();
    if (args.positionals.empty()) {
        return fail("usage: luma run <target> [--profile P] [-- args...]");
    }
    if (args.positionals.size() > 1) {
        return fail("unexpected argument '" + args.positionals[1] +
                    "' (pass program arguments after --)");
    }
    if (args.dry_run) {
        return fail("'luma run' does not support --dry-run");
    }
    const std::string& target_name = args.positionals.front();

    Session session;
    if (const auto err = load_session(session, args, {target_name})) {
        fail(err->message);
        print_failed_build(args, start);
        return 1;
    }

    const config::TargetInfo* target = nullptr;
    for (const auto& info : session.plan.targets) {
        if (info.name == target_name) {
            target = &info;
        }
    }
    if (!target || target->kind != config::TargetKind::Executable) {
        return fail("target '" + target_name + "' is not an executable");
    }

    if (const int code = build_session(session, args, start); code != 0) {
        return code;
    }

    std::vector<std::string> command = {fs::to_native(fs::join(session.config.root, target->output))};
    command.insert(command.end(), args.passthrough.begin(), args.passthrough.end());
    std::cout.flush();
    std::cerr.flush();
    return executor::run_process_interactive(command);
}

/// `luma test`: build the test targets (test = true, or the named ones), run
/// them from the project root, report pass/fail. Output of failing tests is
/// printed unchanged so file:line references stay usable.
int cmd_test(const ParsedArgs& args) {
    const auto start = std::chrono::steady_clock::now();
    const std::string format = args.format.empty() ? "text" : args.format;
    if (format != "text" && format != "json") {
        return fail("unknown format '" + format + "' (expected text or json)");
    }
    if (args.dry_run) {
        return fail("'luma test' does not support --dry-run");
    }
    double timeout = 300;
    if (!args.timeout.empty()) {
        char* end = nullptr;
        timeout = std::strtod(args.timeout.c_str(), &end);
        if (end == args.timeout.c_str() || *end != '\0' || timeout < 0) {
            return fail("invalid --timeout: " + args.timeout + " (seconds, 0 = none)");
        }
    }

    // Load everything once to find the test targets, then plan just those.
    std::vector<std::string> names = args.positionals;
    if (names.empty()) {
        Session all;
        if (const auto err = load_session(all, args)) {
            return fail(err->message);
        }
        for (const auto& t : all.plan.targets) {
            if (t.test) {
                names.push_back(t.name);
            }
        }
        if (names.empty()) {
            std::cout << "luma: no test targets (mark executables with test = true in luma.lua)" << std::endl;
            return 0;
        }
    }
    Session session;
    if (const auto err = load_session(session, args, names)) {
        fail(err->message);
        print_failed_build(args, start);
        return 1;
    }
    std::vector<const config::TargetInfo*> tests;
    for (const auto& name : names) {
        const config::TargetInfo* found = nullptr;
        for (const auto& t : session.plan.targets) {
            if (t.name == name) {
                found = &t;
            }
        }
        if (!found || found->kind != config::TargetKind::Executable) {
            return fail("target '" + name + "' is not an executable");
        }
        tests.push_back(found);
    }
    if (const int code = build_session(session, args, start); code != 0) {
        return code;
    }

    struct Result {
        std::string name, status, output;
        int exit_code = 0;
        std::int64_t ms = 0;
    };
    const std::size_t jobs = args.jobs ? args.jobs : std::max<std::size_t>(1, std::thread::hardware_concurrency());
    std::atomic<std::size_t> next{0};
    std::mutex print_mutex;
    std::size_t finished = 0, passed = 0;
    std::vector<std::string> failed_names;
    const auto tests_start = std::chrono::steady_clock::now();
    auto report = [&](const Result& r) {
        std::lock_guard lock(print_mutex);
        ++finished;
        if (r.status == "passed") {
            ++passed;
        } else {
            failed_names.push_back(r.name);
        }
        if (format == "json") {
            util::JsonWriter json;
            json.begin_object();
            json.field("name", r.name);
            json.field("status", r.status);
            json.key("exit_code").value(static_cast<std::int64_t>(r.exit_code));
            json.key("ms").value(r.ms);
            json.field("output", r.output);
            json.end_object();
            std::cout << "luma: test " << json.str() << std::endl;
            return;
        }
        char line[64];
        std::snprintf(line, sizeof(line), "[%zu/%zu] ", finished, tests.size());
        char secs[32];
        std::snprintf(secs, sizeof(secs), "%.2fs", static_cast<double>(r.ms) / 1000.0);
        std::cout << line << (r.status == "passed" ? "PASS " : "FAIL ") << r.name << " (" << secs;
        if (r.status == "timeout") {
            std::cout << ", timed out";
        } else if (r.status != "passed") {
            std::cout << ", exit code " << r.exit_code;
        }
        std::cout << ")\n";
        if (r.status != "passed" || args.verbose) {
            std::cout << r.output;
            if (!r.output.empty() && r.output.back() != '\n') {
                std::cout << '\n';
            }
        }
        std::cout.flush();
    };
    auto worker = [&]() {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= tests.size() || executor::cancellation_requested()) {
                return;
            }
            const auto* t = tests[i];
            std::vector<std::string> command = {fs::to_native(fs::join(session.config.root, t->output))};
            command.insert(command.end(), args.passthrough.begin(), args.passthrough.end());
            const auto t0 = std::chrono::steady_clock::now();
            const auto pr = executor::run_process(command, session.config.root, true, timeout);
            Result r;
            r.name = t->name;
            r.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            r.exit_code = pr.exit_code;
            r.output = pr.stdout_text + pr.stderr_text;
            r.status = pr.timed_out ? "timeout" : pr.exit_code == 0 ? "passed" : pr.exit_code < 0 ? "error" : "failed";
            report(r);
        }
    };
    {
        executor::CancellationScope cancel_scope;
        std::vector<std::thread> pool;
        for (std::size_t k = 0; k < std::min(jobs, tests.size()); ++k) {
            pool.emplace_back(worker);
        }
        for (auto& th : pool) {
            th.join();
        }
    }
    const std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tests_start).count();
    const std::size_t failed = failed_names.size();
    if (format == "json") {
        util::JsonWriter json;
        json.begin_object();
        json.key("passed").value(static_cast<std::int64_t>(passed));
        json.key("failed").value(static_cast<std::int64_t>(failed));
        json.key("ms").value(ms);
        json.end_object();
        std::cout << "luma: tests " << json.str() << std::endl;
    } else {
        char secs[32];
        std::snprintf(secs, sizeof(secs), "%.2fs", ms / 1000.0);
        std::cout << "luma: tests: " << passed << " passed, " << failed << " failed in " << secs;
        if (failed) {
            std::sort(failed_names.begin(), failed_names.end());
            std::cout << " (";
            for (std::size_t i = 0; i < failed_names.size(); ++i) {
                std::cout << (i ? ", " : "") << failed_names[i];
            }
            std::cout << ")";
        }
        std::cout << std::endl;
    }
    if (executor::cancellation_requested()) {
        return executor::cancellation_exit_code();
    }
    return failed ? 1 : 0;
}

int cmd_targets(const ParsedArgs& args) {
    const std::string format = args.format.empty() ? "table" : args.format;
    if (format != "json" && format != "table") {
        return fail("unsupported format for targets: " + format + " (expected json or table)");
    }
    if (!args.positionals.empty()) {
        return fail("unexpected argument: " + args.positionals.front());
    }
    Session session;
    if (const auto err = load_session(session, args)) {
        return fail(err->message);
    }
    std::cout << (format == "json" ? targets_json(session) : targets_table(session)) << std::flush;
    return 0;
}

int cmd_compdb(const ParsedArgs& args) {
    if (!args.positionals.empty()) {
        return fail("unexpected argument: " + args.positionals.front());
    }
    Session session;
    if (const auto err = load_session(session, args)) {
        return fail(err->message);
    }
    const Path output = args.output.empty() ? fs::join(session.config.root, "compile_commands.json")
                                            : fs::absolute(args.output);
    const auto result = write_compdb(session.config, session.plan, output, session.cache_dir, false);
    if (!result.ok) {
        return fail(result.error);
    }
    std::cout << result.path << std::endl;
    return 0;
}

bool valid_project_name(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    for (const char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

bool write_new_file(const Path& path, const std::string& content) {
    std::ofstream out(fs::to_fs_path(path), std::ios::binary);
    out << content;
    return static_cast<bool>(out.flush());
}

int cmd_init(const ParsedArgs& args) {
    if (args.positionals.size() > 1) {
        return fail("usage: luma init [name]");
    }
    const Path dir = fs::absolute(".");

    std::string name;
    if (!args.positionals.empty()) {
        name = args.positionals.front();
        if (!valid_project_name(name)) {
            return fail("invalid project name '" + name + "' (use letters, digits, '_', '-', '.')");
        }
    } else {
        name = fs::filename(dir);
        for (char& c : name) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') {
                c = '_';
            }
        }
        if (!valid_project_name(name)) {
            name = "app";
        }
    }

    const Path config_path = fs::join(dir, "luma.lua");
    const Path main_path = fs::join(dir, "src/main.cpp");
    for (const Path& path : {config_path, main_path}) {
        if (fs::exists(path)) {
            return fail("refusing to overwrite existing " + fs::relative_to(path, dir));
        }
    }

    const std::string config =
        "project(\"" + name + "\")\n"
        "\n"
        "builddir(\"build\")\n"
        "\n"
        "target(\"" + name + "\", {\n"
        "    kind = \"executable\",\n"
        "    sources = glob(\"src/**/*.cpp\"),\n"
        "    include_dirs = { \"include\" },\n"
        "})\n"
        "\n"
        "-- Luma provides \"debug\" (-O0 -g) and \"release\" (-O3) profiles with sensible\n"
        "-- flags for the detected toolchain. Declaring one of them adds to those defaults\n"
        "-- (your flags come last, so they win); add inherit = false to start from scratch:\n"
        "--\n"
        "-- profile(\"debug\", { defines = { \"MYAPP_DEBUG\" } })\n"
        "-- profile(\"release\", { cxxflags = { \"-march=native\" } })\n"
        "-- profile(\"asan\", { cxxflags = { \"-O1\", \"-g\", \"-fsanitize=address\" }, linkflags = { \"-fsanitize=address\" } })\n";

    const std::string main_cpp =
        "#include <iostream>\n"
        "\n"
        "int main() {\n"
        "    std::cout << \"Hello from " + name + "!\\n\";\n"
        "    return 0;\n"
        "}\n";

    fs::create_directories(fs::join(dir, "src"));
    fs::create_directories(fs::join(dir, "include"));
    if (!write_new_file(config_path, config) || !write_new_file(main_path, main_cpp)) {
        return fail("failed to write project files in " + dir);
    }
    std::cout << "created luma.lua\ncreated src/main.cpp\ncreated include/\n";

    const Path gitignore = fs::join(dir, ".gitignore");
    if (!fs::exists(gitignore) &&
        write_new_file(gitignore, "build/\n.luma/\ncompile_commands.json\n")) {
        std::cout << "created .gitignore\n";
    }
    std::cout << "\nNext: luma build && luma run " << name << std::endl;
    return 0;
}

int cmd_graph(const ParsedArgs& args) {
    const std::string format = args.format.empty() ? "dot" : args.format;
    if (format != "dot") {
        return fail("unsupported graph format: " + format);
    }
    Session session;
    if (const auto err = load_session(session, args)) {
        return fail(err->message);
    }
    emit_dot(session.plan.graph);
    return 0;
}

int cmd_inspect(const ParsedArgs& args) {
    if (args.positionals.size() != 1) {
        return fail("usage: luma inspect <target>");
    }
    Session session;
    if (const auto err = load_session(session, args)) {
        return fail(err->message);
    }
    if (!config::find_target(session.config, args.positionals.front())) {
        return fail("unknown target: " + args.positionals.front());
    }
    inspect_target(session.config, args.positionals.front());
    return 0;
}

int cmd_explain(const ParsedArgs& args) {
    if (args.positionals.size() != 1) {
        return fail("usage: luma explain <target>");
    }
    const std::string& target = args.positionals.front();
    Session session;
    if (const auto err = load_session(session, args, {target})) {
        return fail(err->message);
    }
    cache::CacheStore cache(session.cache_dir);
    cache.load();

    const auto explanations =
        executor::explain_target(session.plan, cache, target, session.config.root);

    bool would_rebuild = false;
    for (const auto& item : explanations) {
        would_rebuild = would_rebuild || item.would_run;
    }
    if (!would_rebuild) {
        std::cout << target << " is up to date\n";
        return 0;
    }

    std::cout << target << " would rebuild because:\n";
    for (const auto& item : explanations) {
        if (!item.would_run) {
            continue;
        }
        for (const auto& reason : item.reasons) {
            std::cout << "  - " << reason << " (" << item.id.value << ")\n";
        }
    }
    return 0;
}

int cmd_clean(const ParsedArgs& args) {
    Session session;
    if (const auto err = load_session(session, args)) {
        return fail(err->message);
    }
    const Path build_dir = session.build_dir();
    const Path cache_dir = fs::join(session.config.root, ".luma");
    fs::remove_all(build_dir);
    fs::remove_all(cache_dir);
    std::cout << "cleaned " << build_dir << " and " << cache_dir << '\n';
    return 0;
}

} // namespace

CommandResult run(int argc, char** argv) {
    if (argc < 2) {
        print_usage(std::cerr);
        return {1};
    }

    const ParsedArgs args = parse_args(argc, argv);
    if (args.command == "--help" || args.command == "-h" || args.command == "help") {
        print_usage(std::cout);
        return {0};
    }
    if (args.command == "--version" || args.command == "-v" || args.command == "version") {
        std::cout << "luma " << LUMA_VERSION << '\n';
        return {0};
    }
    if (!args.error.empty()) {
        return {fail(args.error)};
    }

    if (args.command == "build") {
        return {cmd_build(args)};
    }
    if (args.command == "run") {
        return {cmd_run(args)};
    }
    if (args.command == "test") {
        return {cmd_test(args)};
    }
    if (args.command == "targets") {
        return {cmd_targets(args)};
    }
    if (args.command == "compdb") {
        return {cmd_compdb(args)};
    }
    if (args.command == "init") {
        return {cmd_init(args)};
    }
    if (args.command == "graph") {
        return {cmd_graph(args)};
    }
    if (args.command == "inspect") {
        return {cmd_inspect(args)};
    }
    if (args.command == "explain") {
        return {cmd_explain(args)};
    }
    if (args.command == "clean") {
        return {cmd_clean(args)};
    }

    std::cerr << "luma: error: unknown command: " << args.command << "\n\n";
    print_usage(std::cerr);
    return {1};
}

} // namespace luma::cli
