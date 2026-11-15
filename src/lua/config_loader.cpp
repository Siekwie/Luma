#include "luma/lua/config_loader.hpp"

#include "luma/executor/process.hpp"
#include "luma/filesystem/path_util.hpp"
#include "luma/toolchain/toolchain.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>
#include <vector>

namespace luma::lua_loader {

namespace {

namespace fs = luma::fs;

using config::GenerateDef;
using config::ProfileDef;
using config::TargetDef;
using config::TargetKind;
using config::ToolchainDef;
using config::ToolchainStyle;

constexpr const char* kConfigKey = "luma.config";

struct LoaderState {
    config::ProjectConfig* project = nullptr;
    std::string error;
};

LoaderState* state(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kConfigKey);
    auto* ctx = static_cast<LoaderState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return ctx;
}

void fail(lua_State* L, const std::string& message) {
    if (auto* ctx = state(L)) {
        ctx->error = message;
    }
    luaL_error(L, "%s", message.c_str());
}

bool is_table(lua_State* L, int index) {
    return lua_istable(L, index);
}

/// Read a string or an array of strings (in array order) at `index`.
std::vector<std::string> read_string_array(lua_State* L, int index) {
    std::vector<std::string> values;
    if (lua_type(L, index) == LUA_TSTRING) {
        values.emplace_back(lua_tostring(L, index));
        return values;
    }
    if (!is_table(L, index)) {
        return values;
    }

    const int table = lua_absindex(L, index);
    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, table));
    for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(L, table, i);
        if (lua_isstring(L, -1)) {
            values.emplace_back(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }
    return values;
}

std::vector<Path> read_path_array(lua_State* L, int index) {
    std::vector<Path> values;
    for (const auto& value : read_string_array(L, index)) {
        values.push_back(fs::normalize(value));
    }
    return values;
}

std::string read_string_field(lua_State* L, int index, const char* key) {
    lua_getfield(L, index, key);
    std::string value;
    if (lua_isstring(L, -1)) {
        value = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

bool read_bool_field(lua_State* L, int index, const char* key, bool default_value) {
    lua_getfield(L, index, key);
    bool value = default_value;
    if (lua_isboolean(L, -1)) {
        value = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);
    return value;
}

ToolchainStyle read_toolchain_style(const std::string& style, const std::string& cxx) {
    if (style == "msvc") {
        return ToolchainStyle::Msvc;
    }
    // Infer from the compiler's file name: "cl", "cl.exe", "C:/.../cl.exe",
    // "clang-cl" and "clang-cl.exe" all take MSVC-style arguments.
    const std::size_t slash = cxx.find_last_of("/\\");
    std::string name = slash == std::string::npos ? cxx : cxx.substr(slash + 1);
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (name.ends_with(".exe")) {
        name.resize(name.size() - 4);
    }
    return name == "cl" || name == "clang-cl" ? ToolchainStyle::Msvc : ToolchainStyle::Gnu;
}

/// Support the call-chaining form `target "name" { ... }`: when a
/// declaration is called with only a name, return a function that takes
/// the table.
template <lua_CFunction Declaration>
int curried_declaration(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    return Declaration(L);
}

template <lua_CFunction Declaration>
bool return_curried(lua_State* L) {
    if (lua_gettop(L) != 1) {
        return false;
    }
    luaL_checkstring(L, 1);
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, curried_declaration<Declaration>, 1);
    return true;
}

int api_project(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (auto* ctx = state(L)) {
        ctx->project->name = name;
    }
    return 0;
}

int api_toolchain(lua_State* L) {
    if (return_curried<api_toolchain>(L)) {
        return 1;
    }
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    ToolchainDef toolchain;
    toolchain.name = name;
    toolchain.cc = read_string_field(L, 2, "cc");
    toolchain.cxx = read_string_field(L, 2, "cxx");
    toolchain.linker = read_string_field(L, 2, "linker");
    if (toolchain.linker.empty()) {
        toolchain.linker = read_string_field(L, 2, "link");
    }
    toolchain.archiver = read_string_field(L, 2, "archive");
    if (toolchain.archiver.empty()) {
        toolchain.archiver = read_string_field(L, 2, "archiver");
    }
    if (toolchain.archiver.empty()) {
        toolchain.archiver = "ar";
    }
    lua_getfield(L, 2, "languages");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        toolchain.languages = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    const std::string kind = read_string_field(L, 2, "kind");
    if (toolchain.cxx.empty() && !toolchain.cc.empty()) {
        toolchain.cxx = toolchain.cc;
    }
    if (toolchain.cxx.empty()) {
        toolchain.cxx = "c++";
    }
    toolchain.style = read_toolchain_style(kind, toolchain.cxx);
    if (toolchain.cc.empty()) {
        // cl.exe / clang-cl compile C and C++ alike; there is no "cc" next to them.
        toolchain.cc = toolchain.style == ToolchainStyle::Msvc ? toolchain.cxx : "cc";
    }

    if (auto* ctx = state(L)) {
        ctx->project->toolchains[name] = std::move(toolchain);
        if (ctx->project->active_toolchain.empty()) {
            ctx->project->active_toolchain = name;
        }
    }
    return 0;
}

int api_profile(lua_State* L) {
    if (return_curried<api_profile>(L)) {
        return 1;
    }
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    ProfileDef profile;
    profile.name = name;
    lua_getfield(L, 2, "defines");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        profile.defines = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "cflags");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        profile.cflags = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "cxxflags");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        profile.cxxflags = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "linkflags");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        profile.linkflags = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "lto");
    profile.lto_set = lua_isboolean(L, -1);
    profile.lto = profile.lto_set && lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 2, "inherit");
    if (lua_type(L, -1) == LUA_TSTRING) {
        profile.inherit_from = lua_tostring(L, -1);
        profile.inherit = true;
    } else if (lua_isboolean(L, -1)) {
        profile.inherit = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);

    if (auto* ctx = state(L)) {
        ctx->project->profiles[name] = std::move(profile);
    }
    return 0;
}

