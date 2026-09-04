#pragma once
// =============================================================================
// Pure string rules about ELF names that more than one layer needs.
// =============================================================================

#include <string>
#include <string_view>

#include "core/types.hpp"

namespace abistudy {

/// @brief Strips the version suffix from a SONAME: "libssl.so.3" -> "libssl".
/// @post  Never empty for a non-empty input.
[[nodiscard]] SonameStem soname_stem(std::string_view soname_or_filename);

/// @brief True if an ELF version node name marks the symbol as private
///        ("LIBDBUS_PRIVATE_1.16.2", "GLIBC_PRIVATE", "..._INTERNAL").
[[nodiscard]] bool is_private_version_node(std::string_view node) noexcept;

/// @brief Replaces every digit run with '#': "u_strlen_72" -> "u_strlen_#".
///        Used to recognise policy-driven per-release symbol renames.
[[nodiscard]] std::string digits_blind(std::string_view s);

/// @brief True if `relative_dir` (a path relative to a package root, no
///        leading slash) is a directory the link editor searches by default:
///        lib, usr/lib, lib64, usr/lib64, optionally with one multiarch
///        triplet below. A shared object anywhere deeper (sane/, spa-0.2/,
///        gstreamer-1.0/, caca/, security/) is a dlopen'ed plugin: nothing
///        links against it with -l, so it is not part of the library's ABI.
[[nodiscard]] bool is_linkable_library_dir(std::string_view relative_dir) noexcept;

} // namespace abistudy
