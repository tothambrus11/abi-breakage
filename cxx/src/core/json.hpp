#pragma once
// =============================================================================
// JSON parsing with the project's error type. Lives in core so that adapters
// and the composition root do not depend on one another for it.
// =============================================================================

#include <string_view>

#include "core/error.hpp"
#include "core/types.hpp"

namespace abistudy {

/// @brief Parses a JSON document from text.
/// @errors parse if the text is not valid JSON; `what` names the source.
[[nodiscard]] inline Result<Json> parse_json(std::string_view text, std::string_view what) {
  try {
    return Json::parse(text);
  } catch (const Json::exception &e) {
    return fail(ErrorCode::parse, "{}: invalid JSON: {}", what, e.what());
  }
}

} // namespace abistudy
