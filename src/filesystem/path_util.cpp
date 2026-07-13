#include "luma/filesystem/path_util.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace luma::fs {

namespace fs = std::filesystem;

Path normalize(Path path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() >= 2 && path[0] == '.' && path[1] == '/') {
        path.erase(0, 2);
    }
    return path;
}

Path join(const Path& base, const Path& part) {
    if (base.empty()) {
        return normalize(part);
    }
    if (part.empty()) {
        return normalize(base);
    }
    if (base.ends_with('/')) {
        return normalize(base + part);
    }
    return normalize(base + "/" + part);
}

Path relative_to(const Path& path, const Path& root) {
    const fs::path p(path);
    const fs::path r(root);
    std::error_code ec;
    const auto rel = fs::relative(p, r, ec);
    if (ec) {
        return normalize(path);
    }
    return normalize(rel.generic_string());
}

std::string stem(const Path& path) {
    return fs::path(path).stem().string();
}

std::string extension(const Path& path) {
    return fs::path(path).extension().string();
}

bool exists(const Path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

bool is_directory(const Path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

bool is_regular_file(const Path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

bool create_directories(const Path& path) {
    std::error_code ec;
    return fs::create_directories(path, ec);
}

bool remove_all(const Path& path) {
    std::error_code ec;
    return static_cast<bool>(fs::remove_all(path, ec));
}

std::optional<Path> find_upwards(const Path& start, const Path& filename) {
    fs::path current = fs::absolute(start);
    std::error_code ec;
    if (!fs::is_directory(current, ec)) {
        current = current.parent_path();
    }

    while (!current.empty()) {
        const fs::path candidate = current / filename;
        if (fs::exists(candidate, ec)) {
            return candidate.generic_string();
        }
        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return std::nullopt;
}

std::vector<Path> list_files_recursive(const Path& root) {
    std::vector<Path> files;
    if (!is_directory(root)) {
        return files;
    }

    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file()) {
            files.push_back(normalize(entry.path().generic_string()));
        }
    }
    return files;
}

std::string to_native(const Path& path) {
    return fs::path(path).string();
}

} // namespace luma::fs
