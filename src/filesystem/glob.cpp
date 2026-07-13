#include "luma/filesystem/glob.hpp"

#include "luma/filesystem/path_util.hpp"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace luma::fs {

namespace {

std::string glob_to_regex(const std::string& pattern) {
    std::ostringstream out;
    out << '^';
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                out << ".*";
                ++i;
                if (i + 1 < pattern.size() && pattern[i + 1] == '/') {
                    ++i;
                }
            } else {
                out << "[^/]*";
            }
        } else if (c == '?') {
            out << "[^/]";
        } else if (c == '.') {
            out << "\\.";
        } else if (c == '\\' || c == '+' || c == '(' || c == ')' || c == '|' || c == '^' || c == '$' ||
                   c == '[' || c == ']' || c == '{') {
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
    const auto recursive = normalized_pattern.find("/**/");

    std::string search_root = root;
    std::string relative_pattern = normalized_pattern;

    if (recursive != std::string::npos) {
        const std::string prefix = normalized_pattern.substr(0, recursive);
        search_root = prefix.empty() ? root : join(root, prefix);
        relative_pattern = normalized_pattern.substr(recursive + 4);
    }

    const std::regex matcher(glob_to_regex(relative_pattern), std::regex::icase);
    std::vector<Path> matches;

    if (!exists(search_root)) {
        return matches;
    }

    const auto files = list_files_recursive(search_root);
    for (const auto& file : files) {
        Path rel = relative_to(file, root);
        if (recursive != std::string::npos) {
            const std::string prefix = normalized_pattern.substr(0, recursive);
            if (!prefix.empty()) {
                rel = relative_to(file, search_root);
            }
        }
        if (std::regex_match(rel, matcher)) {
            matches.push_back(relative_to(file, root));
        }
    }

    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    return matches;
}

} // namespace luma::fs
