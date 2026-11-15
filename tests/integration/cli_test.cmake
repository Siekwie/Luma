# End-to-end test of the luma command line, run with `cmake -P`.
#
# Required variables:
#   LUMA        path to the luma executable
#   SOURCE_DIR  repository root
#   WORK_DIR    scratch directory (wiped first)

cmake_minimum_required(VERSION 3.20)

foreach(var LUMA SOURCE_DIR WORK_DIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} must be defined")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(COPY "${SOURCE_DIR}/examples/hello" DESTINATION "${WORK_DIR}")
file(COPY "${SOURCE_DIR}/tests/fixtures/mixed" DESTINATION "${WORK_DIR}")
# Never reuse outputs that may exist in the source tree.
file(REMOVE_RECURSE "${WORK_DIR}/hello/build" "${WORK_DIR}/hello/.luma" "${WORK_DIR}/hello/compile_commands.json")

set(failures 0)

macro(fail message)
    message(SEND_ERROR "FAIL: ${message}")
    math(EXPR failures "${failures} + 1")
endmacro()

# run_luma(<dir> <out-var> <code-var> args...)
function(run_luma dir out_var code_var)
    execute_process(
        COMMAND "${LUMA}" ${ARGN}
        WORKING_DIRECTORY "${dir}"
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        RESULT_VARIABLE code)
    set(${out_var} "${out}" PARENT_SCOPE)
    set(${code_var} "${code}" PARENT_SCOPE)
    message(STATUS "luma ${ARGN} (in ${dir}) -> ${code}\n${out}${err}")
endfunction()

set(hello "${WORK_DIR}/hello")
set(mixed "${WORK_DIR}/mixed")

# --- build: progress lines and summary --------------------------------------
run_luma("${hello}" out code build)
if(NOT (code EQUAL 0))
    fail("hello build succeeds")
endif()
if(NOT (out MATCHES "\\[[0-9]+/4\\] compile src/main.cpp\n"))
    fail("compile progress line")
endif()
if(NOT (out MATCHES "\\[4/4\\] link build/hello"))
    fail("link is the last of 4 tasks")
endif()
if(NOT (out MATCHES "luma: build succeeded \\(4 run, 0 up to date\\) in [0-9]+\\.[0-9][0-9]s\n$"))
    fail("build summary line")
endif()
if(NOT (EXISTS "${hello}/compile_commands.json"))
    fail("build writes compile_commands.json")
endif()

string(REPLACE "\n" ";" lines "${out}")
foreach(line IN LISTS lines)
    if(NOT line STREQUAL "" AND NOT line MATCHES "^\\[[0-9]+/[0-9]+\\] [a-z]+ " AND NOT line MATCHES "^luma: build")
        if(NOT (FALSE))
            fail("unexpected output line: ${line}")
        endif()
    endif()
endforeach()

run_luma("${hello}" out code build)
if(NOT (out MATCHES "luma: build succeeded \\(0 run, 4 up to date\\)"))
    fail("second build is a no-op")
endif()

# Editing a header rebuilds its dependents.
file(APPEND "${hello}/include/hello.hpp" "// touched by test\n")
run_luma("${hello}" out code build)
if(NOT (out MATCHES "compile src/main.cpp"))
    fail("header change rebuilds main.cpp")
endif()
if(NOT (out MATCHES "link build/hello"))
    fail("header change relinks")
endif()

# --- run -----------------------------------------------------------------------
run_luma("${hello}" out code run hello)
if(NOT (code EQUAL 0))
    fail("luma run hello exits 0")
endif()
if(NOT (out MATCHES "Hello from Luma"))
    fail("luma run hello prints greeting")
endif()

# --- targets --format json --------------------------------------------------
run_luma("${hello}" json code targets --format json)
if(NOT (code EQUAL 0))
    fail("targets json succeeds")
endif()
string(JSON project ERROR_VARIABLE json_error GET "${json}" project)
if(NOT (json_error STREQUAL "NOTFOUND"))
    fail("targets output is valid JSON (${json_error})")
endif()
if(NOT (project STREQUAL "hello"))
    fail("project name")