TargetKind read_target_kind(lua_State* L, int index) {
    const std::string kind = read_string_field(L, index, "kind");
    if (kind == "static_library" || kind == "static" || kind == "lib") {
        return TargetKind::StaticLibrary;
    }
    if (kind == "shared_library" || kind == "shared" || kind == "dll") {
        return TargetKind::SharedLibrary;
    }
    return TargetKind::Executable;
}

void read_sources(lua_State* L, int index, TargetDef& target) {
    lua_getfield(L, index, "sources");
    if (lua_isstring(L, -1)) {
        target.source_patterns.push_back(lua_tostring(L, -1));
    } else if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_isstring(L, -1)) {
                const std::string value = lua_tostring(L, -1);
                if (value.find('*') != std::string::npos) {
                    target.source_patterns.push_back(value);
                } else {
                    target.sources.push_back(fs::normalize(value));
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

int api_target(lua_State* L) {
    if (return_curried<api_target>(L)) {
        return 1;
    }
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    TargetDef target;
    target.name = name;
    target.kind = read_target_kind(L, 2);
    target.test = read_bool_field(L, 2, "test", false);
    target.language = read_string_field(L, 2, "language");
    if (target.language.empty()) {
        target.language = "cxx";
    }
    read_sources(L, 2, target);

    lua_getfield(L, 2, "include_dirs");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        target.include_dirs = read_path_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "defines");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        target.defines = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "cflags");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        target.cflags = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "cxxflags");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        target.cxxflags = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "linkflags");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        target.linkflags = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "dependencies");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        target.dependencies = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "public_defines");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        target.public_defines = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "public_include_dirs");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        target.public_include_dirs = read_path_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "public_cflags");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        target.public_cflags = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "public_cxxflags");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        target.public_cxxflags = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "system_libs");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        target.system_libs = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    if (auto* ctx = state(L)) {
        ctx->project->targets[name] = std::move(target);
    }
    return 0;
}

int api_generate(lua_State* L) {
    if (return_curried<api_generate>(L)) {
        return 1;
    }
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    GenerateDef generator;
    generator.name = name;

    lua_getfield(L, 2, "inputs");
    if (lua_isstring(L, -1)) {
        generator.input_patterns.push_back(lua_tostring(L, -1));
    } else if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_isstring(L, -1)) {
                const std::string value = lua_tostring(L, -1);
                if (value.find('*') != std::string::npos) {
                    generator.input_patterns.push_back(value);
                } else {
                    generator.inputs.push_back(fs::normalize(value));
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "outputs");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        generator.outputs = read_path_array(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "command");
    if (lua_istable(L, -1) || lua_type(L, -1) == LUA_TSTRING) {
        generator.command = read_string_array(L, -1);
    }
    lua_pop(L, 1);

    if (auto* ctx = state(L)) {
        ctx->project->generators.push_back(std::move(generator));
    }
    return 0;
}

