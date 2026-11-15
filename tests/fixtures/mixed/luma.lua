-- Fixture for the CLI integration test: a C library inside a C++ project.
project("mixed")

target("cmath", {
    kind = "static_library",
    sources = { "src/twice.c" },
    include_dirs = { "include" },
    -- Must be seen by every user of twice.h (checked by #error in both files).
    public_defines = { "TWICE_ABI=2" },
})

target("app", {
    kind = "executable",
    sources = glob("src/**/*.cpp"),
    defines = { "APP_GREETING=\"hello mixed\"" },
    dependencies = { "cmath" },
})
