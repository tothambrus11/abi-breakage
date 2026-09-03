#pragma once
// =============================================================================
// Typed views over the snapshot.debian.org machine-readable API. Each function
// maps one endpoint to domain types; nothing above this layer sees JSON.
// Endpoint reference: https://snapshot.debian.org/ ("machine-readable API").
// =============================================================================

#include <optional>
#include <utility>
#include <vector>

#include "core/version.hpp"
#include "snapshot/client.hpp"

namespace abistudy::snapshot {

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

/// @brief All archive versions of a SOURCE package, newest first as the API
///        returns them, each parsed.
/// @errors not_found if the source package is unknown; parse on malformed
///         responses; network/http_status from the client.
[[nodiscard]] Result<std::vector<DebianVersion>> source_versions(
  const Client & /*c*/, const SourceName &
  /*src*/
);

/// @brief Binary packages produced by one source version.
/// @errors As source_versions.
[[nodiscard]] Result<std::vector<BinaryBuild>> binary_packages(
  const Client & /*c*/, const SourceName & /*src*/, const VersionString &
  /*ver*/
);

/// @brief Source package and newest version for a BINARY package name.
/// @returns nullopt if the binary package has no versions in the archive.
/// @errors As source_versions (not_found is mapped to nullopt).
struct BinaryOrigin {
  SourceName source;
  VersionString newest;
};
[[nodiscard]] Result<std::optional<BinaryOrigin>> binary_origin(
  const Client & /*c*/, const BinaryName & /*bin*/
);

/// @brief Files (per architecture) for a binary package version.
/// @errors As source_versions.
[[nodiscard]] Result<std::vector<BinaryFile>> binary_files(
  const Client & /*c*/, const BinaryName & /*bin*/, const VersionString &
  /*ver*/
);

/// @brief Convenience: the hash of the amd64 (or arch-independent "all")
///        .deb for a binary package version.
/// @returns nullopt if neither architecture has a file.
[[nodiscard]] Result<std::optional<FileHash>> amd64_deb_hash(
  const Client & /*c*/, const BinaryName & /*bin*/, const VersionString &
  /*ver*/
);

} // namespace abistudy::snapshot
