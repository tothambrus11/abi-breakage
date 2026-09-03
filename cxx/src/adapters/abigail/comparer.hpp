#pragma once
// =============================================================================
// AbiComparer adapter over the libabigail library API.
//
// The comparison runs in-process and the change taxonomy is derived by
// walking the diff tree, so every count is traceable to a specific IR node.
// Besides the taxonomy this adapter records the facts the lenient break
// definition needs: how each changed type is exposed through the exported
// interface, whether a layout change is append-only, and each symbol's
// binding (REVIEW.md §1.1, §1.4).
// =============================================================================

#include <string>
#include <string_view>
#include <unordered_set>

#include "ports/abi_comparer.hpp"

namespace abistudy::abigail {

class AbigailComparer final : public ports::AbiComparer {
public:
  /// @thread Not thread-safe: libabigail keeps per-environment state. Run
  ///         concurrent comparisons in separate processes.
  [[nodiscard]] Result<SharedObjectDiff> compare(
    const ports::Side &a, const ports::Side &b, const ports::CompareOptions &opt
  ) const override;

  /// @brief "libabigail <major>.<minor>.<revision>".
  [[nodiscard]] std::string version() const override;
};

/// @brief Attribution of a DWARF declaration path to the library's own
///        headers (REVIEW.md §1.6). `shipped` holds the paths of the -dev
///        headers relative to their include root ("foo/bar.h", "zlib.h").
///        A path matches by include-relative SUFFIX; a path under the system
///        include directories additionally needs an exact relative match or
///        a multi-component suffix, so glibc's `bits/types.h` is never
///        claimed by a library that ships a top-level `types.h`.
[[nodiscard]] bool declared_in_own_headers(
  std::string_view dwarf_path, const std::unordered_set<std::string> &shipped
);

} // namespace abistudy::abigail
