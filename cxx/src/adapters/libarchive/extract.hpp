#pragma once
// =============================================================================
// PackageExtractor adapter over libarchive, in-process. A .deb is an `ar`
// archive holding `data.tar.{xz,zst,gz}`; the inner tarball is streamed
// straight out of the outer archive, so no intermediate file is written and
// peak disk use is the extracted tree alone.
// =============================================================================

#include <filesystem>
#include <string>

#include "ports/extractor.hpp"

namespace abistudy::deb {

class LibarchiveExtractor final : public ports::PackageExtractor {
public:
  [[nodiscard]] Result<ports::ExtractStats> extract(
    const std::filesystem::path &deb, const std::filesystem::path &dest
  ) const override;
};

/// @brief Reads a file that may be xz/gz/bz2/zstd-compressed (or plain) into memory.
///        Used for the archive `Packages.xz` index.
/// @errors io; parse if the compressed stream is corrupt.
[[nodiscard]] Result<std::string> read_maybe_compressed(const std::filesystem::path &p);

} // namespace abistudy::deb
