#pragma once
// =============================================================================
// The little statistics the study needs, written out so every interval in a
// report is traceable to one function. Transitions are nested within
// libraries, so intervals for transition-level proportions resample
// LIBRARIES (cluster bootstrap), never transitions (REVIEW.md §3.2).
// =============================================================================

#include <cstdint>
#include <vector>

namespace abistudy {

/// @brief Median of a sample; 0 for an empty sample.
[[nodiscard]] double median(std::vector<std::uint32_t> v);

/// @brief A two-sided percentile interval.
struct Interval {
  double lo = 0;
  double hi = 0;
};

struct BootstrapOptions {
  std::uint32_t resamples = 1000;
  std::uint64_t seed = 42;
};

/// @brief 95 % percentile interval for the proportion of TRUE outcomes across
///        all transitions, resampling libraries with replacement. `clusters[i]`
///        holds the outcomes of library i's transitions.
/// @pre   At least one cluster with at least one outcome.
/// @post  0 <= lo <= hi <= 1.
[[nodiscard]] Interval cluster_bootstrap(
  const std::vector<std::vector<bool>> &clusters, const BootstrapOptions &o = {}
);

/// @brief 95 % percentile interval for a library-level proportion (one
///        outcome per library), resampling libraries with replacement.
/// @pre   `per_library` is non-empty.
[[nodiscard]] Interval library_bootstrap(
  const std::vector<bool> &per_library, const BootstrapOptions &o = {}
);

} // namespace abistudy
