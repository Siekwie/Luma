#include "luma/cli/commands.hpp"

#include "luma/cache/cache_store.hpp"
#include "luma/cli/session.hpp"
#include "luma/executor/executor.hpp"
#include "luma/filesystem/path_util.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace luma::cli {

namespace fs = luma::fs;

namespace {

void print_usage() {
    std::cerr
        << "Luma — a modern build graph engine\n\n"
        << "Usage:\n"
        << "  luma build [target...] [--profile name] [--dry-run] [--verbose] [-j N]\n"
        << "  luma clean\n"
        << "  luma graph [--format dot]\n"
        << "  luma explain <target>\n"
        << "  luma inspect <target>\n"
        << "  luma --version\n"
        << "  luma --help\n";
}

struct ParsedArgs {
    std::string command;
    std::string profile = "debug";
    std::string graph_format = "dot";
    std::string explain_target;
    std::string inspect_target;
    std::vector<std::string> build_targets;
    bool dry_run = false;
    bool verbose = false;
    std::size_t jobs = 0;
};

ParsedArgs parse_args(int argc, char** argv) {
    ParsedArgs args;
    if (argc < 2) {
        return args;
    }

    args.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--profile" && i + 1 < argc) {
            args.profile = argv[++i];
        } else if (arg == "--format" && i + 1 < argc) {
            args.graph_format = argv[++i];
        } else if (arg == "--dry-run") {
            args.dry_run = true;
        } else if (arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "-j" && i + 1 < argc) {
            args.jobs = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (args.command == "explain") {
            args.explain_target = argv[i];
        } else if (args.command == "inspect") {
            args.inspect_target = argv[i];
        } else if (args.command == "build") {
            args.build_targets.emplace_back(argv[i]);
        }
    }
    return args;
}

SessionOptions session_options_from(const ParsedArgs& args) {
    SessionOptions options;
    options.project_root = fs::normalize(".");
    options.profile = args.profile;
    options.targets = args.build_targets;
    return options;
}

CommandResult cmd_version() {
    std::cout << "luma 0.1.0\n";
    return {0};
}

} // namespace

CommandResult run(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return {0};
    }

    const ParsedArgs args = parse_args(argc, argv);
    if (args.command == "--help" || args.command == "-h") {
        print_usage();
        return {0};
    }
    if (args.command == "--version" || args.command == "-v") {
        return cmd_version();
    }

    if (args.command == "graph") {
        Session session;
        if (const auto err = Session::load(session, session_options_from(args))) {
            std::cerr << "error: " << err->message << '\n';
            return {1};
        }
        if (args.graph_format == "dot") {
            emit_dot(session.plan.graph);
        } else {
            std::cerr << "unsupported graph format: " << args.graph_format << '\n';
            return {1};
        }
        return {0};
    }

    if (args.command == "inspect") {
        if (args.inspect_target.empty()) {
            std::cerr << "usage: luma inspect <target>\n";
            return {1};
        }
        Session session;
        if (const auto err = Session::load(session, session_options_from(args))) {
            std::cerr << "error: " << err->message << '\n';
            return {1};
        }
        inspect_target(session.config, args.inspect_target);
        return {0};
    }

    if (args.command == "explain") {
        if (args.explain_target.empty()) {
            std::cerr << "usage: luma explain <target>\n";
            return {1};
        }
        Session session;
        if (const auto err = Session::load(session, session_options_from(args))) {
            std::cerr << "error: " << err->message << '\n';
            return {1};
        }
        cache::CacheStore cache(session.cache_dir);
        cache.load();

        const auto explanations =
            executor::explain_target(session.plan, cache, args.explain_target, session.config.root);

        bool would_rebuild = false;
        for (const auto& item : explanations) {
            if (item.would_run) {
                would_rebuild = true;
            }
        }

        if (!would_rebuild) {
            std::cout << args.explain_target << " is up to date\n";
            return {0};
        }

        std::cout << args.explain_target << " would rebuild because:\n";
        for (const auto& item : explanations) {
            if (!item.would_run) {
                continue;
            }
            for (const auto& reason : item.reasons) {
                std::cout << "  - " << reason << " (" << item.id.value << ")\n";
            }
        }
        return {0};
    }

    if (args.command == "clean") {
        Session session;
        if (const auto err = Session::load(session, session_options_from(args))) {
            std::cerr << "error: " << err->message << '\n';
            return {1};
        }
        const Path build_dir = fs::join(session.config.root, session.config.build_dir);
        const Path cache_dir = fs::join(session.config.root, ".luma");
        fs::remove_all(build_dir);
        fs::remove_all(cache_dir);
        std::cout << "cleaned " << build_dir << " and " << cache_dir << '\n';
        return {0};
    }

    if (args.command == "build") {
        Session session;
        if (const auto err = Session::load(session, session_options_from(args))) {
            std::cerr << "error: " << err->message << '\n';
            return {1};
        }

        cache::CacheStore cache(session.cache_dir);
        cache.load();

        executor::ExecutorOptions options{
            .dry_run = args.dry_run,
            .verbose = args.verbose,
            .jobs = args.jobs,
        };

        const auto result = executor::execute(session.config, session.plan, cache, options);
        if (result.exit_code != 0) {
            return {result.exit_code};
        }

        if (args.verbose || args.dry_run) {
            std::cout << "built " << result.tasks_run << " task(s), skipped " << result.tasks_skipped
                      << '\n';
        }
        return {0};
    }

    std::cerr << "unknown command: " << args.command << '\n';
    print_usage();
    return {1};
}

} // namespace luma::cli
