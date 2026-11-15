#pragma once

#include "luma/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace luma::fs {

/// Convert backslashes to forward slashes and strip leading "./".
Path normalize(Path path);
/// True for "/x", "\\x" and "C:/x" style paths.
bool is_absolute(const Path& path);
/// normalize() plus removal of "." / ".." segments and trailing slashes.
Path lexically_normal(const Path& path);
/// Join two paths; an absolute `part` replaces `base`. Result is lexically normal.
Path join(const Path& base, const Path& part);
/// Make a path absolute against the current working directory.
Path absolute(const Path& path);
Path relative_to(const Path& path, const Path& root);
std::string stem(const Path& path);
std::string extension(const Path& path);
bool exists(const Path& path);
bool is_directory(const Path& path);
bool is_regular_file(const Path& path);
bool create_directories(const Path& path);
bool remove_all(const Path& path);
std::optional<Path> find_upwards(const Path& start, const Path& filename);
std::vector<Path> list_files_recursive(const Path& root);
std::string to_native(const Path& path);
std::string filename(const Path& path);
std::string parent(const Path& path);

} // namespace luma::fs