int api_glob(lua_State* L) {
    const char* pattern = luaL_checkstring(L, 1);
    lua_pushstring(L, pattern);
    return 1;
}

int api_set_builddir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (auto* ctx = state(L)) {
        ctx->project->build_dir = fs::normalize(path);
    }
    return 0;
}

int api_set_toolchain(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (auto* ctx = state(L)) {
        ctx->project->active_toolchain = name;
    }
    return 0;
}

int api_set_profile(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (auto* ctx = state(L)) {
        ctx->project->profile = name;
    }
    return 0;
}

int api_when_os(lua_State* L) {
    const char* os_name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

#if defined(_WIN32)
    const char* current = "windows";
#elif defined(__APPLE__)
    const char* current = "macos";
#else
    const char* current = "linux";
#endif

    if (std::strcmp(os_name, current) == 0) {
        lua_pushvalue(L, 2);
        lua_call(L, 0, 0);
    }
    return 0;
}

int api_query(lua_State* L) {
    int argc = lua_gettop(L);
    if (argc < 1) {
        fail(L, "query requires a command");
    }

    std::vector<std::string> command;
    for (int i = 1; i <= argc; ++i) {
        command.emplace_back(luaL_checkstring(L, i));
    }

    const auto result = executor::run_process(command);
    std::string output = !result.stdout_text.empty() ? result.stdout_text : result.stderr_text;
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    lua_pushstring(L, output.c_str());
    return 1;
}

void register_api(lua_State* L) {
    const luaL_Reg funcs[] = {
        {"project", api_project},
        {"toolchain", api_toolchain},
        {"profile", api_profile},
        {"target", api_target},
        {"generate", api_generate},
        {"glob", api_glob},
        {"builddir", api_set_builddir},
        {"set_toolchain", api_set_toolchain},
        {"set_profile", api_set_profile},
        {"when_os", api_when_os},
        {"query", api_query},
        {nullptr, nullptr},
    };
    lua_pushglobaltable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_pop(L, 1);
}

/// luaL_dofile() equivalent that opens the file through std::filesystem:
/// luaL_loadfile() uses fopen(), which on Windows reads a char* path in the
/// ANSI code page, so UTF-8 project paths outside it could not be opened.
/// Like luaL_loadfile(), skips a UTF-8 BOM and a first line starting with '#'.
int run_script(lua_State* L, const Path& script_path) {
    std::ifstream in(fs::to_fs_path(script_path), std::ios::binary);
    if (!in) {
        lua_pushfstring(L, "cannot open %s", script_path.c_str());
        return LUA_ERRFILE;
    }
    std::string source{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
        lua_pushfstring(L, "cannot read %s", script_path.c_str());
        return LUA_ERRFILE;
    }
    if (source.starts_with("\xEF\xBB\xBF")) {
        source.erase(0, 3);
    }
    if (source.starts_with("#")) {
        // Keep the newline so that line numbers in error messages stay right.
        source.erase(0, std::min(source.find('\n'), source.size()));
    }
    const std::string chunk_name = "@" + script_path;
    if (const int status = luaL_loadbuffer(L, source.data(), source.size(), chunk_name.c_str());
        status != LUA_OK) {
        return status;
    }
    return lua_pcall(L, 0, LUA_MULTRET, 0);
}

} // namespace

