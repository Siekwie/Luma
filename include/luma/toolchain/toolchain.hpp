#pragma once

#include "luma/config/model.hpp"

namespace luma::toolchain {

config::ToolchainDef detect_default();
std::string identity_string(const config::ToolchainDef& toolchain);

} // namespace luma::toolchain
