#include "luma/filesystem/path_util.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace luma::fs {

namespace fs = std::filesystem;

fs::path to_fs_path(const Path& path) {
    return fs::path(std::u8string(path.begin(), path.end()));
}

Path from_fs_path(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return Path(text.begin(), text.end());
}

Path normalize(Path path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() >= 2 && path[0] == '.' && path[1] == '/') {
        path.erase(0, 2);
    }
    return path;
}

bool is_absolute(const Path& path) {
    if (path.empty()) {
        return false;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }
    // Windows drive letter ("C:/..."), accepted on every platform so that
    // paths written on Windows behave the same everywhere.
    return path.size() >= 2 && path[1] == ':' &&
           std::isalpha(static_cast<unsigned char>(path[0])) != 0;
}

Path lexically_normal(const Path& path) {
    if (path.empty()) {
        return path;
    }
    std::string out = from_fs_path(to_fs_path(normalize(path)).lexically_normal());
    while (out.size() > 1 && out.back() == '/' && !(out.size() == 3 && out[1] == ':')) {
        out.pop_back();
    }
    if (out.empty()) {
        return ".";
    }
    return out;
}

Path join(const Path& base, const Path& part) {
    if (base.empty() || is_absolute(part)) {
        return lexically_normal(part);
    }
    if (part.empty()) {
        return lexically_normal(base);
    }
    return lexically_normal(base + "/" + part);
}

Path absolute(const Path& path) {
    std::error_code ec;
    const fs::path abs = fs::absolute(to_fs_path(path), ec);
    if (ec) {
        return lexically_normal(path);
    }
    return lexically_normal(from_fs_path(abs));
}

Path relative_to(const Path& path, const Path& root) {
    const fs::path p = to_fs_path(path);
    const fs::path r = to_fs_path(root);
    std::error_code ec;
    const auto rel = fs::relative(p, r, ec);
    if (ec) {
        return normalize(path);
    }
    return normalize(from_fs_path(rel));
}

std::string stem(const Path& path) {
    return from_fs_path(to_fs_path(path).stem());
}

std::string extension(const Path& path) {
    return from_fs_path(to_fs_path(path).extension());
}

bool exists(const Path& path) {
    std::error_code ec;
    return fs::exists(to_fs_path(path), ec);
}

bool is_directory(const Path& path) {
    std::error_code ec;
    return fs::is_directory(to_fs_path(path), ec);
}

bool is_regular_file(const Path& path) {
    std::error_code ec;
    return fs::is_regular_file(to_fs_path(path), ec);
}

bool create_directories(const Path& path) {
    std::error_code ec;
    return fs::create_directories(to_fs_path(path), ec);
}

bool remove_all(const Path& path) {
    std::error_code ec;
    return static_cast<bool>(fs::remove_all(to_fs_path(path), ec));
}

std::optional<Path> find_upwards(const Path& start, const Path& filename) {
    std::error_code ec;
    fs::path current = fs::absolute(to_fs_path(start), ec).lexically_normal();
    if (!fs::is_directory(current, ec)) {
        current = current.parent_path();
    }

    while (!current.empty()) {
        const fs::path candidate = current / to_fs_path(filename);
        if (fs::is_regular_file(candidate, ec)) {
            return from_fs_path(candidate);
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

    // Error-code overloads throughout: the range-for form's operator++ and
    // entry.is_regular_file() throw on access errors (common on Windows).
    std::error_code ec;
    fs::recursive_directory_iterator it(
        to_fs_path(root), fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code type_ec;
        if (it->is_regular_file(type_ec)) {
            files.push_back(normalize(from_fs_path(it->path())));
        }
        it.increment(ec);
    }
    return files;
}

std::string to_native(const Path& path) {
    const std::u8string text = to_fs_path(path).make_preferred().u8string();
    return std::string(text.begin(), text.end());
}

std::string filename(const Path& path) {
    return from_fs_path(to_fs_path(path).filename());
}

std::string parent(const Path& path) {
    return normalize(from_fs_path(to_fs_path(path).parent_path()));
}

} // namespace luma::fs
