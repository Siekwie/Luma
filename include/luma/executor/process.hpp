#pragma once

#include <string>
#include <vector>

namespace luma::executor {

struct ProcessResult {
    /// Exit code of the child; -1 if it could not be started.
    int exit_code = -1;
    std::string stdout_text;
    /// Empty when the output was merged into stdout_text.
    std::string stderr_text;
};

/// Run a command (argv-style, PATH lookup) and capture its output.
/// With `merge_output`, stderr is redirected into the same pipe as stdout so
/// diagnostics keep their original interleaving. The child's stdin is the
/// null device. Safe to call from several threads at once.
ProcessResult run_process(
    const std::vector<std::string>& command,
    const std::string& working_directory = {},
    bool merge_output = false);

/// Run a command that inherits this process's stdin/stdout/stderr and wait
/// for it. Returns the child's exit code (128 + signal number when it was
/// killed by a signal on POSIX), or 127 if it could not be started.
int run_process_interactive(
    const std::vector<std::string>& command,
    const std::string& working_directory = {});

/// While alive, SIGINT/SIGTERM/SIGHUP (POSIX) or console Ctrl+C/Break/Close
/// (Windows) request cancellation instead of killing luma outright: running
/// children started by run_process() are terminated, no new ones start, and
/// cancellation_requested() becomes true so the caller can shut down cleanly
/// (and persist its cache). A second signal terminates immediately.
/// On Windows, children are additionally placed in a job object that kills
/// them if luma itself is terminated.
class CancellationScope {
public:
    CancellationScope();
    ~CancellationScope();
    CancellationScope(const CancellationScope&) = delete;
    CancellationScope& operator=(const CancellationScope&) = delete;
};

bool cancellation_requested();

/// Exit code to use after a cancellation (128 + signal number; 130 on Windows).
int cancellation_exit_code();

/// Human-readable command line (arguments with spaces are quoted).
std::string format_command(const std::vector<std::string>& command);

} // namespace luma::executor
