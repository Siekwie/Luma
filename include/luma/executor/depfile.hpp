#pragma once

#include "luma/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace luma::executor {

/// Parse a Makefile-style dependency file as written by `gcc/clang -MMD`.
/// Returns the prerequisites of the first rule (escapes such as "\ " and "$$"
/// are decoded; Windows drive letters are handled).
std::vector<Path> parse_gcc_depfile(std::string_view content);

struct MsvcIncludeScan {
    /// Compiler output with /showIncludes notes (and the echoed source file
    /// name) removed.
    std::string output;
    /// Included files, excluding system headers.
    std::vector<Path> includes;
};

/// Default English prefix of cl.exe /showIncludes lines. Can be overridden
/// with the LUMA_MSVC_DEPS_PREFIX environment variable for localized
/// toolsets.
inline constexpr std::string_view kMsvcShowIncludesPrefix = "Note: including file:";

MsvcIncludeScan scan_msvc_output(
    std::string_view output,
    std::string_view source_file_name,
    std::string_view prefix = kMsvcShowIncludesPrefix);

} // namespace luma::executor
