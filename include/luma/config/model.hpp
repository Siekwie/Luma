#pragma once

#include "luma/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace luma::config {

enum class TargetKind { Executable, StaticLibrary, SharedLibrary };

enum class ToolchainStyle { Gnu, Msvc };

struct ToolchainDef {
    std::string name;
    ToolchainStyle style = ToolchainStyle::Gnu;
    std::string cc = "cc";
    std::string cxx = "c++";
    std::string linker;
    std::string archiver = "ar";
    std::vector<std::string> languages;
};

struct ProfileDef {
    std::string name;
    std::vector<std::string> defines;
    std::vector<std::string> cflags;
    std::vector<std::string> cxxflags;
    std::vector<std::string> linkflags;
    bool lto = false;
    // For the built-in profile names ("debug", "release"): start from Luma's toolchain
    // defaults (-O0 -g / -O3, warnings, C++20) and append this profile's settings.
    // Set `inherit = false` in luma.lua to start from nothing.
    bool inherit = true;
};

struct GenerateDef {
    std::string name;
    std::vector<std::string> input_patterns;
    std::vector<std::string> inputs;
    std::vector<Path> outputs;
    std::vector<std::string> command;
};

struct TargetDef {
    std::string name;
    TargetKind kind = TargetKind::Executable;
    std::string language = "cxx";
    std::vector<std::string> source_patterns;
    std::vector<Path> sources;
    std::vector<Path> include_dirs;
    std::vector<std::string> defines;
    std::vector<std::string> cflags;
    std::vector<std::string> cxxflags;
    std::vector<std::string> linkflags;
    std::vector<std::string> dependencies;
    std::vector<std::string> system_libs;
};

struct ProjectConfig {
    std::string name;
    Path root;
    Path build_dir = "build";
    std::string profile = "debug";
    std::string active_toolchain;
    std::unordered_map<std::string, ToolchainDef> toolchains;
    std::unordered_map<std::string, ProfileDef> profiles;
    std::unordered_map<std::string, TargetDef> targets;
    std::vector<GenerateDef> generators;
};

const TargetDef* find_target(const ProjectConfig& config, const std::string& name);
const ProfileDef* find_profile(const ProjectConfig& config, const std::string& name);
const ToolchainDef* find_toolchain(const ProjectConfig& config);

} // namespace luma::config
