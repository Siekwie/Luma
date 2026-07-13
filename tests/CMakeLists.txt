cmake_minimum_required(VERSION 3.20)

add_executable(luma_graph_tests
    graph_test.cpp
)

target_link_libraries(luma_graph_tests PRIVATE luma_core)

add_test(NAME graph_tests COMMAND luma_graph_tests)
