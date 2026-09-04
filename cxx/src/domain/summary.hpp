#pragma once
// =============================================================================
// The study's aggregate: every figure the reports show, computed once here
// from the transitions and nowhere else. The result is a JSON document (the
// summary artefact) so the text and HTML renderers, and any external tool,
// read the same numbers. Pure.
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "domain/records.hpp"
#include "domain/statistics.hpp"
#include "domain/transition.hpp"

namespace abistudy {

struct SummaryInputs {
  std::vector<Transition> transitions;  ///< Every successfully compared pair.
  std::uint32_t errored = 0;            ///< Pairs that failed (reader, memory, timeout, spawn).
  std::uint32_t no_linkable_object = 0; ///< Pairs whose runtime packages ship no lib*.so to link.
  std::uint32_t not_attempted = 0;      ///< Pairs skipped by budget or deadline.
  std::uint32_t objects = 0;            ///< Shared objects compared.
};

struct SummaryOptions {
  BootstrapOptions bootstrap;
  /// Language thresholds (mangled fraction) reported as a sensitivity check.
  std::vector<double> language_thresholds{0.1, 0.2, 0.5};
};

/// @brief Computes the summary document.
/// @post  Deterministic for equal inputs (bootstrap seed fixed by options).
[[nodiscard]] Json summarize(const SummaryInputs &in, const SummaryOptions &o = {});

/// @brief Renders the summary as the plain-text report.
[[nodiscard]] std::string render_text(const Json &summary);

} // namespace abistudy
