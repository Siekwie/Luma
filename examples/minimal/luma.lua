project("example")

target("app", {
    kind = "executable",
    sources = glob("src/**/*.cpp"),
    include_dirs = { "include" },
    defines = { "APP_VERSION=1" },
    cxxflags = { "-Wall", "-Wextra", "-std=c++20" },
    dependencies = { "core" },
})

target("core", {
    kind = "static_library",
    sources = glob("src/core/**/*.cpp"),
    include_dirs = { "include" },
})

profile("debug", {
    defines = { "DEBUG" },
    cxxflags = { "-O0", "-g" },
})

profile("release", {
    cxxflags = { "-O3" },
})

generate("version", {
    inputs = { ".git/HEAD" },
    outputs = { "generated/version.hpp" },
    command = {
        "my-version-generator",
        "--output",
        "$output",
    },
})
