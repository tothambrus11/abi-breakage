#pragma once
// =============================================================================
// Turns a Release descriptor into files on disk: downloads the packages a
// stage needs and extracts them into a scratch tree, then finds the shared
// objects and include roots inside. Both the diff and the header stages use
// this, so disk discipline (delete after use, verify hashes) lives in one place.
// =============================================================================

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/fs.hpp"
#include "pipeline/artifacts.hpp"
#include "snapshot/client.hpp"

namespace abistudy::pipeline {

/// @brief Which package roles to materialise.
struct Want {
  bool runtime = true; ///< The shared-library packages.
  bool dbgsym = true;  ///< Their -dbgsym siblings (DWARF).
  bool dev = true;     ///< The -dev packages (headers).
  /// If non-zero, the release is NOT downloaded when the sum of its .deb sizes
  /// (from HEAD requests) exceeds this; Materialized::too_large says so.
  std::uint64_t max_download_bytes = 0;
};

/// @brief A release extracted on disk.
/// @invariant Directories exist while the owning TempDir is alive.
struct Materialized {
  fs::TempDir scratch;                ///< Owns everything below.
  std::filesystem::path runtime_root; ///< dest of runtime debs (may be empty tree).
  std::filesystem::path debug_root;   ///< "<dbgsym>/usr/lib/debug" or empty if none.
  std::filesystem::path include_root; ///< "<dev>/usr/include" or empty if none.
  std::vector<std::filesystem::path>
    shared_objects; ///< Regular ELF files lib*.so* under runtime_root.
  std::uint64_t bytes_extracted = 0;
  std::vector<std::string> missing; ///< Packages requested but without an amd64 .deb.
  std::optional<std::string>
    too_large; ///< Set (and nothing fetched) when over Want::max_download_bytes.
};

/// @brief Sum of the .deb sizes (HEAD requests) the requested roles of `rel`
///        would download. Packages without a size header count as zero.
/// @errors network/http_status from the client.
[[nodiscard]] Result<std::uint64_t> estimate_download_bytes(
  const snapshot::Client &client, const Release &rel, Want want
);

/// @brief Downloads and extracts the requested roles of `rel` under `scratch_base`.
/// @pre   `scratch_base` is non-empty.
/// @post  Every .deb downloaded is deleted after extraction; peak disk is the
///        extracted tree only. A package with no amd64/all file is reported in
///        `missing`, not treated as an error.
/// @errors network/http_status/integrity from the client; io; parse if a .deb
///         is malformed.
[[nodiscard]] Result<Materialized> materialize(
  const snapshot::Client &client, const Release &rel, Want want,
  const std::filesystem::path &scratch_base
);

/// @brief Regular files named lib*.so* whose first four bytes are the ELF
///        magic, below `root`. Symlinks are skipped so each object is seen once.
[[nodiscard]] std::vector<std::filesystem::path> find_shared_objects(
  const std::filesystem::path &root
);

} // namespace abistudy::pipeline
