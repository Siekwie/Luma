#include "luma/config/graph_builder.hpp"

#include "luma/filesystem/glob.hpp"
#include "luma/filesystem/path_util.hpp"
#include "luma/toolchain/toolchain.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace luma::config {

namespace {

using luma::fs::glob;
using luma::fs::join;
using luma::fs::normalize;
using luma::fs::relative_to;

std::string to_lower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

template <typename T>
void append(std::vector<T>& out, const std::vector<T>& values) {
    out.insert(out.end(), values.begin(), values.end());
}

template <typename T>
void append_unique(std::vector<T>& out, const std::vector<T>& values) {
    for (const auto& value : values) {
        if (std::find(out.begin(), out.end(), value) == out.end()) {
            out.push_back(value);
        }
    }
}

bool is_windows_host() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

bool is_apple_host() {
#ifdef __APPLE__
    return true;
#else
    return false;
#endif
}

std::vector<Path> resolve_sources(const ProjectConfig& config, const TargetDef& target) {
    std::unordered_set<std::string> seen;
    std::vector<Path> sources;

    auto add = [&](Path path) {
        path = fs::lexically_normal(path);
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

std::string object_extension(const ToolchainDef& toolchain) {
    return toolchain.style == ToolchainStyle::Msvc ? ".obj" : ".o";
}

std::string library_prefix(const ToolchainDef& toolchain) {
    return toolchain.style == ToolchainStyle::Msvc ? "" : "lib";
}

std::string library_extension(const ToolchainDef& toolchain, TargetKind kind) {
    if (kind == TargetKind::SharedLibrary) {
        if (is_windows_host()) {
            return ".dll";
        }
        return is_apple_host() ? ".dylib" : ".so";
    }
    return toolchain.style == ToolchainStyle::Msvc ? ".lib" : ".a";
}

std::string executable_extension() {
    return is_windows_host() ? ".exe" : "";
}

/// Object file for a source: <build>/obj/<target>/<source path>.o, so that
/// sources with equal stems in different directories do not collide.
Path object_path(
    const ProjectConfig& config,
    const std::string& target_name,
    const Path& source,
    const ToolchainDef& toolchain) {
    std::string rel = relative_to(join(config.root, source), config.root);
    // Keep objects inside the build directory even for sources outside the root.
    std::string sanitized;
    std::size_t start = 0;
    while (start <= rel.size()) {
        const std::size_t slash = rel.find('/', start);
        std::string segment = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (segment == "..") {
            segment = "__";
        }
        std::replace(segment.begin(), segment.end(), ':', '_');
        if (!segment.empty()) {
            if (!sanitized.empty()) {
                sanitized += '/';
            }
            sanitized += segment;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return join(join(join(config.build_dir, "obj"), target_name), sanitized + object_extension(toolchain));
}

Path output_path(
    const ProjectConfig& config,
    const TargetDef& target,
    const ToolchainDef& toolchain) {
    if (target.kind == TargetKind::Executable) {
        return join(config.build_dir, target.name + executable_extension());
    }
    return join(
        config.build_dir,
        library_prefix(toolchain) + target.name + library_extension(toolchain, target.kind));
}

/// File passed to the linker when linking against a target.
Path link_input_path(const Path& output, const TargetDef& target, const ToolchainDef& toolchain) {
    if (target.kind == TargetKind::SharedLibrary && toolchain.style == ToolchainStyle::Msvc) {
        // MSVC links against the import library written next to the DLL.
        return output.substr(0, output.size() - 4) + ".lib";
    }
    return output;
}

struct CompileInputs {
    SourceLanguage language = SourceLanguage::Cxx;
    std::vector<std::string> flags;
    std::vector<std::string> defines;
    std::vector<Path> include_dirs;
    Path source;
    Path object;
};

std::vector<std::string> gnu_compile_command(
    const ToolchainDef& toolchain, const CompileInputs& in, const Path& root) {
    std::vector<std::string> cmd = {in.language == SourceLanguage::C ? toolchain.cc : toolchain.cxx};
    append(cmd, in.flags);
    for (const auto& define : in.defines) {
        cmd.push_back("-D" + define);
    }
    for (const auto& include : in.include_dirs) {
        cmd.push_back("-I" + join(root, include));
    }
    cmd.push_back("-MMD");
    cmd.push_back("-c");
    cmd.push_back(join(root, in.source));
    cmd.push_back("-o");
    cmd.push_back(join(root, in.object));
    return cmd;
}

std::vector<std::string> msvc_compile_command(
    const ToolchainDef& toolchain, const CompileInputs& in, const Path& root) {
    std::vector<std::string> cmd = {
        in.language == SourceLanguage::C ? toolchain.cc : toolchain.cxx, "/nologo", "/showIncludes", "/c"};
    if (in.language == SourceLanguage::Cxx) {
        cmd.push_back("/EHsc");
    }
    append(cmd, in.flags);
    for (const auto& define : in.defines) {
        cmd.push_back("/D" + define);
    }
    for (const auto& include : in.include_dirs) {
        cmd.push_back("/I" + join(root, include));
    }
    cmd.push_back("/Fo" + join(root, in.object));
    cmd.push_back(join(root, in.source));
    return cmd;
}

std::vector<std::string> compile_command(
    const ToolchainDef& toolchain, const CompileInputs& in, const Path& root) {
    if (toolchain.style == ToolchainStyle::Msvc) {
        return msvc_compile_command(toolchain, in, root);
    }
    return gnu_compile_command(toolchain, in, root);
}

struct LinkInputs {
    TargetKind kind = TargetKind::Executable;
    bool has_cxx = true;
    std::vector<std::string> linkflags;
    std::vector<Path> objects;
    std::vector<Path> libraries;
    std::vector<std::string> system_libs;
    std::vector<Path> rpath_dirs;
    Path output;
};

std::vector<std::string> gnu_link_command(
    const ToolchainDef& toolchain, const LinkInputs& in, const Path& root) {
    std::string linker = toolchain.linker;
    if (linker.empty()) {
        linker = in.has_cxx ? toolchain.cxx : toolchain.cc;
    }
    std::vector<std::string> cmd = {linker};
    if (in.kind == TargetKind::SharedLibrary) {
        cmd.push_back("-shared");
    }
    append(cmd, in.linkflags);
    for (const auto& object : in.objects) {
        cmd.push_back(join(root, object));
    }
    for (const auto& library : in.libraries) {
        cmd.push_back(join(root, library));
    }
    for (const auto& dir : in.rpath_dirs) {
        cmd.push_back("-Wl,-rpath," + join(root, dir));
    }
    for (const auto& lib : in.system_libs) {
        cmd.push_back("-l" + lib);
    }
    cmd.push_back("-o");
    cmd.push_back(join(root, in.output));
    return cmd;
}

std::vector<std::string> msvc_link_command(
    const ToolchainDef& toolchain, const LinkInputs& in, const Path& root) {
    const std::string linker = toolchain.linker.empty() ? "link" : toolchain.linker;
    std::vector<std::string> cmd = {linker, "/nologo"};
    if (in.kind == TargetKind::SharedLibrary) {
        cmd.push_back("/DLL");
    }
    append(cmd, in.linkflags);
    for (const auto& object : in.objects) {
        cmd.push_back(join(root, object));
    }
    for (const auto& library : in.libraries) {
        cmd.push_back(join(root, library));
    }
    for (const auto& lib : in.system_libs) {
        cmd.push_back(lib.ends_with(".lib") ? lib : lib + ".lib");
    }
    cmd.push_back("/OUT:" + join(root, in.output));
    if (in.kind == TargetKind::SharedLibrary) {
        const Path output = join(root, in.output);
        cmd.push_back("/IMPLIB:" + output.substr(0, output.size() - 4) + ".lib");
    }
    return cmd;
}

std::vector<std::string> link_command(
    const ToolchainDef& toolchain, const LinkInputs& in, const Path& root) {
    if (toolchain.style == ToolchainStyle::Msvc) {
        return msvc_link_command(toolchain, in, root);
    }
    return gnu_link_command(toolchain, in, root);
}

std::vector<std::string> archive_command(
    const ToolchainDef& toolchain,
    const std::vector<Path>& objects,
    const Path& output,
    const Path& root) {
    std::vector<std::string> cmd;
    if (toolchain.style == ToolchainStyle::Msvc) {
        const std::string archiver =
            toolchain.archiver.empty() || toolchain.archiver == "ar" ? "lib" : toolchain.archiver;
        cmd = {archiver, "/nologo", "/OUT:" + join(root, output)};
    } else {
        cmd = {toolchain.archiver.empty() ? "ar" : toolchain.archiver, "rcs", join(root, output)};
    }
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

/// Salt mixed into every task's cache key. Everything that shapes a task's
/// command line is already part of the key, so this only names the profile.
std::string fingerprint_config(const ProjectConfig& config, const ProfileDef* profile) {
    std::ostringstream out;
    out << "profile=" << config.profile << '\n';
    out << "lto=" << ((profile && profile->lto) ? 1 : 0) << '\n';
    return out.str();
}

/// Dependencies of `root_name` in link order (dependents before their
/// dependencies), each listed once. With `through_shared` false, the walk
/// does not descend into the dependencies of shared libraries, which already
/// contain them.
std::vector<std::string> transitive_dependencies(
    const ProjectConfig& config, const std::string& root_name, bool through_shared) {
    std::vector<std::string> post_order;
    std::unordered_set<std::string> visited;
    std::function<void(const std::string&, bool)> visit = [&](const std::string& name, bool is_root) {
        if (!visited.insert(name).second) {
            return;
        }
        const TargetDef* target = find_target(config, name);
        if (!target) {
            return;
        }
        if (is_root || through_shared || target->kind != TargetKind::SharedLibrary) {
            for (const auto& dep : target->dependencies) {
                visit(dep, false);
            }
        }
        if (!is_root) {
            post_order.push_back(name);
        }
    };
    visit(root_name, true);
    std::reverse(post_order.begin(), post_order.end());
    return post_order;
}

} // namespace

SourceLanguage source_language(const Path& path) {
    const std::string ext = fs::extension(path);
    if (ext == ".C") {
        return is_windows_host() ? SourceLanguage::C : SourceLanguage::Cxx;
    }
    const std::string lower = to_lower(ext);
    if (lower == ".c") {
        return SourceLanguage::C;
    }
    if (lower == ".cpp" || lower == ".cc" || lower == ".cxx" || lower == ".c++" || lower == ".cp") {
        return SourceLanguage::Cxx;
    }
    return SourceLanguage::Other;
}

std::string canonical_language(const std::string& language) {
    const std::string lower = to_lower(language);
    if (lower == "c") {
        return "c";
    }
    return "cxx";
}

std::string kind_name(TargetKind kind) {
    switch (kind) {
    case TargetKind::Executable:
        return "executable";
    case TargetKind::StaticLibrary:
        return "static_library";
    case TargetKind::SharedLibrary:
        return "shared_library";
    }
    return "executable";
}

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
    out.targets.clear();
    out.toolchain_identity = toolchain::identity_string(toolchain);
    out.configuration_fingerprint = fingerprint_config(config, profile);

    for (const auto& [name, target] : config.targets) {
        for (const auto& dep : target.dependencies) {
            if (!find_target(config, dep)) {
                return BuildPlanError{
                    "target '" + name + "' depends on unknown target '" + dep + "'"};
            }
        }
    }

    std::vector<std::string> target_names;
    if (!only_targets.empty()) {
        std::unordered_set<std::string> needed;
        for (const auto& name : only_targets) {
            if (!find_target(config, name)) {
                return BuildPlanError{"unknown target: " + name};
            }
            needed.insert(name);
            for (const auto& dep : transitive_dependencies(config, name, true)) {
                needed.insert(dep);
            }
        }
        target_names.assign(needed.begin(), needed.end());
    } else {
        for (const auto& [name, _] : config.targets) {
            target_names.push_back(name);
        }
    }
    std::sort(target_names.begin(), target_names.end());

    std::vector<graph::TaskId> generator_tasks;
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
                .dependencies = {},
                .command = expand_generate_command(generator, output, inputs, config.root),
                .environment = {},
                .working_directory = config.root,
                .cache_key = std::nullopt,
                .metadata = {
                    {meta::kRule, "generate"},
                    {meta::kVerb, "generate"},
                    {meta::kDescription, output},
                    {"generator", generator.name},
                },
            };

            generator_tasks.push_back(task.id);
            if (const auto err = out.graph.add_task(std::move(task))) {
                return BuildPlanError{err->message};
            }
        }
    }

    std::unordered_map<std::string, Path> target_outputs;
    for (const auto& name : target_names) {
        target_outputs[name] = output_path(config, *find_target(config, name), toolchain);
    }

    for (const auto& target_name : target_names) {
        const TargetDef& target = *find_target(config, target_name);

        std::vector<std::string> defines;
        if (profile) {
            append(defines, profile->defines);
        }
        append(defines, target.defines);

        std::vector<std::string> cflags;
        std::vector<std::string> cxxflags;
        std::vector<std::string> linkflags;
        if (profile) {
            append(cflags, profile->cflags);
            append(cxxflags, profile->cxxflags);
            append(linkflags, profile->linkflags);
        }
        append(cflags, target.cflags);
        append(cxxflags, target.cxxflags);
        append(linkflags, target.linkflags);

        if (target.kind == TargetKind::SharedLibrary && toolchain.style == ToolchainStyle::Gnu &&
            !is_windows_host()) {
            cflags.push_back("-fPIC");
            cxxflags.push_back("-fPIC");
        }

        const auto sources = resolve_sources(config, target);
        if (sources.empty()) {
            return BuildPlanError{"target '" + target.name + "' has no sources"};
        }

        std::vector<Path> include_dirs;
        append_unique(include_dirs, target.include_dirs);
        for (const auto& dep_name : transitive_dependencies(config, target.name, true)) {
            append_unique(include_dirs, find_target(config, dep_name)->include_dirs);
        }

        std::vector<graph::TaskId> compile_tasks;
        std::vector<Path> objects;
        bool has_cxx = false;
        for (const auto& source : sources) {
            const SourceLanguage language = source_language(source);
            if (language == SourceLanguage::Other) {
                continue; // headers and other files listed as sources are not compiled
            }
            has_cxx = has_cxx || language == SourceLanguage::Cxx;

            const CompileInputs inputs{
                .language = language,
                .flags = language == SourceLanguage::C ? cflags : cxxflags,
                .defines = defines,
                .include_dirs = include_dirs,
                .source = source,
                .object = object_path(config, target.name, source, toolchain),
            };

            graph::Task compile_task{
                .id = graph::TaskId{"compile:" + target.name + ":" + source},
                .inputs = {source},
                .outputs = {inputs.object},
                .dependencies = generator_tasks,
                .command = compile_command(toolchain, inputs, config.root),
                .environment = {},
                .working_directory = config.root,
                .cache_key = std::nullopt,
                .metadata = {
                    {meta::kTarget, target.name},
                    {meta::kRule, "compile"},
                    {meta::kVerb, "compile"},
                    {meta::kDescription, source},
                    {meta::kLanguage, language == SourceLanguage::C ? "c" : "cxx"},
                    {meta::kSource, source},
                },
            };
            if (toolchain.style == ToolchainStyle::Msvc) {
                compile_task.metadata[meta::kDepsFormat] = "msvc";
            } else {
                const Path& object = inputs.object;
                compile_task.metadata[meta::kDepsFormat] = "gcc";
                compile_task.metadata[meta::kDepfile] =
                    object.substr(0, object.size() - object_extension(toolchain).size()) + ".d";
            }

            compile_tasks.push_back(compile_task.id);
            objects.push_back(inputs.object);
            if (const auto err = out.graph.add_task(std::move(compile_task))) {
                return BuildPlanError{err->message};
            }
        }

        if (objects.empty()) {
            return BuildPlanError{"target '" + target.name + "' has no C or C++ sources"};
        }

        const Path output = target_outputs.at(target.name);
        std::vector<graph::TaskId> deps = compile_tasks;
        std::vector<std::string> command;

        if (target.kind == TargetKind::StaticLibrary) {
            command = archive_command(toolchain, objects, output, config.root);
        } else {
            LinkInputs link{
                .kind = target.kind,
                .has_cxx = has_cxx,
                .linkflags = linkflags,
                .objects = objects,
                .libraries = {},
                .system_libs = target.system_libs,
                .rpath_dirs = {},
                .output = output,
            };
            bool links_shared = false;
            for (const auto& dep_name : transitive_dependencies(config, target.name, false)) {
                const TargetDef& dep = *find_target(config, dep_name);
                link.libraries.push_back(link_input_path(target_outputs.at(dep_name), dep, toolchain));
                append_unique(link.system_libs, dep.system_libs);
                links_shared = links_shared || dep.kind == TargetKind::SharedLibrary;
                // A target needs every library it links to be up to date.
                deps.push_back(graph::TaskId{"link:" + dep_name});
            }
            if (links_shared && toolchain.style == ToolchainStyle::Gnu && !is_windows_host() &&
                !is_apple_host()) {
                link.rpath_dirs.push_back(config.build_dir);
            }
            command = link_command(toolchain, link, config.root);
        }

        // Linking against a static library also requires the libraries it
        // depends on to exist, so wait for every transitive dependency.
        for (const auto& dep_name : transitive_dependencies(config, target.name, true)) {
            graph::TaskId dep_id{"link:" + dep_name};
            if (std::find(deps.begin(), deps.end(), dep_id) == deps.end()) {
                deps.push_back(dep_id);
            }
        }

        const bool is_archive = target.kind == TargetKind::StaticLibrary;
        graph::Task link_task{
            .id = graph::TaskId{"link:" + target.name},
            .inputs = objects,
            .outputs = {output},
            .dependencies = deps,
            .command = std::move(command),
            .environment = {},
            .working_directory = config.root,
            .cache_key = std::nullopt,
            .metadata = {
                {meta::kTarget, target.name},
                {meta::kRule, is_archive ? "archive" : "link"},
                {meta::kVerb, is_archive ? "archive" : "link"},
                {meta::kDescription, output},
            },
        };

        auto& tasks = out.target_tasks[target.name];
        tasks = compile_tasks;
        tasks.push_back(link_task.id);

        if (const auto err = out.graph.add_task(std::move(link_task))) {
            return BuildPlanError{err->message};
        }

        out.targets.push_back(TargetInfo{
            .name = target.name,
            .kind = target.kind,
            .language = canonical_language(target.language),
            .output = output,
            .sources = sources,
            .include_dirs = include_dirs,
            .defines = defines,
            .dependencies = target.dependencies,
        });
    }

    if (const auto err = out.graph.validate()) {
        return BuildPlanError{err->message};
    }

    return std::nullopt;
}

} // namespace luma::config
