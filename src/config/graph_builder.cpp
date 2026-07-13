#include "luma/config/graph_builder.hpp"

#include "luma/filesystem/glob.hpp"
#include "luma/filesystem/path_util.hpp"
#include "luma/toolchain/toolchain.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <unordered_set>

namespace luma::config {

namespace {

using luma::fs::glob;
using luma::fs::join;
using luma::fs::normalize;
using luma::fs::relative_to;
using luma::fs::stem;

std::string to_lower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

TargetKind parse_kind(const std::string& kind) {
    const std::string lower = to_lower(kind);
    if (lower == "executable" || lower == "exe" || lower == "app") {
        return TargetKind::Executable;
    }
    if (lower == "static_library" || lower == "static" || lower == "lib") {
        return TargetKind::StaticLibrary;
    }
    if (lower == "shared_library" || lower == "shared" || lower == "dll") {
        return TargetKind::SharedLibrary;
    }
    return TargetKind::Executable;
}

std::vector<Path> resolve_sources(const ProjectConfig& config, const TargetDef& target) {
    std::unordered_set<std::string> seen;
    std::vector<Path> sources;

    auto add = [&](Path path) {
        path = normalize(path);
        if (seen.insert(path).second) {
            sources.push_back(std::move(path));
        }
    };

    for (const auto& source : target.sources) {
        add(source);
    }
    for (const auto& pattern : target.source_patterns) {
        for (const auto& match : glob(config.root, pattern)) {
            add(match);
        }
    }

    std::sort(sources.begin(), sources.end());
    return sources;
}

std::vector<std::string> merged_defines(
    const ProfileDef* profile,
    const TargetDef& target) {
    std::vector<std::string> out;
    if (profile) {
        out.insert(out.end(), profile->defines.begin(), profile->defines.end());
    }
    out.insert(out.end(), target.defines.begin(), target.defines.end());
    return out;
}

std::vector<std::string> merged_cxxflags(
    const ProfileDef* profile,
    const TargetDef& target) {
    std::vector<std::string> out;
    if (profile) {
        out.insert(out.end(), profile->cxxflags.begin(), profile->cxxflags.end());
    }
    out.insert(out.end(), target.cxxflags.begin(), target.cxxflags.end());
    return out;
}

std::vector<std::string> merged_linkflags(
    const ProfileDef* profile,
    const TargetDef& target) {
    std::vector<std::string> out;
    if (profile) {
        out.insert(out.end(), profile->linkflags.begin(), profile->linkflags.end());
    }
    out.insert(out.end(), target.linkflags.begin(), target.linkflags.end());
    return out;
}

std::string object_extension(const ToolchainDef& toolchain) {
    return toolchain.style == ToolchainStyle::Msvc ? ".obj" : ".o";
}

std::string library_prefix(const ToolchainDef& toolchain) {
    (void)toolchain;
    return "lib";
}

std::string library_extension(const ToolchainDef& toolchain, TargetKind kind) {
    if (kind == TargetKind::SharedLibrary) {
#ifdef _WIN32
        return toolchain.style == ToolchainStyle::Msvc ? ".dll" : ".dll";
#else
        return ".so";
#endif
    }
#ifdef _WIN32
    return toolchain.style == ToolchainStyle::Msvc ? ".lib" : ".a";
#else
    return ".a";
#endif
}

std::string executable_extension(const ToolchainDef& toolchain) {
#ifdef _WIN32
    (void)toolchain;
    return ".exe";
#else
    (void)toolchain;
    return "";
#endif
}

Path object_path(
    const ProjectConfig& config,
    const std::string& target_name,
    const Path& source,
    const ToolchainDef& toolchain) {
    const Path rel = relative_to(source, config.root);
    const std::string obj = stem(rel) + object_extension(toolchain);
    return join(join(join(config.build_dir, "obj"), target_name), obj);
}

Path output_path(
    const ProjectConfig& config,
    const TargetDef& target,
    const ToolchainDef& toolchain) {
    if (target.kind == TargetKind::Executable) {
        return join(config.build_dir, target.name + executable_extension(toolchain));
    }
    return join(
        config.build_dir,
        library_prefix(toolchain) + target.name + library_extension(toolchain, target.kind));
}

std::vector<std::string> gnu_compile_command(
    const ToolchainDef& toolchain,
    const std::vector<std::string>& flags,
    const std::vector<std::string>& defines,
    const std::vector<Path>& include_dirs,
    const Path& source,
    const Path& output,
    const Path& root) {
    std::vector<std::string> cmd = {toolchain.cxx};
    cmd.insert(cmd.end(), flags.begin(), flags.end());
    for (const auto& define : defines) {
        cmd.push_back("-D" + define);
    }
    for (const auto& include : include_dirs) {
        cmd.push_back("-I" + join(root, include));
    }
    cmd.push_back("-c");
    cmd.push_back(join(root, source));
    cmd.push_back("-o");
    cmd.push_back(join(root, output));
    return cmd;
}

std::vector<std::string> msvc_compile_command(
    const ToolchainDef& toolchain,
    const std::vector<std::string>& flags,
    const std::vector<std::string>& defines,
    const std::vector<Path>& include_dirs,
    const Path& source,
    const Path& output,
    const Path& root) {
    std::vector<std::string> cmd = {toolchain.cxx, "/nologo", "/c", "/EHsc"};
    cmd.insert(cmd.end(), flags.begin(), flags.end());
    for (const auto& define : defines) {
        cmd.push_back("/D" + define);
    }
    for (const auto& include : include_dirs) {
        cmd.push_back("/I" + join(root, include));
    }
    cmd.push_back("/Fo" + join(root, output));
    cmd.push_back(join(root, source));
    return cmd;
}

std::vector<std::string> compile_command(
    const ToolchainDef& toolchain,
    const std::vector<std::string>& flags,
    const std::vector<std::string>& defines,
    const std::vector<Path>& include_dirs,
    const Path& source,
    const Path& output,
    const Path& root) {
    if (toolchain.style == ToolchainStyle::Msvc) {
        return msvc_compile_command(toolchain, flags, defines, include_dirs, source, output, root);
    }
    return gnu_compile_command(toolchain, flags, defines, include_dirs, source, output, root);
}

std::vector<std::string> gnu_link_executable(
    const ToolchainDef& toolchain,
    const std::vector<std::string>& linkflags,
    const std::vector<Path>& objects,
    const std::vector<Path>& libraries,
    const std::vector<std::string>& system_libs,
    const Path& output,
    const Path& root) {
    const std::string linker = toolchain.linker.empty() ? toolchain.cxx : toolchain.linker;
    std::vector<std::string> cmd = {linker};
    cmd.insert(cmd.end(), linkflags.begin(), linkflags.end());
    for (const auto& object : objects) {
        cmd.push_back(join(root, object));
    }
    for (const auto& library : libraries) {
        cmd.push_back(join(root, library));
    }
    for (const auto& lib : system_libs) {
        cmd.push_back("-l" + lib);
    }
    cmd.push_back("-o");
    cmd.push_back(join(root, output));
    return cmd;
}

std::vector<std::string> msvc_link_executable(
    const ToolchainDef& toolchain,
    const std::vector<std::string>& linkflags,
    const std::vector<Path>& objects,
    const std::vector<Path>& libraries,
    const std::vector<std::string>& system_libs,
    const Path& output,
    const Path& root) {
    const std::string linker = toolchain.linker.empty() ? "link" : toolchain.linker;
    std::vector<std::string> cmd = {linker, "/nologo"};
    cmd.insert(cmd.end(), linkflags.begin(), linkflags.end());
    for (const auto& object : objects) {
        cmd.push_back(join(root, object));
    }
    for (const auto& library : libraries) {
        cmd.push_back(join(root, library));
    }
    for (const auto& lib : system_libs) {
        cmd.push_back(lib + ".lib");
    }
    cmd.push_back("/OUT:" + join(root, output));
    return cmd;
}

std::vector<std::string> link_executable_command(
    const ToolchainDef& toolchain,
    const std::vector<std::string>& linkflags,
    const std::vector<Path>& objects,
    const std::vector<Path>& libraries,
    const std::vector<std::string>& system_libs,
    const Path& output,
    const Path& root) {
    if (toolchain.style == ToolchainStyle::Msvc) {
        return msvc_link_executable(
            toolchain, linkflags, objects, libraries, system_libs, output, root);
    }
    return gnu_link_executable(
        toolchain, linkflags, objects, libraries, system_libs, output, root);
}

std::vector<std::string> archive_command(
    const ToolchainDef& toolchain,
    const std::vector<Path>& objects,
    const Path& output,
    const Path& root) {
    if (toolchain.style == ToolchainStyle::Msvc) {
        std::vector<std::string> cmd = {"lib", "/nologo", "/OUT:" + join(root, output)};
        for (const auto& object : objects) {
            cmd.push_back(join(root, object));
        }
        return cmd;
    }

    std::vector<std::string> cmd = {toolchain.archiver, "rcs", join(root, output)};
    for (const auto& object : objects) {
        cmd.push_back(join(root, object));
    }
    return cmd;
}

std::string replace_token(std::string value, const std::string& token, const std::string& replacement) {
    std::size_t pos = 0;
    while ((pos = value.find(token, pos)) != std::string::npos) {
        value.replace(pos, token.size(), replacement);
        pos += replacement.size();
    }
    return value;
}

std::vector<std::string> expand_generate_command(
    const GenerateDef& generator,
    const Path& output,
    const std::vector<Path>& inputs,
    const Path& root) {
    std::vector<std::string> cmd = generator.command;
    for (auto& arg : cmd) {
        arg = replace_token(arg, "$output", join(root, output));
        arg = replace_token(arg, "$root", root);
        if (arg.find("$inputs") != std::string::npos) {
            std::ostringstream joined;
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                if (i != 0) {
                    joined << ' ';
                }
                joined << join(root, inputs[i]);
            }
            arg = replace_token(arg, "$inputs", joined.str());
        }
    }
    return cmd;
}

std::string fingerprint_config(const ProjectConfig& config, const ProfileDef* profile) {
    std::ostringstream out;
    out << "profile=" << config.profile << '\n';
    if (profile) {
        for (const auto& flag : profile->cxxflags) {
            out << "p_cxx=" << flag << '\n';
        }
        for (const auto& define : profile->defines) {
            out << "p_def=" << define << '\n';
        }
    }
    for (const auto& [name, target] : config.targets) {
        out << "target=" << name << '\n';
        for (const auto& flag : target.cxxflags) {
            out << "t_cxx=" << flag << '\n';
        }
    }
    return out.str();
}

} // namespace

