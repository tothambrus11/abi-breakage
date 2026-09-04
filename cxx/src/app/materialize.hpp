#pragma once
// =============================================================================
// Turns a Release descriptor into files on disk: downloads the packages a
// stage needs and extracts them into a scratch tree, then finds the shared
// objects and include roots inside. Both the diff and the header stages use
// this, so disk discipline (delete after use, verify hashes) lives in one place.
// =============================================================================

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "app/stages.hpp"
#include "core/fs.hpp"

namespace abistudy::app {

/// @brief Which package roles to materialise.
struct Want {
  bool runtime = true; ///< The shared-library packages.
  bool dbgsym = true;  ///< Their -dbgsym siblings (DWARF).
  bool dev = true;     ///< The -dev packages (headers).
};

/// @brief A release extracted on disk.
/// @invariant Directories exist while the owning TempDir is alive.
struct Materialized {
  fs::TempDir scratch;                               ///< Owns everything below.
  std::filesystem::path runtime_root;                ///< dest of runtime debs (may be empty tree).
  std::filesystem::path debug_root;                  ///< "<dbgsym>/usr/lib/debug" or empty if none.
  std::filesystem::path include_root;                ///< "<dev>/usr/include" or empty if none.
  std::vector<std::filesystem::path> shared_objects; ///< Linkable ELF lib*.so* under runtime_root.
  /// lib*.so* ELF files under runtime_root that are NOT linkable (plugins in
  /// subdirectories), relative to runtime_root: recorded, never compared.
  std::vector<std::string> excluded_objects;
  std::uint64_t bytes_extracted = 0;
  std::vector<std::string> missing; ///< Packages requested but without an amd64 .deb.
};

/// @brief The package names a release needs for the requested roles.
[[nodiscard]] std::vector<BinaryName> packages_for(const Release &rel, Want want);

/// @brief Downloads and extracts the requested roles of `rel` under `scratch_base`.
/// @pre   `scratch_base` is non-empty.
/// @post  Every .deb downloaded is deleted after extraction; peak disk is the
///        extracted tree only. A package with no amd64/all file is reported in
///        `missing`, not treated as an error.
/// @errors network/http_status/integrity from the source; io; parse if a .deb
///         is malformed.
[[nodiscard]] Result<Materialized> materialize(
  const Services &sv, const Release &rel, Want want, const std::filesystem::path &scratch_base
);

/// @brief The newest binary version of `name` in `rel` that has an amd64 (or
///        arch-independent) .deb, with the file's hash. A binNMU that never
///        built on amd64 sorts newest but has no file; the older version does.
/// @returns nullopt if no version has such a file.
/// @errors network/http_status/parse from the package source: a transient
///         failure is an error, never "no file".
[[nodiscard]] Result<std::optional<std::pair<VersionString, FileHash>>> first_amd64(
  const Services &sv, const Release &rel, const BinaryName &name
);

/// @brief Directories, relative to a -dev include tree's package root, in
///        which the -dev package installs `lib*.so` development links: the
///        directories the package itself declares as link-search paths
///        (`hdf5/serial/`, `blas/`), on top of the default ones.
[[nodiscard]] std::vector<std::string> dev_link_dirs(const std::filesystem::path &dev_root);

/// @brief Splits the regular ELF files named lib*.so* below `root` into the
///        linkable ones (in a default link directory, see
///        is_linkable_library_dir, or in one of `extra_dirs`) and the rest
///        (plugins), the latter as paths relative to `root`. Symlinks are
///        skipped so each object is seen once.
struct FoundObjects {
  std::vector<std::filesystem::path> linkable;
  std::vector<std::string> excluded;
};
[[nodiscard]] FoundObjects find_shared_objects(
  const std::filesystem::path &root, std::span<const std::string> extra_dirs = {}
);

} // namespace abistudy::app
