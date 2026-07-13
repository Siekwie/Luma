#include "luma/graph/build_graph.hpp"
#include "luma/graph/task.hpp"
#include "luma/types.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace luma::graph;

static int failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << #cond << " (" << __FILE__ << ':' << __LINE__ << ")\n"; \
            ++failures; \
        } \
    } while (0)

static BuildGraph make_linear_graph() {
    BuildGraph graph;

    Task a{
        .id = TaskId{"a"},
        .outputs = {"a.out"},
    };
    Task b{
        .id = TaskId{"b"},
        .outputs = {"b.out"},
        .dependencies = {TaskId{"a"}},
    };
    Task c{
        .id = TaskId{"c"},
        .outputs = {"c.out"},
        .dependencies = {TaskId{"b"}},
    };

    CHECK(!graph.add_task(std::move(a)));
    CHECK(!graph.add_task(std::move(b)));
    CHECK(!graph.add_task(std::move(c)));
    return graph;
}

static void test_topological_order() {
    const auto graph = make_linear_graph();
    const auto order = graph.topological_order();
    CHECK(order.has_value());
    CHECK(order->size() == 3);
    CHECK((*order)[0].value == "a");
    CHECK((*order)[1].value == "b");
    CHECK((*order)[2].value == "c");
}

static void test_duplicate_id() {
    BuildGraph graph;
    Task t{.id = TaskId{"dup"}, .outputs = {"x"}};
    CHECK(!graph.add_task(Task{t}));
    const auto err = graph.add_task(std::move(t));
    CHECK(err.has_value());
    CHECK(err->code == GraphError::Code::DuplicateTaskId);
}

static void test_unknown_dependency() {
    BuildGraph graph;
    Task t{
        .id = TaskId{"orphan"},
        .dependencies = {TaskId{"missing"}},
    };
    CHECK(!graph.add_task(std::move(t)));
    const auto err = graph.validate();
    CHECK(err.has_value());
    CHECK(err->code == GraphError::Code::UnknownDependency);
}

static void test_cycle_detection() {
    BuildGraph graph;

    Task a{.id = TaskId{"a"}, .dependencies = {TaskId{"b"}}};
    Task b{.id = TaskId{"b"}, .dependencies = {TaskId{"a"}}};

    CHECK(!graph.add_task(std::move(a)));
    CHECK(!graph.add_task(std::move(b)));

    const auto err = graph.validate();
    CHECK(err.has_value());
    CHECK(err->code == GraphError::Code::CycleDetected);
    CHECK(!graph.topological_order().has_value());
}

static void test_cache_key_stability() {
    Task task{
        .id = TaskId{"compile:main.o"},
        .command = {"clang++", "-c", "main.cpp"},
    };

    const luma::Hash k1 = compute_cache_key(task, {"abc123"});
    const luma::Hash k2 = compute_cache_key(task, {"abc123"});
    CHECK(k1 == k2);

    const luma::Hash k3 = compute_cache_key(task, {"def456"});
    CHECK(k1 != k3);
}

int main() {
    test_topological_order();
    test_duplicate_id();
    test_unknown_dependency();
    test_cycle_detection();
    test_cache_key_stability();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "all graph tests passed\n";
    return 0;
}
