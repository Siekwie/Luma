#include "luma/filesystem/hash.hpp"

#include "luma/filesystem/path_util.hpp"
#include "luma/filesystem/sha256.hpp"

#include <array>
#include <fstream>

namespace luma::fs {

std::optional<Hash> hash_file(const Path& path) {
    std::ifstream file(to_native(path), std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    Sha256 hasher;
    std::array<char, 64 * 1024> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = file.gcount();
        if (read <= 0) {
            break;
        }
        hasher.update(buffer.data(), static_cast<std::size_t>(read));
    }
    if (file.bad()) {
        return std::nullopt;
    }
    return hasher.finish_hex();
}

} // namespace luma::fs
