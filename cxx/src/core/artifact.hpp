#pragma once
// =============================================================================
// Pipeline artefacts: the JSON files each stage writes for the next. Every
// artefact carries a schema identifier so a stage refuses input produced by
// an incompatible version instead of silently misreading it.
//
//   { "schema": "abistudy/<stage>/<n>", "tool": "abistudy 3.0.0",
//     "generated_at": "<ISO-8601 UTC>", "data": <payload> }
// =============================================================================

#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/error.hpp"

namespace abistudy {

using Json = nlohmann::json;

/// @brief Identifies the layout of an artefact's payload.
/// @invariant `id` looks like "abistudy/<stage>/<positive integer>".
struct Schema {
  std::string_view id;
};

/// @brief Version string of this build, embedded in every artefact.
[[nodiscard]] constexpr std::string_view tool_version() noexcept { return "abistudy 3.0.0"; }

/// @brief Parses a JSON document from text.
/// @errors parse if the text is not valid JSON.
[[nodiscard]] Result<Json> parse_json(std::string_view text, std::string_view what);

/// @brief Reads an artefact and checks its schema.
/// @post  On success the returned value is the "data" payload only.
/// @errors io if unreadable; parse if not JSON or not an artefact envelope;
///         schema if the embedded schema differs from `expected`.
[[nodiscard]] Result<Json> load_artifact(const std::filesystem::path &p, Schema expected);

/// @brief Writes `payload` wrapped in the artefact envelope, atomically.
/// @errors io.
[[nodiscard]] Result<void> save_artifact(
  const std::filesystem::path &p, Schema schema, const Json &payload
);

/// @brief Typed convenience over load_artifact: converts the payload to T.
/// @errors As load_artifact, plus parse if the payload does not convert to T.
template <class T>
[[nodiscard]] Result<T> load_artifact_as(const std::filesystem::path &p, Schema expected) {
  ABISTUDY_TRY(Json j, load_artifact(p, expected));
  try {
    return j.get<T>();
  } catch (const Json::exception &e) {
    return fail(
      ErrorCode::parse, "'{}': payload does not match {}: {}", p.string(), expected.id, e.what()
    );
  }
}

/// @brief Current UTC time as ISO-8601 ("2026-09-02T14:03:11Z").
[[nodiscard]] std::string utc_now_iso8601();

} // namespace abistudy