std::optional<BuildPlanError> build_plan(
    const ProjectConfig& config,
    BuildPlan& out,
    const std::vector<std::string>& only_targets) {
    const ToolchainDef* toolchain_ptr = find_toolchain(config);
    if (!toolchain_ptr) {
        return BuildPlanError{"no toolchain configured"};
    }
    const ToolchainDef toolchain = *toolchain_ptr;
    const ProfileDef* profile = find_profile(config, config.profile);

    out.graph = graph::BuildGraph{};
    out.target_tasks.clear();
    out.toolchain_identity = toolchain::identity_string(toolchain);
    out.configuration_fingerprint = fingerprint_config(config, profile);

    std::vector<std::string> target_names;
    if (!only_targets.empty()) {
        std::unordered_set<std::string> needed(only_targets.begin(), only_targets.end());
        bool added = true;
        while (added) {
            added = false;
            for (const auto& name : needed) {
                const TargetDef* target = find_target(config, name);
                if (!target) {
                    return BuildPlanError{"unknown target: " + name};
                }
                for (const auto& dep : target->dependencies) {
                    if (!needed.contains(dep)) {
                        needed.insert(dep);
                        added = true;
                    }
                }
            }
        }
        target_names.assign(needed.begin(), needed.end());
        std::sort(target_names.begin(), target_names.end());
    } else {
        for (const auto& [name, _] : config.targets) {
            target_names.push_back(name);
        }
        std::sort(target_names.begin(), target_names.end());
    }

    std::unordered_map<std::string, Path> target_outputs;
    std::unordered_map<std::string, std::vector<graph::TaskId>> target_object_tasks;

    for (const auto& generator : config.generators) {
        std::vector<Path> inputs = generator.inputs;
        for (const auto& pattern : generator.input_patterns) {
            for (const auto& match : glob(config.root, pattern)) {
                inputs.push_back(match);
            }
        }
        std::sort(inputs.begin(), inputs.end());
        inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());

        for (const auto& output : generator.outputs) {
            graph::Task task{
                .id = graph::TaskId{"gen:" + generator.name + ":" + output},
                .inputs = inputs,
                .outputs = {output},
                .command = expand_generate_command(generator, output, inputs, config.root),
                .working_directory = config.root,
                .metadata = {{"generator", generator.name}, {"rule", "generate"}},
            };

            if (const auto err = out.graph.add_task(std::move(task))) {
                return BuildPlanError{err->message};
            }
        }
    }

    for (const auto& target_name : target_names) {
        const TargetDef* target_ptr = find_target(config, target_name);
        if (!target_ptr) {
            return BuildPlanError{"unknown target: " + target_name};
        }
        const TargetDef& target = *target_ptr;

        const auto defines = merged_defines(profile, target);
        const auto cxxflags = merged_cxxflags(profile, target);
        const auto linkflags = merged_linkflags(profile, target);
        const auto sources = resolve_sources(config, target);

        if (sources.empty()) {
            return BuildPlanError{"target '" + target.name + "' has no sources"};
        }

        std::vector<Path> include_dirs = target.include_dirs;
        for (const auto& dep_name : target.dependencies) {
            const TargetDef* dep = find_target(config, dep_name);
            if (!dep) {
                return BuildPlanError{
                    "target '" + target.name + "' depends on unknown target '" + dep_name + "'"};
            }
            include_dirs.insert(include_dirs.end(), dep->include_dirs.begin(), dep->include_dirs.end());
        }

        std::vector<graph::TaskId> compile_tasks;
        for (const auto& source : sources) {
            const Path object = object_path(config, target.name, source, toolchain);
            graph::TaskId task_id{"compile:" + target.name + ":" + source};

            graph::Task compile_task{
                .id = task_id,
                .inputs = {source},
                .outputs = {object},
                .command = compile_command(
                    toolchain, cxxflags, defines, include_dirs, source, object, config.root),
                .working_directory = config.root,
                .metadata = {{"target", target.name}, {"rule", "compile-cxx"}, {"source", source}},
            };

            if (const auto err = out.graph.add_task(std::move(compile_task))) {
                return BuildPlanError{err->message};
            }
            compile_tasks.push_back(task_id);
        }

        target_object_tasks[target.name] = compile_tasks;
        target_outputs[target.name] = output_path(config, target, toolchain);
    }

    for (const auto& target_name : target_names) {
        const TargetDef& target = *find_target(config, target_name);
        const auto linkflags = merged_linkflags(profile, target);
        const Path output = target_outputs.at(target.name);
        const auto& compile_tasks = target_object_tasks.at(target.name);

        std::vector<Path> objects;
        for (const auto& task_id : compile_tasks) {
            const graph::Task* task = out.graph.find_task(task_id);
            if (task && !task->outputs.empty()) {
                objects.push_back(task->outputs.front());
            }
        }

        std::vector<graph::TaskId> deps = compile_tasks;
        std::vector<Path> libraries;
        std::vector<Path> dep_include_dirs = target.include_dirs;

        for (const auto& dep_name : target.dependencies) {
            const auto dep_out = target_outputs.find(dep_name);
            if (dep_out == target_outputs.end()) {
                return BuildPlanError{
                    "target '" + target.name + "' depends on unbuilt target '" + dep_name + "'"};
            }
            libraries.push_back(dep_out->second);
            deps.push_back(graph::TaskId{"link:" + dep_name});

            const auto dep_objects = target_object_tasks.find(dep_name);
            if (dep_objects != target_object_tasks.end()) {
                for (const auto& dep_task : dep_objects->second) {
                    deps.push_back(dep_task);
                }
            }

            if (const TargetDef* dep = find_target(config, dep_name)) {
                dep_include_dirs.insert(
                    dep_include_dirs.end(), dep->include_dirs.begin(), dep->include_dirs.end());
            }
        }

        graph::TaskId link_id{"link:" + target.name};
        std::vector<std::string> command;

        if (target.kind == TargetKind::StaticLibrary) {
            command = archive_command(toolchain, objects, output, config.root);
        } else {
            command = link_executable_command(
                toolchain,
                linkflags,
                objects,
                libraries,
                target.system_libs,
                output,
                config.root);
        }

        graph::Task link_task{
            .id = link_id,
            .inputs = objects,
            .outputs = {output},
            .dependencies = deps,
            .command = std::move(command),
            .working_directory = config.root,
            .metadata = {{"target", target.name}, {"rule", "link"}},
        };

        if (const auto err = out.graph.add_task(std::move(link_task))) {
            return BuildPlanError{err->message};
        }

        auto& tasks = out.target_tasks[target.name];
        tasks = compile_tasks;
        tasks.push_back(link_id);
    }

    if (const auto err = out.graph.validate()) {
        return BuildPlanError{err->message};
    }

    return std::nullopt;
}

} // namespace luma::config
