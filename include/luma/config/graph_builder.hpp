#pragma once

#include "luma/config/model.hpp"
#include "luma/graph/build_graph.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace luma::config {

/// Resolved view of a target after globbing and dependency resolution.
/// Paths are relative to the project root unless they were absolute in the
/// configuration.
struct TargetInfo {
    std::string name;
    TargetKind kind = TargetKind::Executable;
    /// "cxx" or "c".
    std::string language = "cxx";
    Path output;
    std::vector<Path> sources;
    /// Effective include directories (own first, then dependencies').
    std::vector<Path> include_dirs;
    /// Effective defines (profile first, then target).
    std::vector<std::string> defines;
    /// Direct dependencies as declared.
    std::vector<std::string> dependencies;
};

struct BuildPlan {
    graph::BuildGraph graph;
    std::unordered_map<std::string, std::vector<graph::TaskId>> target_tasks;
    /// Targets in the plan, sorted by name.
    std::vector<TargetInfo> targets;
    std::string toolchain_identity;
    std::string configuration_fingerprint;
};

struct BuildPlanError {
    std::string message;
};

/// Task metadata keys set by build_plan() and read by the executor and the
/// introspection commands.
namespace meta {
inline constexpr const char* kTarget = "target";
inline constexpr const char* kRule = "rule";            // compile | link | archive | generate
inline constexpr const char* kVerb = "verb";            // word shown in progress lines
inline constexpr const char* kDescription = "description";
inline constexpr const char* kLanguage = "language";    // c | cxx (compile tasks)
inline constexpr const char* kSource = "source";        // compile tasks
inline constexpr const char* kDepsFormat = "deps";      // gcc | msvc (compile tasks)
inline constexpr const char* kDepfile = "depfile";      // gcc deps only
} // namespace meta

enum class SourceLanguage { C, Cxx, Other };

/// Language of a source file, decided by its extension (".c" is C;
/// ".cpp", ".cc", ".cxx", ".c++" and ".C" are C++).
SourceLanguage source_language(const Path& path);

/// Canonical spelling of a target language ("c" or "cxx").
std::string canonical_language(const std::string& language);

std::string kind_name(TargetKind kind);

std::optional<BuildPlanError> build_plan(
    const ProjectConfig& config,
    BuildPlan& out,
    const std::vector<std::string>& only_targets = {});

} // namespace luma::config