std::optional<LoadError> load_config(const Path& script_path, config::ProjectConfig& out) {
    lua_State* L = luaL_newstate();
    if (!L) {
        return LoadError{"failed to create Lua state"};
    }

    luaL_openlibs(L);

    LoaderState ctx;
    ctx.project = &out;

    lua_pushlightuserdata(L, &ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, kConfigKey);

    register_api(L);

    if (const int status = run_script(L, script_path); status != LUA_OK) {
        LoadError error{lua_tostring(L, -1)};
        lua_close(L);
        return error;
    }

    if (!ctx.error.empty()) {
        LoadError error{ctx.error};
        lua_close(L);
        return error;
    }

    lua_close(L);

    if (out.toolchains.empty()) {
        out.toolchains["auto"] = toolchain::detect_default();
        out.active_toolchain = "auto";
    }

    {
        const ToolchainDef& toolchain = out.toolchains[out.active_toolchain];
        ProfileDef debug;
        debug.name = "debug";
        debug.defines = {"DEBUG"};
        ProfileDef release;
        release.name = "release";
        release.defines = {"NDEBUG"};
        if (toolchain.style == ToolchainStyle::Msvc) {
            // /Z7 keeps debug info in each object file, so parallel cl.exe
            // processes never contend for a shared .pdb.
            debug.cflags = {"/Od", "/Z7", "/MDd", "/W4"};
            debug.cxxflags = {"/Od", "/Z7", "/MDd", "/W4", "/std:c++20"};
            debug.linkflags = {"/DEBUG"};
            release.cflags = {"/O2", "/MD", "/W4"};
            release.cxxflags = {"/O2", "/MD", "/W4", "/std:c++20"};
        } else {
            debug.cflags = {"-O0", "-g", "-Wall", "-Wextra"};
            debug.cxxflags = {"-O0", "-g", "-Wall", "-Wextra", "-std=c++20"};
            release.cflags = {"-O3", "-Wall", "-Wextra"};
            release.cxxflags = {"-O3", "-Wall", "-Wextra", "-std=c++20"};
        }
        // Built-in profiles: added when missing, used as the base of user profiles
        // with the same name (user flags come last, so they win), unless `inherit = false`.
        auto merge = [](ProfileDef base, const ProfileDef& user) {
            if (!user.inherit) return user;
            auto append = [](std::vector<std::string>& to, const std::vector<std::string>& from) {
                for (const auto& f : from) {
                    if (std::find(to.begin(), to.end(), f) == to.end()) to.push_back(f);
                }
            };
            append(base.defines, user.defines);
            append(base.cflags, user.cflags);
            append(base.cxxflags, user.cxxflags);
            append(base.linkflags, user.linkflags);
            if (user.lto_set) {
                base.lto = user.lto;
                base.lto_set = true;
            }
            base.inherit = true;
            return base;
        };
        for (ProfileDef* builtin : {&debug, &release}) {
            auto it = out.profiles.find(builtin->name);
            if (it != out.profiles.end() && !it->second.inherit_from.empty()) continue;  // resolved below
            if (it == out.profiles.end()) {
                // Only add missing built-ins when the project declared no profiles at all,
                // so a project that defines e.g. only "dev" keeps exactly that.
                bool anyUser = false;
                for (const auto& [n, p] : out.profiles) {
                    (void)p;
                    if (n != "debug" && n != "release") anyUser = true;
                }
                if (!anyUser) out.profiles[builtin->name] = *builtin;
            } else {
                it->second = merge(*builtin, it->second);
            }
        }

        // Named inheritance (`inherit = "debug"`), resolved parent-first; chains allowed, cycles not.
        std::map<std::string, int> state;  // 1 = resolving, 2 = done
        std::optional<std::string> err;
        std::function<void(const std::string&)> resolve = [&](const std::string& name) {
            if (err || state[name] == 2) return;
            ProfileDef& p = out.profiles[name];
            if (p.inherit_from.empty()) {
                state[name] = 2;
                return;
            }
            if (state[name] == 1) {
                err = "profile '" + name + "' inherits from itself (through '" + p.inherit_from + "')";
                return;
            }
            state[name] = 1;
            const std::string parent = p.inherit_from;
            if (!out.profiles.count(parent)) {
                const bool builtinName = parent == "debug" || parent == "release";
                if (!builtinName) {
                    err = "profile '" + name + "' inherits from unknown profile '" + parent + "'";
                    return;
                }
                out.profiles[parent] = parent == "debug" ? debug : release;  // e.g. only "asan" declared
            }
            resolve(parent);
            if (err) return;
            ProfileDef merged = merge(out.profiles[parent], p);
            merged.name = name;
            merged.inherit_from.clear();
            out.profiles[name] = std::move(merged);
            state[name] = 2;
        };
        std::vector<std::string> names;
        for (const auto& [n, p] : out.profiles) names.push_back(n);
        for (const auto& n : names) resolve(n);
        if (err) return LoadError{*err};
    }

    return std::nullopt;
}

} // namespace luma::lua_loader
