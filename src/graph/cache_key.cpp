#include "luma/graph/task.hpp"

#include <cstdint>
#include <sstream>

namespace luma::graph {

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

    for (const auto& [key, value] : task.environment.vars) {
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

    // FNV-1a 64-bit over the canonical string. Replace with a proper hash later.
    const std::string canonical = out.str();
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : canonical) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }

    std::ostringstream hex;
    hex << std::hex << hash;
    return hex.str();
}

} // namespace luma::graph
