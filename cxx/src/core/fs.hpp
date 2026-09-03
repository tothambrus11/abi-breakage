#pragma once
// =============================================================================
// Filesystem helpers with Result-based error reporting and RAII temporaries.
// std::filesystem throws; these wrappers keep exceptions out of the pipeline.
// =============================================================================

#include <filesystem>
#include <string>
#include <string_view>

#include "core/error.hpp"

namespace abistudy::fs {

namespace stdfs = std::filesystem;

/// @brief Thread-safe text for an errno value (std::strerror is not).
[[nodiscard]] std::string errno_text(int err);

/// @brief Reads a whole file as bytes-in-a-string.
/// @errors io if the file cannot be opened or read.
[[nodiscard]] Result<std::string> read_file(const stdfs::path &p);

/// @brief Writes `content` to `p` atomically: to a sibling temp file, fsync,
///        then rename over `p`.
/// @post  On success `p` holds exactly `content`; on failure `p` is unchanged.
/// @errors io.
[[nodiscard]] Result<void> write_file_atomic(const stdfs::path &p, std::string_view content);

/// @brief mkdir -p.
/// @post  `p` exists and is a directory.
/// @errors io.
[[nodiscard]] Result<void> ensure_dir(const stdfs::path &p);

/// @brief rm -rf that never fails loudly; for cleanup paths.
/// @post  Best effort; returns the number of entries removed (0 if absent).
std::uintmax_t remove_all_noexcept(const stdfs::path &p) noexcept;

/// @brief True if `p` is lexically inside `root` (both made absolute+normal).
///        Used to attribute a declaration to the package that owns its header.
/// @pre   `root` is non-empty.
[[nodiscard]] bool is_under(const stdfs::path &root, const stdfs::path &p);

/// @brief Total size in bytes of the regular files under `p` (0 if absent).
[[nodiscard]] std::uintmax_t tree_size(const stdfs::path &p) noexcept;

/// @brief A directory created on construction and removed on destruction.
/// @invariant While alive, path() exists and is owned by this object.
/// @ownership Non-copyable; movable (the moved-from object owns nothing).
class TempDir {
public:
  /// @brief Creates a fresh directory under `base` (created if needed) with
  ///        a name starting with `prefix`.
  /// @errors io.
  [[nodiscard]] static Result<TempDir> create(const stdfs::path &base, std::string_view prefix);

  TempDir(TempDir &&o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
  TempDir &operator=(TempDir &&o) noexcept;
  TempDir(const TempDir &) = delete ("a TempDir owns its directory; copy would double-delete");
  TempDir &operator=(const TempDir &) =
    delete ("a TempDir owns its directory; copy would double-delete");
  ~TempDir();

  /// @brief The directory; empty only for a moved-from object.
  [[nodiscard]] const stdfs::path &path() const noexcept { return path_; }

  /// @brief Gives up ownership: the directory is NOT removed on destruction.
  /// @post  path() is empty.
  stdfs::path release() noexcept {
    auto p = std::move(path_);
    path_.clear();
    return p;
  }

private:
  explicit TempDir(stdfs::path p) : path_(std::move(p)) {}
  stdfs::path path_;
};

} // namespace abistudy::fs
