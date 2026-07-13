#pragma once

#include "luma/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace luma::fs {

Path normalize(Path path);
Path join(const Path& base, const Path& part);
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

} // namespace luma::fs
