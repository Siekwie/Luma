#pragma once

#include "luma/types.hpp"

#include <vector>

namespace luma::fs {

/// Match glob patterns like "src/**/*.cpp" against the filesystem.
std::vector<Path> glob(const Path& root, const std::string& pattern);

} // namespace luma::fs
