#include "luma/executor/process.hpp"

#include "luma/filesystem/path_util.hpp"

#include <array>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace luma::executor {

namespace fs = luma::fs;

namespace {

/// Serializes pipe creation and process spawning. Without it, a child started
/// on one thread can inherit the pipe handles another thread is setting up,
/// which keeps those pipes open (no EOF) until the unrelated child exits.
std::mutex& spawn_mutex() {
    static std::mutex mutex;
    return mutex;
}

#ifdef _WIN32
/// Quote one argument following the MSVC runtime's CommandLineToArgvW rules.
std::string quote_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == '\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(*it);
        }
    }
    out.push_back('"');
    return out;
}

std::string build_command_line(const std::vector<std::string>& command) {
    std::string out;
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        out += quote_arg(command[i]);
    }
    return out;
}

std::string last_error_message() {
    const DWORD code = GetLastError();
    char* buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);
    std::string message = size && buffer ? std::string(buffer, size) : "error " + std::to_string(code);
    if (buffer) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}

std::string drain_handle(HANDLE pipe) {
    std::string text;
    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) &&
           read > 0) {
        text.append(buffer.data(), read);
    }
    return text;
}

BOOL WINAPI ignore_console_ctrl(DWORD) {
    // The child shares our console and receives Ctrl+C itself; we keep
    // running so that we can forward its exit code.
    return TRUE;
}
#else
std::string drain_fd(int fd) {
    std::string text;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t got = ::read(fd, buffer.data(), buffer.size());
        if (got > 0) {
            text.append(buffer.data(), static_cast<std::size_t>(got));
        } else if (got < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    return text;
}

bool make_cloexec_pipe(int fds[2]) {
    if (pipe(fds) != 0) {
        return false;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

[[noreturn]] void exec_child(const std::vector<std::string>& command, const std::string& cwd) {
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
        const char msg[] = "luma: cannot change directory for child process\n";
        (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(127);
    }

    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (const auto& arg : command) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());

    const char prefix[] = "luma: failed to execute '";
    const char suffix[] = "'\n";
    (void)!write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    (void)!write(STDERR_FILENO, command[0].c_str(), command[0].size());
    (void)!write(STDERR_FILENO, suffix, sizeof(suffix) - 1);
    _exit(127);
}

int wait_for(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return 1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}
#endif

} // namespace

std::string format_command(const std::vector<std::string>& command) {
    std::string out;
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        const std::string& arg = command[i];
        if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) {
            out += arg;
            continue;
        }
        out.push_back('"');
        for (const char c : arg) {
            if (c == '"' || c == '\\') {
                out.push_back('\\');
            }
            out.push_back(c);
        }
        out.push_back('"');
    }
    return out;
}

