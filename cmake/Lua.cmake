set(LUA54_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/lua/src)

set(LUA54_CORE_SOURCES
    ${LUA54_SRC_DIR}/lapi.c
    ${LUA54_SRC_DIR}/lcode.c
    ${LUA54_SRC_DIR}/lctype.c
    ${LUA54_SRC_DIR}/ldebug.c
    ${LUA54_SRC_DIR}/ldo.c
    ${LUA54_SRC_DIR}/ldump.c
    ${LUA54_SRC_DIR}/lfunc.c
    ${LUA54_SRC_DIR}/lgc.c
    ${LUA54_SRC_DIR}/llex.c
    ${LUA54_SRC_DIR}/lmem.c
    ${LUA54_SRC_DIR}/lobject.c
    ${LUA54_SRC_DIR}/lopcodes.c
    ${LUA54_SRC_DIR}/lparser.c
    ${LUA54_SRC_DIR}/lstate.c
    ${LUA54_SRC_DIR}/lstring.c
    ${LUA54_SRC_DIR}/ltable.c
    ${LUA54_SRC_DIR}/ltm.c
    ${LUA54_SRC_DIR}/lundump.c
    ${LUA54_SRC_DIR}/lvm.c
    ${LUA54_SRC_DIR}/lzio.c
    ${LUA54_SRC_DIR}/lauxlib.c
    ${LUA54_SRC_DIR}/lbaselib.c
    ${LUA54_SRC_DIR}/lcorolib.c
    ${LUA54_SRC_DIR}/ldblib.c
    ${LUA54_SRC_DIR}/liolib.c
    ${LUA54_SRC_DIR}/lmathlib.c
    ${LUA54_SRC_DIR}/loadlib.c
    ${LUA54_SRC_DIR}/loslib.c
    ${LUA54_SRC_DIR}/lstrlib.c
    ${LUA54_SRC_DIR}/ltablib.c
    ${LUA54_SRC_DIR}/lutf8lib.c
    ${LUA54_SRC_DIR}/linit.c
)

if(NOT EXISTS ${LUA54_SRC_DIR}/lapi.c)
    message(FATAL_ERROR "Lua sources missing. Expected ${LUA54_SRC_DIR}/lapi.c")
endif()

add_library(lua54 STATIC ${LUA54_CORE_SOURCES})
target_include_directories(lua54 PUBLIC ${LUA54_SRC_DIR})

if(WIN32)
    # luaconf.h defines LUA_USE_WINDOWS itself on _WIN32.
    if(MSVC)
        target_compile_definitions(lua54 PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
else()
    target_compile_definitions(lua54 PUBLIC LUA_USE_POSIX)
    target_link_libraries(lua54 PUBLIC m dl)
endif()
