cmake_minimum_required(VERSION 3.20)

add_executable(luma_graph_tests graph_test.cpp)
target_link_libraries(luma_graph_tests PRIVATE luma_core)
add_test(NAME graph_tests COMMAND luma_graph_tests)

add_executable(luma_unit_tests unit_test.cpp)
target_link_libraries(luma_unit_tests PRIVATE luma_core)
add_test(NAME unit_tests COMMAND luma_unit_tests)

# End-to-end: builds and runs examples/hello and a mixed C/C++ fixture with
# the freshly built luma executable. Needs a working C/C++ toolchain.
add_test(
    NAME cli_tests
    COMMAND ${CMAKE_COMMAND}
        -DLUMA=$<TARGET_FILE:luma>
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli_work
        -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/cli_test.cmake)
