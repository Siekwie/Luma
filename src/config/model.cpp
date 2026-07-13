#include "luma/config/model.hpp"

namespace luma::config {

const TargetDef* find_target(const ProjectConfig& config, const std::string& name) {
    const auto it = config.targets.find(name);
    return it == config.targets.end() ? nullptr : &it->second;
}

const ProfileDef* find_profile(const ProjectConfig& config, const std::string& name) {
    const auto it = config.profiles.find(name);
    return it == config.profiles.end() ? nullptr : &it->second;
}

const ToolchainDef* find_toolchain(const ProjectConfig& config) {
    if (!config.active_toolchain.empty()) {
        const auto it = config.toolchains.find(config.active_toolchain);
        if (it != config.toolchains.end()) {
            return &it->second;
        }
    }
    if (!config.toolchains.empty()) {
        return &config.toolchains.begin()->second;
    }
    return nullptr;
}

} // namespace luma::config
