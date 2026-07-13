#include "luma/filesystem/hash.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace luma::fs {

namespace {

Hash to_hex(const std::uint8_t* data, std::size_t size) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        out << std::setw(2) << static_cast<int>(data[i]);
    }
    return out.str();
}

#ifndef _WIN32
Hash fnv1a_fallback(const std::string& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : bytes) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}
#endif

} // namespace

std::optional<Hash> hash_file(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash_handle = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return std::nullopt;
    }

    DWORD hash_object_size = 0;
    DWORD hash_data_size = 0;
    DWORD cb_data = 0;
    if (BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_object_size),
            sizeof(hash_object_size),
            &cb_data,
            0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }
    if (BCryptGetProperty(
            algorithm,
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_data_size),
            sizeof(hash_data_size),
            &cb_data,
            0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }

    std::vector<std::uint8_t> hash_object(hash_object_size);
    std::array<std::uint8_t, 32> hash_bytes{};
    if (BCryptCreateHash(
            algorithm,
            &hash_handle,
            hash_object.data(),
            hash_object_size,
            nullptr,
            0,
            0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }

    std::array<char, 8192> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = file.gcount();
        if (read <= 0) {
            break;
        }
        if (BCryptHashData(
                hash_handle,
                reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(read),
                0) != 0) {
            BCryptDestroyHash(hash_handle);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return std::nullopt;
        }
    }

    if (BCryptFinishHash(hash_handle, hash_bytes.data(), hash_data_size, 0) != 0) {
        BCryptDestroyHash(hash_handle);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }

    BCryptDestroyHash(hash_handle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return to_hex(hash_bytes.data(), hash_bytes.size());
#else
    std::string bytes;
    std::array<char, 8192> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = file.gcount();
        if (read <= 0) {
            break;
        }
        bytes.append(buffer.data(), static_cast<std::size_t>(read));
    }
    return fnv1a_fallback(bytes);
#endif
}

} // namespace luma::fs
