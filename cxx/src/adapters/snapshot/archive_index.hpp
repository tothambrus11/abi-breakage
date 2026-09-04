#pragma once
// =============================================================================
// Readers for the two archive-wide text indexes the selection stage uses:
// popcon's `by_inst` ranking and the `Packages` index (for Depends).
// =============================================================================

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/error.hpp"
#include "core/types.hpp"
#include "domain/archive.hpp"

namespace abistudy::archive {

/// @brief Parses popcon's by_inst file (rank, name, inst, vote, old, recent, no-files).
/// @post  Rows are in file order (ascending rank). Comment and header lines skipped.
/// @errors io; parse if a data row has fewer than three columns.
[[nodiscard]] Result<std::vector<PopconEntry>> parse_popcon(const std::filesystem::path &by_inst);

/// @brief Binary package name -> its Depends line (empty if none).
using DependsIndex = std::unordered_map<BinaryName, std::string>;

/// @brief Parses a (possibly xz/gz-compressed) Packages index for Depends.
/// @errors io; parse if decompression fails.
[[nodiscard]] Result<DependsIndex> parse_packages_depends(
  const std::filesystem::path &packages_file
);

} // namespace abistudy::archive