endif()
string(JSON active ERROR_VARIABLE json_error GET "${json}" active_profile)
if(NOT (active STREQUAL "debug"))
    fail("default profile is debug")
endif()
string(JSON target_count ERROR_VARIABLE json_error LENGTH "${json}" targets)
if(NOT (target_count EQUAL 2))
    fail("two targets")
endif()
string(JSON name ERROR_VARIABLE json_error GET "${json}" targets 1 name)
string(JSON kind ERROR_VARIABLE json_error GET "${json}" targets 1 kind)
string(JSON output ERROR_VARIABLE json_error GET "${json}" targets 1 output)
string(JSON dep ERROR_VARIABLE json_error GET "${json}" targets 1 dependencies 0)
if(NOT (name STREQUAL "hello" AND kind STREQUAL "executable" AND dep STREQUAL "core"))
    fail("hello target fields")
endif()
if(NOT (IS_ABSOLUTE "${output}"))
    fail("output path is absolute")
endif()
if(CMAKE_HOST_WIN32)
    if(NOT (output MATCHES "hello\\.exe$"))
        fail("executable has .exe on Windows")
    endif()
else()
    if(NOT (output MATCHES "/build/hello$"))
        fail("executable has no suffix outside Windows")
    endif()
endif()
if(NOT (EXISTS "${output}"))
    fail("reported output exists")
endif()

run_luma("${hello}" json code targets --format json --profile release)
string(JSON active ERROR_VARIABLE json_error GET "${json}" active_profile)
if(NOT (active STREQUAL "release"))
    fail("--profile release is reported")
endif()

run_luma("${hello}" out code targets --profile does-not-exist)
if(NOT (NOT code EQUAL 0))
    fail("unknown profile is an error")
endif()

run_luma("${hello}" out code targets)
if(NOT (out MATCHES "TARGET +KIND"))
    fail("targets prints a table")
endif()

# --- compdb ------------------------------------------------------------------
run_luma("${hello}" out code compdb --output "${WORK_DIR}/cc.json")
if(NOT (code EQUAL 0))
    fail("compdb succeeds")
endif()
if(NOT (out MATCHES "cc\\.json"))
    fail("compdb prints the written path")
endif()
file(READ "${WORK_DIR}/cc.json" compdb)
string(JSON entries ERROR_VARIABLE json_error LENGTH "${compdb}")
if(NOT (entries EQUAL 2))
    fail("compdb has one entry per compile task (${json_error})")
endif()
string(JSON file ERROR_VARIABLE json_error GET "${compdb}" 0 file)
string(JSON directory ERROR_VARIABLE json_error GET "${compdb}" 0 directory)
string(JSON arg0 ERROR_VARIABLE json_error GET "${compdb}" 0 arguments 0)
if(NOT (IS_ABSOLUTE "${file}" AND IS_ABSOLUTE "${directory}" AND NOT arg0 STREQUAL ""))
    fail("compdb entry fields")
endif()

# --- C sources, run arguments and exit codes ------------------------------------
run_luma("${mixed}" out code run app -- 7 "two words")
if(NOT (code EQUAL 7))
    fail("luma run forwards the exit code (got ${code})")
endif()
if(NOT (out MATCHES "compile src/twice.c"))
    fail("C source compiled")
endif()
if(NOT (out MATCHES "hello mixed 42"))
    fail("program output")
endif()
if(NOT (out MATCHES "arg:7\narg:two words\n"))
    fail("arguments passed verbatim")
endif()

file(READ "${mixed}/compile_commands.json" compdb)
string(JSON e0 GET "${compdb}" 0 file)
string(JSON c0 GET "${compdb}" 0 arguments 0)
string(JSON c1 GET "${compdb}" 1 arguments 0)
if(e0 MATCHES "twice\\.c$")
    set(c_compiler "${c0}")
    set(cxx_compiler "${c1}")
else()
    set(c_compiler "${c1}")
    set(cxx_compiler "${c0}")
endif()
if(NOT cxx_compiler STREQUAL "cl")
    if(NOT (NOT c_compiler STREQUAL cxx_compiler))
        fail("C file uses the C compiler (${c_compiler} vs ${cxx_compiler})")
    endif()
endif()

