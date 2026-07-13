#pragma once

#include "luma/types.hpp"

#include <optional>

namespace luma::fs {

std::optional<Hash> hash_file(const Path& path);

} // namespace luma::fs
