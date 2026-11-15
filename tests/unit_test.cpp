#include "luma/cache/cache_store.hpp"
#include "luma/cli/introspect.hpp"
#include "luma/config/graph_builder.hpp"
#include "luma/executor/depfile.hpp"
#include "luma/filesystem/glob.hpp"
#include "luma/filesystem/path_util.hpp"
#include "luma/filesystem/sha256.hpp"
#include "luma/util/json.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = luma::fs;
using namespace luma;

static int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cerr << "FAIL: " << #cond << " (" << __FILE__ << ':' << __LINE__ << ")\n"; \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        const auto& va = (a);                                                              \
        const auto& vb = (b);                                                              \
        if (!(va == vb)) {                                                                 \
            std::cerr << "FAIL: " << #a << " == " << #b << "\n  got:      " << va          \
                      << "\n  expected: " << vb << " (" << __FILE__ << ':' << __LINE__ << ")\n"; \
            ++failures;                                                                    \
        }                                                                                  \
    } while (0)

static void test_sha256() {
    CHECK_EQ(fs::sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(fs::sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(
        fs::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    // One million 'a', fed in uneven chunks to exercise buffering.
    fs::Sha256 hasher;
    const std::string chunk(997, 'a');
    std::size_t remaining = 1000000;
    while (remaining > 0) {
        const std::size_t take = std::min(remaining, chunk.size());
        hasher.update(chunk.data(), take);
        remaining -= take;
    }
    CHECK_EQ(hasher.finish_hex(), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

static void test_json() {
    CHECK_EQ(util::json_quote("plain"), std::string("\"plain\""));
    CHECK_EQ(util::json_quote("a\"b\\c"), std::string("\"a\\\"b\\\\c\""));
    CHECK_EQ(util::json_quote("line\nnext\ttab\r"), std::string("\"line\\nnext\\ttab\\r\""));
    CHECK_EQ(util::json_quote(std::string("\x01", 1)), std::string("\"\\u0001\""));
    CHECK_EQ(util::json_quote("C:\\path\\x"), std::string("\"C:\\\\path\\\\x\""));
    CHECK_EQ(util::json_quote("caf\xc3\xa9"), std::string("\"caf\xc3\xa9\""));
    CHECK_EQ(util::json_quote("bad\xff"), std::string("\"bad\\ufffd\""));

    util::JsonWriter writer;
    writer.begin_object();
    writer.field("name", "x");
    writer.key("list").string_array({"a", "b"});
    writer.key("empty").string_array({});
    writer.key("nested").begin_object().field("flag", true).end_object();
    writer.end_object();
    CHECK_EQ(writer.str(), std::string(R"({"name":"x","list":["a","b"],"empty":[],"nested":{"flag":true}})"));

    util::JsonWriter pretty(2);
    pretty.begin_array().begin_object().field("k", "v").end_object().end_array();
    CHECK_EQ(pretty.str(), std::string("[\n  {\n    \"k\": \"v\"\n  }\n]"));
}

static void test_gcc_depfile() {
    const auto deps = executor::parse_gcc_depfile(
        "/b/obj/main.cpp.o: /src/main.cpp /src/inc/a.hpp \\\n"
        "  /src/inc/with\\ space.hpp /src/inc/dollar$$.hpp\n");
    CHECK_EQ(deps.size(), std::size_t{4});
    if (deps.size() == 4) {
        CHECK_EQ(deps[0], std::string("/src/main.cpp"));
        CHECK_EQ(deps[1], std::string("/src/inc/a.hpp"));
        CHECK_EQ(deps[2], std::string("/src/inc/with space.hpp"));
        CHECK_EQ(deps[3], std::string("/src/inc/dollar$.hpp"));
    }

    const auto windows = executor::parse_gcc_depfile(
        "C:/b/obj/main.cpp.o: C:/src/main.cpp \\\r\n C:/src/a.hpp\r\n");
    CHECK_EQ(windows.size(), std::size_t{2});
    if (windows.size() == 2) {
        CHECK_EQ(windows[0], std::string("C:/src/main.cpp"));
        CHECK_EQ(windows[1], std::string("C:/src/a.hpp"));
    }
}

static void test_msvc_scan() {
    const std::string output =
        "main.cpp\r\n"
        "Note: including file: C:\\proj\\include\\hello.hpp\r\n"
        "Note: including file:  C:\\Program Files\\Microsoft Visual Studio\\VC\\include\\string\r\n"
        "C:\\proj\\src\\main.cpp(3): error C2065: 'x': undeclared identifier\r\n";
    const auto scan = executor::scan_msvc_output(output, "main.cpp");
    CHECK_EQ(scan.includes.size(), std::size_t{1});
    if (!scan.includes.empty()) {
        CHECK_EQ(scan.includes[0], std::string("C:/proj/include/hello.hpp"));
    }
    CHECK_EQ(scan.output, std::string("C:\\proj\\src\\main.cpp(3): error C2065: 'x': undeclared identifier\n"));
}

static void test_paths() {
    CHECK_EQ(fs::join("/root", "src/a.cpp"), std::string("/root/src/a.cpp"));
    CHECK_EQ(fs::join("/root/.", "./src/../include/"), std::string("/root/include"));
    CHECK_EQ(fs::join("/root", "/abs/x"), std::string("/abs/x"));
    CHECK_EQ(fs::join("/root", "C:/abs/x"), std::string("C:/abs/x"));
    CHECK(fs::is_absolute("C:\\x"));
    CHECK(!fs::is_absolute("x/y"));
}

/// Temporary project directory, removed on destruction.
struct TempProject {
    std::filesystem::path root;

    TempProject() {
        root = std::filesystem::temp_directory_path() /
               ("luma_unit_" + std::to_string(std::hash<std::string>{}(std::to_string(
                                   std::chrono::steady_clock::now().time_since_epoch().count()))));
        std::filesystem::create_directories(root);
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void write(const std::string& rel, const std::string& content = "") const {
        const auto path = root / rel;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << content;
    }
    Path path() const { return fs::lexically_normal(root.generic_string()); }
};

static void test_glob() {
    TempProject project;
    project.write("src/main.cpp");
    project.write("src/core/a.cpp");
    project.write("src/core/deep/b.cpp");
    project.write("src/core/deep/b.hpp");
    project.write("other/c.cpp");

    const auto all = fs::glob(project.path(), "src/**/*.cpp");
    CHECK_EQ(all.size(), std::size_t{3});
    CHECK(std::find(all.begin(), all.end(), "src/main.cpp") != all.end());
    CHECK(std::find(all.begin(), all.end(), "src/core/deep/b.cpp") != all.end());

    const auto direct = fs::glob(project.path(), "src/*.cpp");
    CHECK_EQ(direct.size(), std::size_t{1});

    const auto core = fs::glob(project.path(), "src/core/**/*.cpp");
    CHECK_EQ(core.size(), std::size_t{2});

    const auto anywhere = fs::glob(project.path(), "**/*.cpp");
    CHECK_EQ(anywhere.size(), std::size_t{4});

    const auto literal = fs::glob(project.path(), "other/c.cpp");
    CHECK_EQ(literal.size(), std::size_t{1});
}

static const graph::Task* find_by_prefix(const config::BuildPlan& plan, const std::string& id) {
    return plan.graph.find_task(graph::TaskId{id});
}

static void test_build_plan_languages_and_outputs() {
    TempProject project;
    project.write("src/main.cpp");
    project.write("src/legacy.c");
    project.write("lib/shared.cpp");
    project.write("include/x.hpp");
    project.write("src/local.hpp");

    config::ProjectConfig cfg;
    cfg.name = "t";
    cfg.root = project.path();
    cfg.profile = "debug";
    cfg.toolchains["gnu"] = config::ToolchainDef{
        .name = "gnu",
        .style = config::ToolchainStyle::Gnu,
        .cc = "my-cc",
        .cxx = "my-c++",
        .linker = "",
        .archiver = "ar",
        .languages = {},
    };
    cfg.active_toolchain = "gnu";
    cfg.profiles["debug"] = config::ProfileDef{
        .name = "debug",
        .defines = {"DEBUG"},
        .cflags = {"-std=c11"},
        .cxxflags = {"-std=c++20"},
        .linkflags = {},
        .lto = false,
    };

    config::TargetDef shared;
    shared.name = "shared";
    shared.kind = config::TargetKind::SharedLibrary;
    shared.sources = {"lib/shared.cpp"};
    shared.include_dirs = {"include"};
    cfg.targets["shared"] = shared;

    config::TargetDef app;
    app.name = "app";
    app.kind = config::TargetKind::Executable;
    app.source_patterns = {"src/**/*"};
    app.dependencies = {"shared"};
    cfg.targets["app"] = app;

    config::BuildPlan plan;
    const auto err = config::build_plan(cfg, plan);
    CHECK(!err.has_value());
    if (err) {
        std::cerr << err->message << '\n';
        return;
    }

    const graph::Task* c_task = find_by_prefix(plan, "compile:app:src/legacy.c");
    const graph::Task* cxx_task = find_by_prefix(plan, "compile:app:src/main.cpp");
    CHECK(c_task && cxx_task);
    if (c_task && cxx_task) {
        CHECK_EQ(c_task->command.front(), std::string("my-cc"));
        CHECK(std::find(c_task->command.begin(), c_task->command.end(), "-std=c11") != c_task->command.end());
        CHECK(std::find(c_task->command.begin(), c_task->command.end(), "-std=c++20") == c_task->command.end());
        CHECK_EQ(c_task->metadata.at("language"), std::string("c"));
        CHECK_EQ(cxx_task->command.front(), std::string("my-c++"));
        CHECK_EQ(cxx_task->outputs.front(), std::string("build/obj/app/src/main.cpp.o"));
        // Dependency include directories propagate.
        CHECK(std::find(cxx_task->command.begin(), cxx_task->command.end(), "-I" + project.path() + "/include") !=
              cxx_task->command.end());
    }

    // Headers matched by the glob are not compiled.
    CHECK(find_by_prefix(plan, "compile:app:src/local.hpp") == nullptr);

    const graph::Task* link_app = find_by_prefix(plan, "link:app");
    const graph::Task* link_shared = find_by_prefix(plan, "link:shared");
    CHECK(link_app && link_shared);
    if (link_app && link_shared) {
#ifdef _WIN32
        CHECK_EQ(link_app->outputs.front(), std::string("build/app.exe"));
        CHECK_EQ(link_shared->outputs.front(), std::string("build/libshared.dll"));
#elif defined(__APPLE__)
        CHECK_EQ(link_app->outputs.front(), std::string("build/app"));
        CHECK_EQ(link_shared->outputs.front(), std::string("build/libshared.dylib"));
#else
        CHECK_EQ(link_app->outputs.front(), std::string("build/app"));
        CHECK_EQ(link_shared->outputs.front(), std::string("build/libshared.so"));
#endif
        CHECK(std::find(link_shared->command.begin(), link_shared->command.end(), "-shared") !=
              link_shared->command.end());
        CHECK(std::find(link_app->dependencies.begin(), link_app->dependencies.end(),
                        graph::TaskId{"link:shared"}) != link_app->dependencies.end());
    }

    CHECK_EQ(plan.targets.size(), std::size_t{2});
    if (plan.targets.size() == 2) {
        CHECK_EQ(plan.targets[0].name, std::string("app"));
        CHECK_EQ(plan.targets[0].sources.size(), std::size_t{3});
    }

    // Restricting the plan pulls in dependencies.
    config::BuildPlan partial;
    CHECK(!config::build_plan(cfg, partial, {"app"}).has_value());
    CHECK(partial.graph.find_task(graph::TaskId{"link:shared"}) != nullptr);
    CHECK(config::build_plan(cfg, partial, {"nope"}).has_value());
}

static void test_cache_journal() {
    TempProject project;
    const Path dir = project.path() + "/cache";
    const graph::TaskId a{"compile:a"};
    const graph::TaskId b{"compile:b"};
    const graph::TaskId c{"compile:c"};

    {
        cache::CacheStore store(dir);
        store.put(a, cache::TaskCacheEntry{.cache_key = "ka", .input_hashes = {{"a.cpp", "ha"}}, .discovered_hashes = {}});
        store.put(c, cache::TaskCacheEntry{.cache_key = "kc", .input_hashes = {}, .discovered_hashes = {}});
        CHECK(store.save());
    }
    {
        // A build that is killed: a started, b finished, c started and its
        // completion record cut short.
        cache::CacheStore store(dir);
        CHECK(store.load());
        CHECK(store.begin_journal());
        store.invalidate(a);
        store.invalidate(b);
        store.put(b, cache::TaskCacheEntry{.cache_key = "kb", .input_hashes = {}, .discovered_hashes = {{"x.hpp", "hx"}}});
        store.invalidate(c);
    }
    {
        std::ofstream journal(std::filesystem::path(dir) / "journal.txt", std::ios::app | std::ios::binary);
        journal << "[task:compile:c]\ncache_key=kc2\n"; // no "end": incomplete
    }
    {
        cache::CacheStore store(dir);
        CHECK(store.load());
        CHECK(store.get(a) == nullptr);
        CHECK(store.get(c) == nullptr);
        const cache::TaskCacheEntry* entry = store.get(b);
        CHECK(entry != nullptr);
        if (entry) {
            CHECK_EQ(entry->cache_key, std::string("kb"));
            CHECK_EQ(entry->discovered_hashes.at("x.hpp"), std::string("hx"));
        }
        CHECK(store.save());
        CHECK(!std::filesystem::exists(std::filesystem::path(dir) / "journal.txt"));
    }
    {
        cache::CacheStore store(dir);
        CHECK(store.load());
        CHECK(store.get(b) != nullptr);
        CHECK(store.get(a) == nullptr);
    }
}

static void test_timings() {
    const std::vector<executor::TaskTiming> timings = {
        {.id = "link:app", .verb = "link", .description = "build/app", .output = "/r/build/app", .ms = 5, .ok = true},
        {.id = "compile:app:a.cpp", .verb = "compile", .description = "a.cpp", .output = "/r/a.o", .ms = 90, .ok = true},
    };
    CHECK_EQ(cli::timings_json(100, timings),
             std::string(R"({"total_ms":100,"tasks":[{"id":"compile:app:a.cpp","verb":"compile","desc":"a.cpp","ms":90,"output":"/r/a.o","ok":true},)"
                         R"({"id":"link:app","verb":"link","desc":"build/app","ms":5,"output":"/r/build/app","ok":true}]})"));

    TempProject project;
    const Path log = project.path() + "/.luma/timings.jsonl";
    for (int i = 0; i < 55; ++i) {
        CHECK(cli::append_timings_log(log, 1000 + i, "debug", "succeeded", i, timings));
    }
    std::ifstream in(log);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    CHECK_EQ(lines.size(), std::size_t{50});
    if (!lines.empty()) {
        CHECK(lines.front().starts_with(R"({"timestamp":1005,"profile":"debug","status":"succeeded","total_ms":5,"tasks":[)"));
        CHECK(lines.back().starts_with(R"({"timestamp":1054,)"));
    }
}

static void test_public_usage_requirements() {
    TempProject project;
    project.write("base/base.c");
    project.write("mid/mid.cpp");
    project.write("app/main.cpp");

    config::ProjectConfig cfg;
    cfg.root = project.path();
    cfg.profile = "debug";
    cfg.toolchains["gnu"] = config::ToolchainDef{
        .name = "gnu", .style = config::ToolchainStyle::Gnu, .cc = "my-cc", .cxx = "my-c++",
        .linker = "", .archiver = "ar", .languages = {}};
    cfg.active_toolchain = "gnu";
    cfg.profiles["debug"] = config::ProfileDef{
        .name = "debug", .defines = {"DEBUG"}, .cflags = {}, .cxxflags = {}, .linkflags = {}, .lto = false};

    config::TargetDef base;
    base.name = "base";
    base.kind = config::TargetKind::StaticLibrary;
    base.sources = {"base/base.c"};
    base.defines = {"BASE_PRIVATE"};
    base.public_defines = {"BASE_ABI=1", "SHARED_DEF"};
    base.public_include_dirs = {"base/include"};
    base.public_cflags = {"-fsigned-char"};
    base.public_cxxflags = {"-fno-rtti"};
    cfg.targets["base"] = base;

    config::TargetDef mid;
    mid.name = "mid";
    mid.kind = config::TargetKind::StaticLibrary;
    mid.sources = {"mid/mid.cpp"};
    mid.public_defines = {"MID_ABI", "SHARED_DEF"};
    mid.dependencies = {"base"};
    cfg.targets["mid"] = mid;

    config::TargetDef app;
    app.name = "app";
    app.sources = {"app/main.cpp"};
    app.defines = {"APP_ONLY", "DEBUG"};
    app.dependencies = {"mid"};
    cfg.targets["app"] = app;

    config::BuildPlan plan;
    const auto err = config::build_plan(cfg, plan);
    CHECK(!err.has_value());
    if (err || plan.targets.size() != 3) {
        return;
    }
    // plan.targets is sorted: app, base, mid.
    const auto& app_info = plan.targets[0];
    const auto& base_info = plan.targets[1];
    const auto& mid_info = plan.targets[2];

    using Strings = std::vector<std::string>;
    // Own defines, own public defines, then dependencies' public defines in
    // dependency order; duplicates removed; private defines not exported.
    CHECK(app_info.defines == (Strings{"DEBUG", "APP_ONLY", "MID_ABI", "SHARED_DEF", "BASE_ABI=1"}));
    CHECK(mid_info.defines == (Strings{"DEBUG", "MID_ABI", "SHARED_DEF", "BASE_ABI=1"}));
    CHECK(base_info.defines == (Strings{"DEBUG", "BASE_PRIVATE", "BASE_ABI=1", "SHARED_DEF"}));
    CHECK(std::find(app_info.include_dirs.begin(), app_info.include_dirs.end(), "base/include") !=
          app_info.include_dirs.end());

    const graph::Task* app_compile = plan.graph.find_task(graph::TaskId{"compile:app:app/main.cpp"});
    const graph::Task* base_compile = plan.graph.find_task(graph::TaskId{"compile:base:base/base.c"});
    CHECK(app_compile && base_compile);
    if (app_compile && base_compile) {
        const auto& cmd = app_compile->command;
        CHECK(std::find(cmd.begin(), cmd.end(), "-DBASE_ABI=1") != cmd.end());
        CHECK(std::find(cmd.begin(), cmd.end(), "-DBASE_PRIVATE") == cmd.end());
        CHECK(std::find(cmd.begin(), cmd.end(), "-fno-rtti") != cmd.end());
        CHECK(std::find(cmd.begin(), cmd.end(), "-fsigned-char") == cmd.end());
        const auto& base_cmd = base_compile->command;
        CHECK(std::find(base_cmd.begin(), base_cmd.end(), "-fsigned-char") != base_cmd.end());
    }
}

int main() {
    test_sha256();
    test_json();
    test_gcc_depfile();
    test_msvc_scan();
    test_paths();
    test_glob();
    test_build_plan_languages_and_outputs();
    test_cache_journal();
    test_public_usage_requirements();
    test_timings();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all unit tests passed\n";
    return 0;
}
