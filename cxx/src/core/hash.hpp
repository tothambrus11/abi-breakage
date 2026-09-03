#pragma once
// =============================================================================
// Content hashing. SHA-1 is used because it is what snapshot.debian.org keys
// its file store by; it is an identity, not a security boundary.
// =============================================================================

#include <string>
#include <string_view>

namespace abistudy {

/// @brief Lowercase hex SHA-1 of `data`.
/// @post  Exactly 40 characters.
[[nodiscard]] std::string sha1_hex(std::string_view data);

/// @brief Short (16 hex chars) SHA-1 prefix, for compact body/declaration
///        fingerprints in header indexes. Collision risk is negligible at the
///        scale of one package's headers and only equality is ever tested.
[[nodiscard]] std::string fingerprint(std::string_view data);

} // namespace abistudy
