#include "luma/graph/task.hpp"

#include "luma/filesystem/sha256.hpp"

#include <algorithm>
#include <sstream>

namespace luma::graph {

namespace fs = luma::fs;

namespace {

void append_kv(std::ostringstream& out, std::string_view key, std::string_view value) {
    out << key << '=' << value << '\n';
}

} // namespace

Hash compute_cache_key(
    const Task& task,
    const std::vector<Hash>& input_hashes,
    const std::string& toolchain_identity,
    const std::string& configuration_fingerprint) {
    std::ostringstream out;

    append_kv(out, "task", task.id.value);

    for (const auto& arg : task.command) {
        append_kv(out, "cmd", arg);
    }

    std::vector<std::pair<std::string, std::string>> env(
        task.environment.vars.begin(), task.environment.vars.end());
    std::sort(env.begin(), env.end());
    for (const auto& [key, value] : env) {
        append_kv(out, "env." + key, value);
    }

    if (task.working_directory) {
        append_kv(out, "cwd", *task.working_directory);
    }

    for (const auto& hash : input_hashes) {
        append_kv(out, "input_hash", hash);
    }

    if (!toolchain_identity.empty()) {
        append_kv(out, "toolchain", toolchain_identity);
    }

    if (!configuration_fingerprint.empty()) {
        append_kv(out, "config", configuration_fingerprint);
    }

    return fs::sha256_hex(out.str());
}

} // namespace luma::graph
