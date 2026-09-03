#pragma once
// =============================================================================
// Facts about the Debian archive as the domain sees them: what a package
// source answers, and what the two archive-wide indexes (popcon ranking,
// Packages) contribute to corpus selection. Pure data.
// =============================================================================

#include <string>

#include "core/types.hpp"

namespace abistudy {

/// @brief A binary package built from some source version.
struct BinaryBuild {
  BinaryName name;
  VersionString version; ///< Binary version; can differ from the source version (binNMU).
};

/// @brief A file available for a binary package version.
struct BinaryFile {
  FileHash hash;
  Architecture arch;
  std::string filename; ///< e.g. "libssl3t64_3.4.1-1_amd64.deb"
};

/// @brief Source package and newest version for a BINARY package name.
struct BinaryOrigin {
  SourceName source;
  VersionString newest;
};

/// @brief One popcon row.
// NOLINTBEGIN(cppcoreguidelines-pro-type-member-init): Strong members have no default value;
// the aggregate is always initialised in full.
struct PopconEntry {
  PopconRank rank;
  BinaryName name;
  InstallCount installs;
};
// NOLINTEND(cppcoreguidelines-pro-type-member-init)

/// @brief Language marker used by the selection stage: a C++ shared library
///        essentially always depends on libstdc++6.
[[nodiscard]] inline Language language_from_depends(const std::string &depends) {
  return depends.contains("libstdc++6") ? Language::cxx : Language::c;
}

} // namespace abistudy
