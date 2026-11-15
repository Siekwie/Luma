# Luma

A modern build graph engine with Lua as its configuration language.

> Build descriptions should be code when necessary, declarations when possible, and always explainable.

Luma builds C and C++ projects on Windows (MSVC, MinGW, clang) and POSIX
(GCC, clang). Its command line is also designed to be driven by IDEs:
machine-readable project descriptions, a compilation database, and
parseable build output.

## Quick start

Build Luma (CMake is only used to bootstrap Luma itself):

```bash
cmake -B build -G Ninja          # or any generator, e.g. Visual Studio
cmake --build build --config Release
ctest --test-dir build -C Release   # optional: unit + end-to-end tests
```

The executable is `build/luma` (single-config generators) or
`build/Release/luma.exe` (Visual Studio).

Build and run the example project:

```bash
cd examples/hello
../../build/luma build
../../build/luma run hello
../../build/luma build --profile release
```

Start a new project:

```bash
mkdir myapp && cd myapp
luma init            # luma.lua, src/main.cpp, include/
luma run myapp
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

Both `target "x" { ... }` and `target("x", { ... })` work.

## Commands

| Command | Description |
| --- | --- |
| `luma build [target...] [--profile P] [-j N] [--dry-run] [--verbose] [--no-compdb]` | Build all targets, or the named targets and their dependencies. |
| `luma run <target> [--profile P] [-j N] [--no-compdb] [-- args...]` | Build the target, then run its executable. |
| `luma targets [--format table\|json] [--profile P]` | Describe the project and its targets. |
| `luma compdb [--profile P] [--output PATH]` | Write a clang `compile_commands.json`. |
| `luma init [name]` | Create a new project in the current directory. |
| `luma clean` | Delete the build directory and the `.luma` cache. |
| `luma graph [--format dot]` | Export the task graph as Graphviz DOT. |
| `luma explain <target>` | Explain why a target would rebuild. |
| `luma inspect <target>` | Show a target's configuration. |
| `luma --version`, `luma --help` | |

Luma looks for `luma.lua` in the current directory and its parents; the
directory containing it is the project root. Options accept both
`--opt value` and `--opt=value`; `-j8` and `--jobs 8` also work.
Unknown options are errors. All commands exit with a nonzero code on
failure and print errors to stderr as `luma: error: <message>`.

### Profiles

`--profile P` selects a profile declared with `profile()`. Without it, Luma
uses the profile chosen by `set_profile()` in `luma.lua`, else `debug`,
else the first profile by name. An unknown `--profile` is an error that
lists the available profiles. If `luma.lua` declares no profiles, Luma
provides `debug` and `release` with flags for the detected toolchain
(`-O0 -g -Wall -Wextra` / `-O3 ...` plus `-std=c++20` for C++; MSVC:
`/Od /Z7 /MDd /W4` / `/O2 /MD /W4` plus `/std:c++20`).

A profile you declare under one of those names starts from the built-in
defaults and adds its own defines and flags after them (later flags win, so
`cxxflags = { "-O2" }` in `debug` overrides `-O0`). Write `inherit = false`
to start from nothing. A project that declares only profiles with other names
(e.g. `dev`) gets exactly those.

All profiles share the build directory, so switching profiles rebuilds what
changed.

### `luma build`

Each task that runs prints exactly one progress line to stdout, followed
by everything the command wrote to stdout and stderr, verbatim:

```text
[1/4] compile src/core/greeting.cpp
[2/4] archive build/libcore.a
[3/4] compile src/main.cpp
/abs/path/src/main.cpp:5:3: error: use of undeclared identifier 'x'
[4/4] link build/hello
luma: build succeeded (4 run, 0 up to date) in 0.41s
```

- Format: `[i/N] <verb> <description>`. `N` is the number of tasks that
  need to run in this build (up-to-date tasks are not counted or printed);
  `i` counts completed tasks.
- Verbs: `compile` (description: source path relative to the root),
  `link` for executables and shared libraries, `archive` for static
  libraries (description: output path relative to the root), `generate`
  for `generate()` outputs.
- A task's progress line and its output are written together after the
  task finishes and flushed immediately, so output from parallel tasks
  never interleaves. Compiler stdout and stderr are merged in their
  original order and line endings are normalized to `\n`. Diagnostics keep
  the compiler's native format (`file:line:col: error: msg` for GCC/clang,
  `file(line): error C1234: msg` for MSVC); compile commands use absolute
  paths, so file names in diagnostics are absolute.
- With MSVC, the `/showIncludes` notes (used for header tracking) and the
  echoed source file name are removed from the output.
- The last line is `luma: build succeeded (X run, Y up to date) in 1.23s`,
  or `luma: build failed` with exit code 1. Configuration errors also end
  with `luma: build failed`. Details of a failed task (task id, exit code,
  command line) go to stderr.
- After a failure no new tasks start; tasks already running finish and are
  reported.
- `--verbose` prints each command line right after its progress line;
  `--dry-run` prints the progress lines and commands without running
  anything and ends with `luma: dry run (X would run, Y up to date)`.
- `-j N` sets the number of parallel jobs (default: number of hardware
  threads).

Before building, `luma build` refreshes `<root>/compile_commands.json`
(always covering all targets, even when only some are built) if its
fingerprint changed: the fingerprint hashes the toolchain identity and the
full database content, and is stored in `.luma/cache/compdb.stamp`. When it
is unchanged and the file exists, nothing is written. `--no-compdb`
disables this.

#### Incremental builds

A task runs when an output is missing, its command line, toolchain or any
input content changed (SHA-256), a header it included changed (tracked
through `-MMD` depfiles or MSVC `/showIncludes`), or a task it depends on
runs. Fingerprints are stored in `.luma/cache/manifest.txt`. For localized
MSVC toolsets set `LUMA_MSVC_DEPS_PREFIX` to the translated
`Note: including file:` prefix.

### `luma run`

```bash
luma run hello --profile release -- --some-flag "an argument"
```

Builds the target (and its dependencies) exactly like `luma build`,
including its progress output on stdout, then runs the executable with the
arguments after `--`, in the current working directory, inheriting
stdin/stdout/stderr. Luma exits with the program's exit code (on POSIX,
`128 + N` if it was killed by signal N). If the build fails, the program is
not run and the exit code is 1. The target must be an executable.

### `luma targets`

Without `--format json`, prints a human-readable summary and table.
With `--format json`, prints a single JSON object (one line) to stdout:

```json
{
  "project": "hello",
  "root": "/abs/root",
  "build_dir": "/abs/root/build",
  "active_profile": "debug",
  "profiles": ["debug", "release"],
  "toolchain": "clang++ Ubuntu clang version 18.1.3 (1ubuntu1)",
  "targets": [
    {
      "name": "hello",
      "kind": "executable",
      "language": "cxx",
      "output": "/abs/root/build/hello",
      "sources": ["/abs/root/src/main.cpp"],
      "include_dirs": ["/abs/root/include"],
      "defines": ["DEBUG"],
      "dependencies": ["core"]
    }
  ]
}
```

| Field | Meaning |
| --- | --- |
| `project` | `project()` name (the root directory's name if unset). |
| `root` | Directory containing `luma.lua`. |
| `build_dir` | Absolute build directory (`builddir()`, default `build`). |
| `active_profile` | Profile the description was computed for. |
| `profiles` | All profile names, sorted. |
| `toolchain` | Compiler command followed by the first line of its version banner. |
| `targets` | Sorted by name. |
| `kind` | `executable`, `static_library` or `shared_library`. |
| `language` | Declared target language, `cxx` (default) or `c`. Individual sources are compiled by extension. |
| `output` | Artifact path (`.exe` / `.dll` / `.lib` on Windows; no suffix / `.so` (`.dylib` on macOS) / `.a` elsewhere; GNU-style libraries get a `lib` prefix). |
| `sources` | All files matched by `sources`, including non-compiled ones such as headers. |
| `include_dirs` | Effective include directories: the target's own, then those of its (transitive) dependencies. |
| `defines` | Effective defines: profile, then target. |
| `dependencies` | Direct dependencies as declared. |

All paths are absolute and use forward slashes. Strings use standard JSON
escaping (invalid UTF-8 is replaced with U+FFFD).

### `luma compdb`

Writes a [JSON compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html)
to `<root>/compile_commands.json` (or `--output PATH`, relative to the
current directory) for the selected profile, then prints the absolute path
of the written file. Each entry is
`{"directory", "file", "arguments", "output"}` with the exact argument
vector the build runs for that compile task; entries are sorted.

### `luma init [name]`

Creates `luma.lua` (one executable target named after the project,
compiling `src/**/*.cpp` with `include/` on the include path),
`src/main.cpp` and `include/` in the current directory, plus a `.gitignore`
if none exists. The name defaults to the directory name and may contain
letters, digits, `_`, `-` and `.`. Refuses to run (exit code 1, nothing
written) if `luma.lua` or `src/main.cpp` already exists.

## Configuration reference

| Function | Purpose |
| --- | --- |
| `project(name)` | Project name. |
| `target(name, { ... })` | Declare a target (fields below). |
| `profile(name, { defines, cflags, cxxflags, linkflags, lto })` | Declare a build profile. |
| `toolchain(name, { cc, cxx, linker, archiver, kind })` | Declare a toolchain (`kind = "msvc"` or `cxx = "cl"` selects MSVC-style commands). The first declared toolchain is active. |
| `set_toolchain(name)`, `set_profile(name)` | Select the active toolchain / default profile. |
| `builddir(path)` | Build directory (default `build`). |
| `glob(pattern)` | Source pattern: `*`, `?`, and `**/` for any number of directories. |
| `generate(name, { inputs, outputs, command })` | Run a command producing files (`$output`, `$inputs`, `$root` are substituted); all compile tasks wait for generators. |
| `when_os(name, fn)` | Run `fn` on `windows`, `linux` or `macos`. |
| `query(cmd, args...)` | Run a command at configuration time and return its output. |

Target fields: `kind` (`executable`, `static_library`, `shared_library`),
`language` (`cxx` or `c`), `sources`, `include_dirs`, `defines`, `cflags`,
`cxxflags`, `linkflags`, `dependencies`, `system_libs`. Lists may also be a
single string.

Without a `toolchain()` declaration Luma picks the first available of
`clang++`/`clang`, `g++`/`gcc` and MSVC `cl`. Sources ending in `.c` are
compiled with the C compiler and C flags (`cflags`); `.cpp`, `.cc`,
`.cxx` and `.c++` with the C++ compiler and `cxxflags`. Other files listed
in `sources` (headers) are not compiled. A target links with the C++
driver if it has C++ sources (or with the toolchain's `linker`).
Dependencies' libraries are linked transitively; shared libraries are
found at run time through an rpath to the build directory on Linux.

## Architecture

```text
Lua configuration  →  Build graph  →  Execution engine
```

1. **Configuration** — `luma.lua` declares toolchains, profiles, targets, and generators
2. **Graph** — declarations become an immutable DAG of compile/link/generate tasks
3. **Execution** — content-hashed (SHA-256) incremental builds with cached task fingerprints in `.luma/cache`

## Project layout

```text
include/luma/     Public headers
src/              Implementation
examples/hello/   Sample project
tests/            Unit tests and the end-to-end CLI test (tests/integration)
```

## License

TBD
