#pragma once
// =============================================================================
// Port: where packages come from. The application asks these questions; an
// adapter (snapshot.debian.org today) answers them. Nothing above this
// interface knows about HTTP, caching or rate limits.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include "core/error.hpp"
#include "core/types.hpp"
#include "domain/archive.hpp"
#include "domain/version.hpp"

namespace abistudy::ports {

class PackageSource {
public:
  virtual ~PackageSource() = default;
  PackageSource() = default;
  PackageSource(const PackageSource &) = delete;
  PackageSource &operator=(const PackageSource &) = delete;
  PackageSource(PackageSource &&) = delete;
  PackageSource &operator=(PackageSource &&) = delete;

  /// @brief All archive versions of a SOURCE package, each parsed.
  /// @errors not_found if the source package is unknown; parse; network/http_status.
  [[nodiscard]] virtual Result<std::vector<DebianVersion>> source_versions(
    const SourceName &src
  ) const = 0;

  /// @brief Binary packages produced by one source version.
  [[nodiscard]] virtual Result<std::vector<BinaryBuild>> binary_packages(
    const SourceName &src, const VersionString &ver
  ) const = 0;

  /// @brief Source package and newest version for a BINARY package name.
  /// @returns nullopt if the binary package has no versions in the archive.
  [[nodiscard]] virtual Result<std::optional<BinaryOrigin>> binary_origin(
    const BinaryName &bin
  ) const = 0;

  /// @brief Files (per architecture) for a binary package version.
  [[nodiscard]] virtual Result<std::vector<BinaryFile>> binary_files(
    const BinaryName &bin, const VersionString &ver
  ) const = 0;

  /// @brief Size in bytes of a content-addressed file; nullopt if unknown.
  [[nodiscard]] virtual Result<std::optional<std::uint64_t>> file_size(
    const FileHash &hash
  ) const = 0;

  /// @brief Downloads the content-addressed file to `dest`, verified.
  /// @post  On success `dest` exists and hashes to `hash`; no partial file otherwise.
  [[nodiscard]] virtual Result<void> download(
    const FileHash &hash, const std::filesystem::path &dest
  ) const = 0;

  /// @brief Plain GET of a URL to `dest`, reusing `dest` if younger than `max_age`.
  [[nodiscard]] virtual Result<void> fetch_to_file(
    std::string_view url, const std::filesystem::path &dest, std::chrono::seconds max_age
  ) const = 0;
};

/// @brief Convenience over binary_files: the hash of the amd64 (or "all") .deb.
/// @returns nullopt if neither architecture has a file.
[[nodiscard]] Result<std::optional<FileHash>> amd64_deb_hash(
  const PackageSource &src, const BinaryName &bin, const VersionString &ver
);

} // namespace abistudy::ports
