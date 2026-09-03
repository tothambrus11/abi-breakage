#pragma once
// =============================================================================
// Domain vocabulary. One strong type per concept that the pipeline passes
// around. The tags are empty structs; they exist only to make the types
// distinct.
// =============================================================================

#include <cstdint>
#include <string>

#include "core/strong.hpp"

namespace abistudy {

// ---- Debian archive identities ---------------------------------------------

/// @brief Name of a Debian SOURCE package (e.g. "openssl"). Never a binary name.
using SourceName = Strong<std::string, struct SourceNameTag>;

/// @brief Name of a Debian BINARY package (e.g. "libssl3t64", "libssl-dev").
using BinaryName = Strong<std::string, struct BinaryNameTag>;

/// @brief A full Debian version string as it appears in the archive,
///        e.g. "1:2.40.1-3". Compare with DebianVersion, not lexically.
using VersionString = Strong<std::string, struct VersionStringTag>;

/// @brief The upstream part of a Debian version ("2.40.1" from "1:2.40.1-3").
///        Two archive versions with equal UpstreamVersion are the same release
///        of the library and are never paired.
using UpstreamVersion = Strong<std::string, struct UpstreamVersionTag>;

/// @brief SHA-1 of a file in snapshot.debian.org, as 40 lowercase hex chars.
using FileHash = Strong<std::string, struct FileHashTag>;

/// @brief Debian architecture string ("amd64", "all").
using Architecture = Strong<std::string, struct ArchitectureTag>;

// ---- ELF / ABI identities ---------------------------------------------------

/// @brief An ELF DT_SONAME value, e.g. "libssl.so.3".
using Soname = Strong<std::string, struct SonameTag>;

/// @brief SONAME with its version suffix stripped ("libssl" from "libssl.so.3").
///        Used to pair shared objects across a SONAME bump.
using SonameStem = Strong<std::string, struct SonameStemTag>;

/// @brief An exported ELF symbol name (mangled if C++).
using SymbolName = Strong<std::string, struct SymbolNameTag>;

/// @brief An ELF symbol version node name, e.g. "LIBDBUS_PRIVATE_1.16.2".
using VersionNode = Strong<std::string, struct VersionNodeTag>;

// ---- Counts and ranks -------------------------------------------------------

/// @brief Debian popcon rank (1 = most installed). Lower is more popular.
using PopconRank = Strong<std::uint32_t, struct PopconRankTag>;

/// @brief Number of popcon submitters that have the package installed.
using InstallCount = Strong<std::uint64_t, struct InstallCountTag>;

/// @brief Number of ABI-change events of one kind within one transition.
using EventCount = Strong<std::uint32_t, struct EventCountTag>;

// ---- Enumerations -----------------------------------------------------------

/// @brief Implementation language of a shared object, decided from the
///        fraction of Itanium-mangled exported function symbols.
enum class Language : std::uint8_t { c, cxx, unknown };

/// @brief Name used in JSON and reports.
[[nodiscard]] constexpr const char *to_string(Language l) noexcept {
  switch (l) {
  case Language::c:
    return "c";
  case Language::cxx:
    return "cxx";
  case Language::unknown:
    return "unknown";
  }
  return "unknown";
}

} // namespace abistudy
