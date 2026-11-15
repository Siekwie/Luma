#include "luma/executor/depfile.hpp"

#include "luma/filesystem/path_util.hpp"

#include <algorithm>
#include <cctype>

namespace luma::executor {

namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string to_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/// Mirrors ninja's heuristic: headers of the Windows SDK and Visual Studio
/// installations never change between builds and are not tracked.
bool is_msvc_system_include(std::string_view path) {
    const std::string lower = to_lower(path);
    return lower.find("program files") != std::string::npos ||
           lower.find("microsoft visual studio") != std::string::npos ||
           lower.find("windows kits") != std::string::npos;
}

} // namespace

std::vector<Path> parse_gcc_depfile(std::string_view content) {
    std::vector<Path> deps;
    std::string token;
    bool seen_colon = false;
    bool done = false;

    auto flush = [&]() {
        if (token.empty()) {
            return;
        }
        if (!seen_colon) {
            // Still in the target part. A token ending in ':' finishes it.
            if (token.back() == ':') {
                seen_colon = true;
            }
        } else {
            deps.push_back(fs::normalize(token));
        }
        token.clear();
    };

    for (std::size_t i = 0; i < content.size() && !done; ++i) {
        const char c = content[i];
        if (c == '\\' && i + 1 < content.size()) {
            const char next = content[i + 1];
            if (next == '\n' || next == '\r') {
                // Line continuation.
                flush();
                ++i;
                if (next == '\r' && i + 1 < content.size() && content[i + 1] == '\n') {
                    ++i;
                }
                continue;
            }
            if (next == ' ' || next == '#' || next == '\\') {
                token.push_back(next);
                ++i;
                continue;
            }
            token.push_back(c);
            continue;
        }
        if (c == '$' && i + 1 < content.size() && content[i + 1] == '$') {
            token.push_back('$');
            ++i;
            continue;
        }
        if (c == ':' && !seen_colon) {
            // "target: deps" separator; "C:/path" style drive colons are
            // followed by a path separator and stay part of the token.
            const bool at_end = i + 1 >= content.size();
            const char next = at_end ? '\0' : content[i + 1];
            if (at_end || is_space(next)) {
                token.push_back(':');
                flush();
                if (token.empty() && !seen_colon) {
                    seen_colon = true;
                }
                continue;
            }
        }
        if (c == '\n' && seen_colon) {
            // End of the first rule (any further lines are phony targets).
            flush();
            done = true;
            continue;
        }
        if (is_space(c)) {
            flush();
            continue;
        }
        token.push_back(c);
    }
    if (!done) {
        flush();
    }
    return deps;
}

MsvcIncludeScan scan_msvc_output(
    std::string_view output,
    std::string_view source_file_name,
    std::string_view prefix) {
    MsvcIncludeScan scan;
    bool first_line = true;
    std::size_t start = 0;
    while (start < output.size()) {
        std::size_t end = output.find('\n', start);
        const bool has_newline = end != std::string_view::npos;
        if (!has_newline) {
            end = output.size();
        }
        std::string_view line = output.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        start = has_newline ? end + 1 : end;

        if (!prefix.empty() && line.starts_with(prefix)) {
            std::string_view path = line.substr(prefix.size());
            while (!path.empty() && is_space(path.front())) {
                path.remove_prefix(1);
            }
            if (!path.empty() && !is_msvc_system_include(path)) {
                const Path normalized = fs::normalize(std::string(path));
                if (std::find(scan.includes.begin(), scan.includes.end(), normalized) ==
                    scan.includes.end()) {
                    scan.includes.push_back(normalized);
                }
            }
            continue;
        }
        if (first_line && !source_file_name.empty() && line == source_file_name) {
            // cl.exe echoes the name of the file it compiles.
            first_line = false;
            continue;
        }
        first_line = false;
        scan.output.append(line);
        scan.output.push_back('\n');
    }
    return scan;
}

} // namespace luma::executor
