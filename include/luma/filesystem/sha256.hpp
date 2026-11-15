#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace luma::fs {

/// Portable, dependency-free SHA-256 (FIPS 180-4).
class Sha256 {
public:
    Sha256();

    void update(const void* data, std::size_t size);
    void update(std::string_view text) { update(text.data(), text.size()); }

    /// Finish the computation and return the 32-byte digest.
    /// The object must not be updated afterwards.
    std::array<std::uint8_t, 32> finish();

    /// Finish and return the digest as 64 lowercase hex characters.
    std::string finish_hex();

private:
    void process_block(const std::uint8_t* block);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

/// SHA-256 of a byte string, as lowercase hex.
std::string sha256_hex(std::string_view data);

} // namespace luma::fs
