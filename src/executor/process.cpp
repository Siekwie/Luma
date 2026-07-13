#include "luma/executor/process.hpp"

#include "luma/filesystem/path_util.hpp"

#include <array>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#endif

namespace luma::executor {

namespace fs = luma::fs;

namespace {

#ifdef _WIN32
std::string quote_arg(const std::string& arg) {
    if (arg.empty()) {
        return "\"\"";
    }
    if (arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

std::string build_command_line(const std::vector<std::string>& command) {
    std::ostringstream out;
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (i != 0) {
            out << ' ';
        }
        out << quote_arg(command[i]);
    }
    return out.str();
}
#endif

} // namespace

std::string format_command(const std::vector<std::string>& command) {
    std::ostringstream out;
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (i != 0) {
            out << ' ';
        }
        out << command[i];
    }
    return out.str();
}

ProcessResult run_process(
    const std::vector<std::string>& command,
    const std::string& working_directory) {
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

    CreatePipe(&stdout_read, &stdout_write, &sa, 0);
    CreatePipe(&stderr_read, &stderr_write, &sa, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    const std::string command_line = build_command_line(command);
    std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back('\0');

    const char* cwd = working_directory.empty() ? nullptr : fs::to_native(working_directory).c_str();
    const BOOL created = CreateProcessA(
        nullptr,
        mutable_command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        cwd,
        &si,
        &pi);

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (!created) {
        result.stderr_text = "CreateProcess failed";
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        return result;
    }

    auto drain = [](HANDLE pipe) {
        std::string text;
        std::array<char, 4096> buffer{};
        DWORD read = 0;
        while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) &&
               read > 0) {
            text.append(buffer.data(), read);
        }
        return text;
    };

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    result.stdout_text = drain(stdout_read);
    result.stderr_text = drain(stderr_read);

    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        result.stderr_text = "pipe failed";
        return result;
    }

    const pid_t pid = fork();
    if (pid == 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        if (!working_directory.empty()) {
            if (chdir(working_directory.c_str()) != 0) {
                _exit(127);
            }
        }

        std::vector<char*> argv;
        argv.reserve(command.size() + 1);
        for (const auto& arg : command) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    auto drain = [](int fd) {
        std::string text;
        std::array<char, 4096> buffer{};
        ssize_t read = 0;
        while ((read = ::read(fd, buffer.data(), buffer.size())) > 0) {
            text.append(buffer.data(), static_cast<std::size_t>(read));
        }
        return text;
    };

    result.stdout_text = drain(stdout_pipe[0]);
    result.stderr_text = drain(stderr_pipe[0]);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else {
        result.exit_code = 1;
    }
#endif

    return result;
}

} // namespace luma::executor
