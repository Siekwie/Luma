#pragma once

#include "luma/config/model.hpp"
#include "luma/types.hpp"

#include <optional>
#include <string>

namespace luma::lua_loader {

struct LoadError {
    std::string message;
};

std::optional<LoadError> load_config(const Path& script_path, config::ProjectConfig& out);

} // namespace luma::lua_loader
