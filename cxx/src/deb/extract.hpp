#pragma once
// =============================================================================
// .deb handling through libarchive, in-process. A .deb is an `ar` archive
// holding `data.tar.{xz,zst,gz}`; the inner tarball is streamed straight out
// of the outer archive, so no intermediate file is written and peak disk use
// is the extracted tree alone.
// =============================================================================

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/error.hpp"

namespace abistudy::deb {

/// @brief What extract_deb produced.
struct ExtractStats {
  std::uint64_t files = 0;   ///< Regular files written.
  std::uint64_t bytes = 0;   ///< Bytes written.
  std::uint64_t refused = 0; ///< Members skipped because their path could escape `dest`.
};

/// @brief Extracts the data tree of `deb` under `dest` (created if needed).
/// @pre   `dest` is non-empty.
/// @post  Entries land at dest/<path-without-leading-"./">. Members with an
///        absolute path or a ".." component are refused and counted in
///        ExtractStats::refused; writing through a planted symlink is refused
///        by libarchive. Device nodes are skipped. Ownership is not restored.
/// @errors io if `deb` is unreadable or `dest` unwritable; parse if `deb` is
///         not an ar archive or has no data.tar member.
[[nodiscard]] Result<ExtractStats> extract_deb(
  const std::filesystem::path &deb, const std::filesystem::path &dest
);

/// @brief Reads a file that may be xz/gz/bz2/zstd-compressed (or plain) into memory.
///        Used for the archive `Packages.xz` index.
/// @errors io; parse if the compressed stream is corrupt.
[[nodiscard]] Result<std::string> read_maybe_compressed(const std::filesystem::path &p);

} // namespace abistudy::deb
