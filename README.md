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
| `luma build [target...] [--profile P] [-j N] [--dry-run] [--verbose] [--no-compdb] [--timings]` | Build all targets, or the named targets and their dependencies. |
| `luma run <target> [--profile P] [-j N] [--no-compdb] [--timings] [-- args...]` | Build the target, then run its executable. |
| `luma test [target...] [--profile P] [-j N] [--timeout S] [--format text\|json] [-- args...]` | Build and run the test targets (`test = true`, or the named ones). |
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

`inherit = "<profile>"` starts a profile from another one (after that one's own
inheritance) and appends its settings — the usual way to add sanitizer builds:

```lua
profile "asan" {
    inherit = "debug",
    cxxflags = { "-fsanitize=address,undefined", "-fno-omit-frame-pointer" },
    linkflags = { "-fsanitize=address,undefined" },
}
```

Chains are fine; a cycle or an unknown parent is an error.

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

#### Timings

With `--timings` (on `build` and `run`), one more line follows the final
status line (also after `luma: build failed`):

```text
luma: build succeeded (2 run, 2 up to date) in 0.52s
luma: timings {"total_ms":521,"tasks":[{"id":"compile:hello:src/main.cpp","verb":"compile","desc":"src/main.cpp","ms":448,"output":"/abs/root/build/obj/hello/src/main.cpp.o","ok":true},{"id":"link:hello","verb":"link","desc":"build/hello","ms":61,"output":"/abs/root/build/hello","ok":true}]}
```

The JSON is always on that single line. `total_ms` is the wall time of the
whole command up to the status line; `tasks` lists only tasks that actually
ran in this build, longest first. `id` is the task id, `verb`/`desc` are
the words of its progress line, `ms` the wall time of its command,
`output` the absolute path of its (first) output, and `ok` whether it
succeeded. A no-op build prints `"tasks":[]`. Without `--timings` the
output is unchanged. For `luma run` the line is printed before the program
starts.

Every build (not dry runs) also appends a record to `.luma/timings.jsonl`,
whether or not `--timings` was given. Each line is one JSON object:

```json
{"timestamp":1794717093,"profile":"debug","status":"succeeded","total_ms":521,"tasks":[...]}
```

`timestamp` is Unix time in seconds, `status` is `succeeded`, `failed` or
`cancelled`, and `tasks` has the same entries as above. The file keeps the
last 50 builds and is replaced atomically.

#### Cancellation and crash safety

Ctrl+C, `SIGTERM` or `SIGHUP` (Windows: Ctrl+C, Ctrl+Break, closing the
console) during a build stops it in an orderly way: running tasks are
terminated (their whole process group on POSIX), no new tasks start, the
cache is saved, and luma prints `luma: build failed` (plus
`luma: build cancelled` on stderr) and exits with `128 + signal` (130 for
Ctrl+C). A second signal terminates immediately.

The cache stays consistent even if luma is killed outright (`SIGKILL`,
`TerminateProcess`): the manifest is only replaced atomically, and while a
build runs every task start and completion is appended to
`.luma/cache/journal.txt`. The next build replays the journal, so tasks
that were interrupted, whose outputs may be half-written, run again, and
tasks that finished are not repeated. On Windows all child processes live
in a job object that is killed together with luma; on Linux children
receive `SIGTERM` when luma dies.

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

### `luma test`

Mark test executables with `test = true`:

```lua
target "math_test" {
    sources = { "tests/math_test.cpp" },
    dependencies = { "math" },
    test = true,
}
```

`luma test` builds the test targets (and only what they need), runs them in
parallel from the project root (`-j N`), and prints one line per test.
A test passes when it exits with 0. The output of failing tests is printed
unchanged, so `file:line` references in it stay clickable in editors:

```text
[1/3] FAIL bad_test (0.01s, exit code 1)
tests/bad_test.cpp:3: expected 4, got 5
[2/3] PASS math_test (0.01s)
[3/3] FAIL hang_test (300.00s, timed out)
luma: tests: 1 passed, 2 failed in 300.01s (bad_test, hang_test)
```

`--timeout S` kills a test after S seconds (default 300, 0 = no limit).
`--verbose` also prints the output of passing tests. Arguments after `--` go
to every test. `--format json` prints one `luma: test {...}` line per test
(`name`, `status` = passed/failed/timeout/error, `exit_code`, `ms`, `output`)
and a final `luma: tests {"passed":N,"failed":M,"ms":T}`. The exit code is
1 when any test failed.

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
      "dependencies": ["core"],
      "test": false
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
| `include_dirs` | Effective include directories: the target's own (`include_dirs`, `public_include_dirs`), then those of its (transitive) dependencies. |
| `defines` | Effective defines, exactly as passed to the compiler: profile, target `defines`, target `public_defines`, then the `public_defines` of its transitive dependencies in dependency order; duplicates removed. |
| `dependencies` | Direct dependencies as declared. |
| `test` | `true` for targets that `luma test` runs. |

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
`cxxflags`, `linkflags`, `dependencies`, `system_libs`, and the usage
requirements `public_defines`, `public_include_dirs`, `public_cflags`,
`public_cxxflags`. Lists may also be a single string.

### Usage requirements (exported settings)

Some settings must be shared by a library and everything that uses its
headers, or the ABI silently mismatches. Declare them as `public_*`:

```lua
target "imgui" {
    kind = "static_library",
    sources = glob "third_party/imgui/*.cpp",
    include_dirs = { "third_party/imgui" },
    public_defines = { "IMGUI_USE_WCHAR32", "IMGUI_DISABLE_OBSOLETE_FUNCTIONS" },
}

target "app" {
    kind = "executable",
    sources = glob "src/**/*.cpp",
    dependencies = { "imgui" },   -- compiled with -DIMGUI_USE_WCHAR32 too
}
```

| Field | Applies to the target | Exported to dependents |
| --- | --- | --- |
| `defines`, `cflags`, `cxxflags` | yes | no |
| `public_defines`, `public_cflags`, `public_cxxflags` | yes | yes, transitively |
| `include_dirs`, `public_include_dirs` | yes | yes, transitively |

Exported settings flow through every dependency edge, including through
static and shared libraries (`app → ui → imgui` gives `app` imgui's
public defines). Order: the profile's, the target's own, its own public
ones, then those of its dependencies in dependency order (dependents before
the libraries they use). Defines and include directories are
deduplicated, keeping the first occurrence; flags keep their order and are
not deduplicated (so pairs like `-include x.h` survive), with
dependencies' public flags last. `public_cflags` apply to C sources and
`public_cxxflags` to C++ sources. `luma targets --format json`, `luma
compdb` and the build all use these effective values.

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
