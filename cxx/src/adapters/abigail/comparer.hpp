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
#include <unordered_map>
#include <vector>

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

/// @brief The headers a -dev package ships, indexed by basename, each as its
///        path relative to the include root ("freetype2/freetype/freetype.h").
struct ShippedHeaders {
  std::unordered_map<std::string, std::vector<std::string>> by_basename;
  [[nodiscard]] bool empty() const noexcept { return by_basename.empty(); }
  void add(std::string relative_path);
};

/// @brief Attribution of a DWARF declaration path to the library's own
///        headers (REVIEW.md §1.6). DWARF records the path the type was
///        compiled from (a build-tree path such as "../include/freetype/
///        freetype.h"); the installed tree may add a prefix directory
///        ("freetype2/"). A path is the library's own iff a shipped header
///        has the same basename -- and, for a path under the system include
///        directories, additionally agrees on the parent directory or
///        matches the installed relative path exactly, so glibc's
///        `bits/types.h` is never claimed by a library that ships a
///        top-level `types.h`.
[[nodiscard]] bool declared_in_own_headers(
  std::string_view dwarf_path, const ShippedHeaders &shipped
);

} // namespace abistudy::abigail
