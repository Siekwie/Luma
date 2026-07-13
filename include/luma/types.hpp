#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace luma {

using Path = std::string;
using Hash = std::string;

struct Environment {
    std::unordered_map<std::string, std::string> vars;
};

/// Opaque metadata attached to a task for introspection (target name, rule, etc.).
using Metadata = std::unordered_map<std::string, std::string>;

} // namespace luma
