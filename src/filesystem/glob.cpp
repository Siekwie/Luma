#include "luma/filesystem/glob.hpp"

#include "luma/filesystem/path_util.hpp"

#include <algorithm>
#include <regex>
#include <sstream>

namespace luma::fs {

namespace {

bool has_wildcard(const std::string& segment) {
    return segment.find_first_of("*?[") != std::string::npos;
}

/// Translate a glob into an ECMAScript regex matched against '/'-separated
/// paths relative to the project root:
///   `**/` matches zero or more directories, `**` anything,
///   `*` anything except '/', `?` one character except '/'.
std::string glob_to_regex(const std::string& pattern) {
    std::ostringstream out;
    out << '^';
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                ++i;
                if (i + 1 < pattern.size() && pattern[i + 1] == '/') {
                    ++i;
                    out << "(?:.*/)?";
                } else {
                    out << ".*";
                }
            } else {
                out << "[^/]*";
            }
        } else if (c == '?') {
            out << "[^/]";
        } else if (c == '.' || c == '\\' || c == '+' || c == '(' || c == ')' || c == '|' ||
                   c == '^' || c == '$' || c == '[' || c == ']' || c == '{' || c == '}') {
            out << '\\' << c;
        } else {
            out << c;
        }
    }
    out << '$';
    return out.str();
}

} // namespace

std::vector<Path> glob(const Path& root, const std::string& pattern) {
    const std::string normalized_pattern = normalize(pattern);

    // Walk only the longest literal directory prefix of the pattern.
    std::string literal_prefix;
    {
        std::size_t start = 0;
        while (true) {
            const std::size_t slash = normalized_pattern.find('/', start);
            if (slash == std::string::npos) {
                break;
            }
            const std::string segment = normalized_pattern.substr(start, slash - start);
            if (has_wildcard(segment)) {
                break;
            }
            literal_prefix = normalized_pattern.substr(0, slash);
            start = slash + 1;
        }
    }

    std::vector<Path> matches;
    if (!has_wildcard(normalized_pattern)) {
        if (is_regular_file(join(root, normalized_pattern))) {
            matches.push_back(normalized_pattern);
        }
        return matches;
    }

    const Path search_root = literal_prefix.empty() ? root : join(root, literal_prefix);
    if (!is_directory(search_root)) {
        return matches;
    }

#ifdef _WIN32
    const auto flags = std::regex::ECMAScript | std::regex::icase;
#else
    const auto flags = std::regex::ECMAScript;
#endif
    const std::regex matcher(glob_to_regex(normalized_pattern), flags);

    for (const auto& file : list_files_recursive(search_root)) {
        const Path rel = relative_to(file, root);
        if (std::regex_match(rel, matcher)) {
            matches.push_back(rel);
        }
    }

    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    return matches;
}

} // namespace luma::fs
