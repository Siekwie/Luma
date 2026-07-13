project("hello")

builddir("build")

target("core", {
    kind = "static_library",
    sources = glob("src/core/**/*.cpp"),
    include_dirs = { "include" },
})

target("hello", {
    kind = "executable",
    sources = glob("src/main.cpp"),
    include_dirs = { "include" },
    dependencies = { "core" },
})

profile("debug", {
    defines = { "DEBUG" },
})

profile("release", {
})