# --- public_defines propagate to dependents ------------------------------------
run_luma("${mixed}" json code targets --format json)
string(JSON t0 GET "${json}" targets 0 name)
if(t0 STREQUAL "app")
    set(app_index 0)
else()
    set(app_index 1)
endif()
string(JSON app_defines GET "${json}" targets ${app_index} defines)
if(NOT (app_defines MATCHES "TWICE_ABI=2" AND app_defines MATCHES "APP_GREETING"))
    fail("app's effective defines include cmath's public define (${app_defines})")
endif()
file(READ "${mixed}/compile_commands.json" compdb)
if(NOT (compdb MATCHES "-DTWICE_ABI=2[^}]*main\\.cpp"))
    fail("compdb carries the public define for dependents")
endif()

# --- failing build -------------------------------------------------------------
file(WRITE "${mixed}/src/broken.cpp" "int broken() { return undeclared_symbol; }\n")
run_luma("${mixed}" out code build)
if(NOT (NOT code EQUAL 0))
    fail("broken build fails")
endif()
if(NOT (out MATCHES "compile src/broken.cpp\n[^\n]*broken.cpp[:(][0-9]+"))
    fail("diagnostics follow the progress line")
endif()
if(NOT (out MATCHES "luma: build failed\n$"))
    fail("failure summary line")
endif()
file(REMOVE "${mixed}/src/broken.cpp")
run_luma("${mixed}" out code build)
if(NOT (code EQUAL 0))
    fail("build recovers after fixing the error")
endif()

# --- init -----------------------------------------------------------------------
set(fresh "${WORK_DIR}/fresh")
file(MAKE_DIRECTORY "${fresh}")
run_luma("${fresh}" out code init demo)
if(NOT (code EQUAL 0))
    fail("init succeeds")
endif()
if(NOT (EXISTS "${fresh}/luma.lua" AND EXISTS "${fresh}/src/main.cpp" AND IS_DIRECTORY "${fresh}/include"))
    fail("init creates the scaffold")
endif()
run_luma("${fresh}" out code init demo)
if(NOT (NOT code EQUAL 0))
    fail("init refuses to overwrite")
endif()
run_luma("${fresh}" out code run demo)
if(NOT (code EQUAL 0 AND out MATCHES "Hello from demo"))
    fail("scaffolded project builds and runs")
endif()

# --- --timings and .luma/timings.jsonl ---------------------------------------
file(APPEND "${hello}/src/main.cpp" "// timings test\n")
run_luma("${hello}" out code build --timings)
if(NOT (code EQUAL 0))
    fail("build --timings succeeds")
endif()
if(NOT (out MATCHES "luma: build succeeded [^\n]*\nluma: timings {[^\n]*}\n$"))
    fail("timings line follows the status line")
endif()
string(REGEX REPLACE ".*\nluma: timings ([^\n]*)\n$" "\\1" timings "${out}")
string(JSON task_count ERROR_VARIABLE json_error LENGTH "${timings}" tasks)
if(NOT (task_count EQUAL 2))
    fail("timings list the 2 tasks that ran (${json_error})")
endif()
string(JSON total ERROR_VARIABLE json_error GET "${timings}" total_ms)
string(JSON verb ERROR_VARIABLE json_error GET "${timings}" tasks 0 verb)
string(JSON ms0 ERROR_VARIABLE json_error GET "${timings}" tasks 0 ms)
string(JSON ms1 ERROR_VARIABLE json_error GET "${timings}" tasks 1 ms)
string(JSON toutput ERROR_VARIABLE json_error GET "${timings}" tasks 0 output)
if(NOT (total GREATER_EQUAL 0 AND ms0 GREATER_EQUAL ms1 AND IS_ABSOLUTE "${toutput}" AND verb MATCHES "^(compile|link)$"))
    fail("timings fields and ordering (${timings})")
endif()

run_luma("${hello}" out code build)
if(out MATCHES "luma: timings")
    fail("no timings line without --timings")
endif()