ProcessResult run_process(
    const std::vector<std::string>& command,
    const std::string& working_directory,
    bool merge_output) {
    ProcessResult result;
    if (command.empty()) {
        result.stderr_text = "empty command";
        return result;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    HANDLE null_input = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi{};
    BOOL created = FALSE;
    std::string spawn_error;

    {
        std::lock_guard lock(spawn_mutex());

        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
            result.stderr_text = "CreatePipe failed: " + last_error_message();
            return result;
        }
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
        if (!merge_output) {
            if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
                result.stderr_text = "CreatePipe failed: " + last_error_message();
                CloseHandle(stdout_read);
                CloseHandle(stdout_write);
                return result;
            }
            SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
        }
        null_input = CreateFileA(
            "NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = stdout_write;
        si.hStdError = merge_output ? stdout_write : stderr_write;
        si.hStdInput = null_input;

        const std::string command_line = build_command_line(command);
        std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
        mutable_command_line.push_back('\0');

        const std::string cwd = working_directory.empty() ? std::string{} : fs::to_native(working_directory);
        created = CreateProcessA(
            nullptr,
            mutable_command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            cwd.empty() ? nullptr : cwd.c_str(),
            &si,
            &pi);
        if (!created) {
            spawn_error = last_error_message();
        }

        // Close our copies of the write ends while still holding the lock so
        // that no other child can inherit them.
        CloseHandle(stdout_write);
        if (stderr_write) {
            CloseHandle(stderr_write);
        }
        if (null_input != INVALID_HANDLE_VALUE) {
            CloseHandle(null_input);
        }
    }

    if (!created) {
        result.stderr_text = "failed to execute '" + command[0] + "': " + spawn_error;
        CloseHandle(stdout_read);
        if (stderr_read) {
            CloseHandle(stderr_read);
        }
        return result;
    }

    // Drain the pipes before waiting: a child that fills a pipe buffer would
    // otherwise block forever.
    std::thread stderr_thread;
    if (stderr_read) {
        stderr_thread = std::thread([&]() { result.stderr_text = drain_handle(stderr_read); });
    }
    result.stdout_text = drain_handle(stdout_read);
    if (stderr_thread.joinable()) {
        stderr_thread.join();
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);

    CloseHandle(stdout_read);
    if (stderr_read) {
        CloseHandle(stderr_read);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    pid_t pid = -1;

    {
        std::lock_guard lock(spawn_mutex());
        if (!make_cloexec_pipe(stdout_pipe)) {
            result.stderr_text = "pipe failed";
            return result;
        }
        if (!merge_output && !make_cloexec_pipe(stderr_pipe)) {
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);
            result.stderr_text = "pipe failed";
            return result;
        }

        pid = fork();
        if (pid == 0) {
            const int null_fd = open("/dev/null", O_RDONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDIN_FILENO);
            }
            dup2(stdout_pipe[1], STDOUT_FILENO);
            dup2(merge_output ? stdout_pipe[1] : stderr_pipe[1], STDERR_FILENO);
            exec_child(command, working_directory);
        }

        close(stdout_pipe[1]);
        if (!merge_output) {
            close(stderr_pipe[1]);
        }
    }

    if (pid < 0) {
        close(stdout_pipe[0]);
        if (!merge_output) {
            close(stderr_pipe[0]);
        }
        result.stderr_text = "fork failed";
        return result;
    }

    std::thread stderr_thread;
    if (!merge_output) {
        stderr_thread = std::thread([&]() { result.stderr_text = drain_fd(stderr_pipe[0]); });
    }
    result.stdout_text = drain_fd(stdout_pipe[0]);
    if (stderr_thread.joinable()) {
        stderr_thread.join();
    }
    close(stdout_pipe[0]);
    if (!merge_output) {
        close(stderr_pipe[0]);
    }

    result.exit_code = wait_for(pid);
#endif

    return result;
}

int run_process_interactive(
    const std::vector<std::string>& command,
    const std::string& working_directory) {
    if (command.empty()) {
        return 127;
    }

#ifdef _WIN32
    PROCESS_INFORMATION pi{};
    BOOL created = FALSE;
    std::string spawn_error;
    {
        std::lock_guard lock(spawn_mutex());

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        for (HANDLE handle : {si.hStdInput, si.hStdOutput, si.hStdError}) {
            if (handle && handle != INVALID_HANDLE_VALUE) {
                SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            }
        }

        const std::string command_line = build_command_line(command);
        std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
        mutable_command_line.push_back('\0');
        const std::string cwd = working_directory.empty() ? std::string{} : fs::to_native(working_directory);

        created = CreateProcessA(
            nullptr,
            mutable_command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            cwd.empty() ? nullptr : cwd.c_str(),
            &si,
            &pi);
        if (!created) {
            spawn_error = last_error_message();
        }
    }

    if (!created) {
        const std::string message = "luma: failed to execute '" + command[0] + "': " + spawn_error + "\n";
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
        return 127;
    }

    SetConsoleCtrlHandler(ignore_console_ctrl, TRUE);
    WaitForSingleObject(pi.hProcess, INFINITE);
    SetConsoleCtrlHandler(ignore_console_ctrl, FALSE);

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exit_code);
#else
    // Ignore interactive signals while the child runs: the terminal delivers
    // them to the child too, and we want to report its exit status.
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    struct sigaction old_int {};
    struct sigaction old_quit {};

    pid_t pid = -1;
    {
        std::lock_guard lock(spawn_mutex());
        sigaction(SIGINT, &ignore, &old_int);
        sigaction(SIGQUIT, &ignore, &old_quit);
        pid = fork();
        if (pid == 0) {
            sigaction(SIGINT, &old_int, nullptr);
            sigaction(SIGQUIT, &old_quit, nullptr);
            exec_child(command, working_directory);
        }
    }

    int exit_code = 127;
    if (pid > 0) {
        exit_code = wait_for(pid);
    }
    sigaction(SIGINT, &old_int, nullptr);
    sigaction(SIGQUIT, &old_quit, nullptr);
    return exit_code;
#endif
}

} // namespace luma::executor
