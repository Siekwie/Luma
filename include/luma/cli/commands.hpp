#pragma once

#include "luma/graph/build_graph.hpp"

#include <optional>
#include <string>
#include <vector>

namespace luma::cli {

struct CommandResult {
    int exit_code;
};

CommandResult run(int argc, char** argv);

} // namespace luma::cli
