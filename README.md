# Luma

A modern build graph engine with Lua as its configuration language.

> Build descriptions should be code when necessary, declarations when possible, and always explainable.

## Quick start

Build Luma (CMake is only used to bootstrap Luma itself):

```bash
cmake -B build
cmake --build build --config Release
```

Build an example project:

```bash
cd examples/hello
../../build/Release/luma.exe build
../../build/Release/luma.exe build --profile release
./build/hello.exe
```

## Example `luma.lua`

```lua
project "hello"

target "core" {
    kind = "static_library",
    sources = glob "src/core/**/*.cpp",
    include_dirs = { "include" },
}

target "hello" {
    kind = "executable",
    sources = glob "src/main.cpp",
    include_dirs = { "include" },
    dependencies = { "core" },
}

profile "debug" {
    defines = { "DEBUG" },
}

profile "release" {}
```

## Commands

```bash
luma build [targets...] [--profile debug|release] [--dry-run] [--verbose] [-j N]
luma clean
luma graph --format dot
luma explain <target>
luma inspect <target>
```

## Architecture

```text
Lua configuration  →  Build graph  →  Execution engine
```

1. **Configuration** — `luma.lua` declares toolchains, profiles, targets, and generators
2. **Graph** — declarations become an immutable DAG of compile/link/generate tasks
3. **Execution** — content-hashed incremental builds with cached task fingerprints in `.luma/cache`

## Features

- Lua configuration with `project`, `target`, `toolchain`, `profile`, `glob`, `generate`, `query`, `when_os`
- C/C++ executables and static libraries
- Auto-detected toolchains (clang++, g++, MSVC `cl`)
- Incremental builds with SHA-256 input hashing (Windows) and cache keys
- Parallel execution (`-j N`, defaults to CPU core count) with DAG-aware scheduling
- `explain` shows why a target would rebuild
- `inspect` shows target configuration
- `graph` exports the task DAG as Graphviz DOT

## Project layout

```text
include/luma/     Public headers
src/              Implementation
examples/hello/   Sample project
tests/            Unit tests
```

## License

TBD
