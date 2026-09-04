#pragma once
// =============================================================================
// HeaderIndexer adapter over the libclang C API. Measures what no ABI tool
// can: changes to the bodies C and C++ copy into every client -- header
// `inline` functions, in-class methods, templates -- and to object-like macro
// values; and records the mangled names of every declaration in the
// library's own headers, so exported symbols can be classified as declared
// or not (REVIEW.md §1.4).
// =============================================================================

#include "ports/header_indexer.hpp"

namespace abistudy::libclang {

class LibclangIndexer final : public ports::HeaderIndexer {
public:
  /// @thread Safe to call concurrently (one CXIndex per call).
  [[nodiscard]] Result<HeaderIndex> index(
    const std::filesystem::path &include_root, const IndexOptions &opt
  ) const override;

  /// @brief libclang's own version string.
  [[nodiscard]] std::string version() const override;
};

} // namespace abistudy::libclang
