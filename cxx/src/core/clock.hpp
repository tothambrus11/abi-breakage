#pragma once
// =============================================================================
// Wall-clock helpers shared by adapters, stages and the composition root.
// =============================================================================

#include <chrono>
#include <format>
#include <string>

namespace abistudy {

/// @brief Current UTC time as ISO-8601 ("2026-09-02T14:03:11Z").
[[nodiscard]] inline std::string utc_now_iso8601() {
  const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  return std::format("{:%FT%TZ}", now);
}

} // namespace abistudy