file(STRINGS "${hello}/.luma/timings.jsonl" history)
list(LENGTH history history_length)
list(GET history -1 last_record)
string(JSON last_status ERROR_VARIABLE json_error GET "${last_record}" status)
string(JSON last_profile ERROR_VARIABLE json_error GET "${last_record}" profile)
string(JSON last_stamp ERROR_VARIABLE json_error GET "${last_record}" timestamp)
if(NOT (history_length GREATER_EQUAL 5 AND last_status STREQUAL "succeeded" AND last_profile STREQUAL "debug" AND last_stamp GREATER 1600000000))
    fail("timings.jsonl records builds (${history_length}: ${last_record})")
endif()
foreach(i RANGE 1 50)
    file(APPEND "${hello}/.luma/timings.jsonl" "${last_record}\n")
endforeach()
run_luma("${hello}" out code build)
file(STRINGS "${hello}/.luma/timings.jsonl" history)
list(LENGTH history history_length)
if(NOT (history_length EQUAL 50))
    fail("timings.jsonl keeps the last 50 builds (has ${history_length})")
endif()

# --- killing a build mid-way must not break the next incremental build --------
# The generator writes a broken header first and the real one after a delay,
# like a compiler that is interrupted while writing its output.
set(killed "${WORK_DIR}/killed")
file(MAKE_DIRECTORY "${killed}/src")
file(WRITE "${killed}/slowgen.cmake" [=[
file(WRITE "${OUT}" "#error partial output from an interrupted generator\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 3)
file(READ "${IN}" value)
string(STRIP "${value}" value)
file(WRITE "${OUT}" "#define VALUE ${value}\n")
]=])
file(WRITE "${killed}/luma.lua" "
project('killed')
generate('slow', {
    inputs = { 'input.txt' },
    outputs = { 'build/gen/value.hpp' },
    command = { [[${CMAKE_COMMAND}]], '-DOUT=$output', '-DIN=$root/input.txt', '-P', '$root/slowgen.cmake' },
})
target('app', { kind = 'executable', sources = glob('src/*.cpp'), include_dirs = { 'build/gen' } })
")
file(WRITE "${killed}/src/main.cpp" "#include \"value.hpp\"\n#include <cstdio>\nint main() { std::printf(\"value %d\\n\", VALUE); return 0; }\n")
file(WRITE "${killed}/input.txt" "1\n")
run_luma("${killed}" out code run app)
if(NOT (code EQUAL 0 AND out MATCHES "value 1"))
    fail("generator project builds")
endif()

file(WRITE "${killed}/input.txt" "2\n")
execute_process(
    COMMAND "${LUMA}" build
    WORKING_DIRECTORY "${killed}"
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE code
    TIMEOUT 1.5)
message(STATUS "killed build -> ${code}\n${out}${err}")
if(code EQUAL 0)
    fail("the build was expected to be killed by the timeout")
endif()
# Restore the input the cache last saw: only the journal tells luma that the
# generator's output is now half-written.
file(WRITE "${killed}/input.txt" "1\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 2) # let a stray child finish
run_luma("${killed}" out code run app)
if(NOT (code EQUAL 0 AND out MATCHES "value 1"))
    fail("build after a killed build recovers")
endif()

# --- luma test: builds and runs test = true targets, reports, times out
set(tproj "${WORK_DIR}/tproj")
file(MAKE_DIRECTORY "${tproj}/tests")
file(WRITE "${tproj}/luma.lua" [=[
project "tproj"
target "lib" { kind = "static_library", sources = { "lib.cpp" } }
target "good_test" { sources = { "tests/good.cpp" }, dependencies = { "lib" }, test = true }
target "bad_test" { sources = { "tests/bad.cpp" }, test = true }
target "slow_test" { sources = { "tests/slow.cpp" }, test = true }
target "app" { sources = { "main.cpp" } }
]=])
file(WRITE "${tproj}/lib.cpp" "int add(int a, int b) { return a + b; }\n")
file(WRITE "${tproj}/main.cpp" "int main() { return 0; }\n")
file(WRITE "${tproj}/tests/good.cpp" "int add(int, int);\nint main() { return add(2, 2) == 4 ? 0 : 1; }\n")
file(WRITE "${tproj}/tests/bad.cpp" "#include <cstdio>\nint main() { std::printf(\"tests/bad.cpp:2: boom\\n\"); return 3; }\n")
file(WRITE "${tproj}/tests/slow.cpp" "#include <chrono>\n#include <thread>\nint main() { std::this_thread::sleep_for(std::chrono::seconds(20)); }\n")
run_luma("${tproj}" out code test --timeout 1)
if(NOT (code EQUAL 1 AND out MATCHES "PASS good_test" AND out MATCHES "FAIL bad_test \\([0-9.]+s, exit code 3\\)\ntests/bad.cpp:2: boom"
        AND out MATCHES "FAIL slow_test \\([0-9.]+s, timed out\\)" AND out MATCHES "luma: tests: 1 passed, 2 failed"))
    fail("luma test runs test targets and reports failures and timeouts")
endif()
if(EXISTS "${tproj}/build/app" OR EXISTS "${tproj}/build/app.exe")
    fail("luma test builds only the test targets")
endif()
run_luma("${tproj}" out code test good_test --format json)
if(NOT (code EQUAL 0 AND out MATCHES "luma: test {\"name\":\"good_test\",\"status\":\"passed\",\"exit_code\":0," AND out MATCHES "luma: tests {\"passed\":1,\"failed\":0,"))
    fail("luma test --format json")
endif()
run_luma("${tproj}" json code targets --format json)
string(JSON t0 ERROR_VARIABLE json_error GET "${json}" targets 0 test)
if(NOT (t0 STREQUAL "OFF" OR t0 STREQUAL "false"))
    fail("targets json reports test = false for app (got ${t0})")
endif()

# --- profiles: inherit = "<profile>" (chains), unknown parent and cycles are errors
set(prof "${WORK_DIR}/prof")
file(MAKE_DIRECTORY "${prof}/src")
file(WRITE "${prof}/src/main.cpp" "int main() { return 0; }\n")
file(WRITE "${prof}/luma.lua" [=[
project "prof"
target "app" { kind = "executable", sources = { "src/main.cpp" } }
profile "debug" { defines = { "MYDEBUG" } }
profile "asan" { inherit = "debug", cxxflags = { "-fsanitize=address" }, linkflags = { "-fsanitize=address" } }
profile "asan2" { inherit = "asan", defines = { "EXTRA" } }
]=])
run_luma("${prof}" out code compdb --profile asan2)
file(READ "${prof}/compile_commands.json" compdb)
if(NOT (code EQUAL 0 AND compdb MATCHES "-O0" AND compdb MATCHES "MYDEBUG" AND compdb MATCHES "-fsanitize=address" AND compdb MATCHES "EXTRA"))
    fail("inherit = \"asan\" chains through asan to debug and the built-in defaults")
endif()
file(WRITE "${prof}/luma.lua" [=[
project "prof"
target "app" { kind = "executable", sources = { "src/main.cpp" } }
profile "asan" { inherit = "debug", cxxflags = { "-fsanitize=address" } }
]=])
run_luma("${prof}" out code compdb --profile asan)
file(READ "${prof}/compile_commands.json" compdb)
if(NOT (code EQUAL 0 AND compdb MATCHES "-g" AND compdb MATCHES "-fsanitize=address"))
    fail("inherit = \"debug\" works when debug itself is not declared")
endif()
file(WRITE "${prof}/luma.lua" [=[
project "prof"
target "app" { kind = "executable", sources = { "src/main.cpp" } }
profile "a" { inherit = "b" }
profile "b" { inherit = "a" }
]=])
execute_process(COMMAND "${LUMA}" targets WORKING_DIRECTORY "${prof}" OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE code)
if(code EQUAL 0 OR NOT (err MATCHES "inherits from itself"))
    fail("profile inheritance cycle is an error (got: ${err})")
endif()
file(WRITE "${prof}/luma.lua" [=[
project "prof"
target "app" { kind = "executable", sources = { "src/main.cpp" } }
profile "a" { inherit = "nope" }
]=])
execute_process(COMMAND "${LUMA}" targets WORKING_DIRECTORY "${prof}" OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE code)
if(code EQUAL 0 OR NOT (err MATCHES "unknown profile 'nope'"))
    fail("inheriting from an unknown profile is an error (got: ${err})")
endif()

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} check(s) failed")
endif()
message(STATUS "all CLI checks passed")
