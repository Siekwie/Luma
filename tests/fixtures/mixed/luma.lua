-- Fixture for the CLI integration test: a C library inside a C++ project.
project("mixed")

target("cmath", {
    kind = "static_library",
    sources = { "src/twice.c" },
    include_dirs = { "include" },
})

target("app", {
    kind = "executable",
    sources = glob("src/**/*.cpp"),
    defines = { "APP_GREETING=\"hello mixed\"" },
    dependencies = { "cmath" },
})
