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

} // namespace abistudy
