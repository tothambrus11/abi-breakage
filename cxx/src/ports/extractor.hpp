#pragma once
// =============================================================================
// Port: unpacks a .deb into a directory tree. libarchive is the adapter.
// =============================================================================

#include <cstdint>
#include <filesystem>

#include "core/error.hpp"

namespace abistudy::ports {

/// @brief What an extraction produced.
struct ExtractStats {
  std::uint64_t files = 0;   ///< Regular files written.
  std::uint64_t bytes = 0;   ///< Bytes written.
  std::uint64_t refused = 0; ///< Members skipped because their path could escape `dest`.
};

class PackageExtractor {
public:
  virtual ~PackageExtractor() = default;
  PackageExtractor() = default;
  PackageExtractor(const PackageExtractor &) = delete;
  PackageExtractor &operator=(const PackageExtractor &) = delete;
  PackageExtractor(PackageExtractor &&) = delete;
  PackageExtractor &operator=(PackageExtractor &&) = delete;

  /// @brief Extracts the data tree of `deb` under `dest` (created if needed).
  /// @pre   `dest` is non-empty.
  /// @post  Members with an absolute path or a ".." component are refused and
  ///        counted; nothing is written outside `dest`.
  /// @errors io; parse if `deb` is not a Debian package.
  [[nodiscard]] virtual Result<ExtractStats> extract(
    const std::filesystem::path &deb, const std::filesystem::path &dest
  ) const = 0;
};

} // namespace abistudy::ports
