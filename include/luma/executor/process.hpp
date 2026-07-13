#pragma once

#include <string>
#include <vector>

namespace luma::executor {

struct ProcessResult {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
};

ProcessResult run_process(
    const std::vector<std::string>& command,
    const std::string& working_directory = {});

std::string format_command(const std::vector<std::string>& command);

} // namespace luma::executor
