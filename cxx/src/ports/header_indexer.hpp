#pragma once
// =============================================================================
// Port: builds a HeaderIndex from an include tree. libclang is the adapter.
// =============================================================================

#include <filesystem>
#include <string>

#include "core/error.hpp"
#include "domain/header_model.hpp"

namespace abistudy::ports {

class HeaderIndexer {
public:
  virtual ~HeaderIndexer() = default;
  HeaderIndexer() = default;
  HeaderIndexer(const HeaderIndexer &) = delete;
  HeaderIndexer &operator=(const HeaderIndexer &) = delete;
  HeaderIndexer(HeaderIndexer &&) = delete;
  HeaderIndexer &operator=(HeaderIndexer &&) = delete;

  /// @brief Indexes every header under `include_root`.
  /// @pre   `include_root` is an existing directory.
  /// @post  coverage.header_files == parsed + failed-to-create + skipped_by_limit.
  ///        Definitions and macros from files OUTSIDE include_root are never
  ///        recorded; declared symbols are recorded from any file.
  /// @errors clang if no index can be created at all. Individual
  ///         translation-unit failures are NOT errors: they are counted.
  [[nodiscard]] virtual Result<HeaderIndex> index(
    const std::filesystem::path &include_root, const IndexOptions &opt
  ) const = 0;

  /// @brief Version of the underlying parser, for provenance.
  [[nodiscard]] virtual std::string version() const = 0;
};

} // namespace abistudy::ports
