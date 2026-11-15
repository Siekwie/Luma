#include "luma/cli/commands.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

std::string to_utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

} // namespace

// On Windows main()'s argv is in the ANSI code page; luma works in UTF-8
// throughout, so re-read the command line as UTF-16 and convert it.
int main(int argc, char** argv) {
    int wide_argc = 0;
    wchar_t** wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (!wide_argv) {
        return luma::cli::run(argc, argv).exit_code;
    }
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(wide_argc));
    for (int i = 0; i < wide_argc; ++i) {
        args.push_back(to_utf8(wide_argv[i]));
    }
    LocalFree(wide_argv);

    std::vector<char*> utf8_argv;
    utf8_argv.reserve(args.size() + 1);
    for (auto& arg : args) {
        utf8_argv.push_back(arg.data());
    }
    utf8_argv.push_back(nullptr);
    return luma::cli::run(static_cast<int>(args.size()), utf8_argv.data()).exit_code;
}
#else
int main(int argc, char** argv) {
    return luma::cli::run(argc, argv).exit_code;
}
#endif
