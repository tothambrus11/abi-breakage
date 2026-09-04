#pragma once
// =============================================================================
// Port: persistence of the study's artefacts. Every artefact is a JSON
// payload wrapped in an envelope with a schema id and provenance:
//
//   { "schema": "abistudy/<stage>/<n>", "tool": "abistudy 4.0.0",
//     "generated_at": "<ISO-8601 UTC>", "provenance": {...}, "data": <payload> }
//
// A stage refuses an artefact whose schema differs from what it expects.
// =============================================================================

#include <filesystem>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "domain/records.hpp"

namespace abistudy::ports {

/// @brief Version string of this build, embedded in every artefact.
[[nodiscard]] constexpr std::string_view tool_version() noexcept { return "abistudy 4.0.0"; }

/// @brief The readers whose output an artefact depends on (REVIEW.md §4.2).
struct Provenance {
  std::string abi_reader;    ///< e.g. "libabigail 2.4.0"
  std::string header_parser; ///< e.g. "clang version 23.1.1"
};

class ArtifactStore {
public:
  virtual ~ArtifactStore() = default;
  ArtifactStore() = default;
  ArtifactStore(const ArtifactStore &) = delete;
  ArtifactStore &operator=(const ArtifactStore &) = delete;
  ArtifactStore(ArtifactStore &&) = delete;
  ArtifactStore &operator=(ArtifactStore &&) = delete;

  /// @brief True if an artefact exists at `p` (schema not checked).
  [[nodiscard]] virtual bool exists(const std::filesystem::path &p) const = 0;

  /// @brief mkdir -p.
  /// @errors io.
  [[nodiscard]] virtual Result<void> ensure_dir(const std::filesystem::path &dir) const = 0;

  /// @brief Artefact files directly under `dir`, sorted by name (empty if absent).
  [[nodiscard]] virtual std::vector<std::filesystem::path> list(
    const std::filesystem::path &dir
  ) const = 0;

  /// @brief Reads an artefact and checks its schema.
  /// @post  On success the returned value is the "data" payload only.
  /// @errors io if unreadable; parse if not an artefact envelope; schema if the
  ///         embedded schema differs from `expected`.
  [[nodiscard]] virtual Result<Json> load(
    const std::filesystem::path &p, Schema expected
  ) const = 0;

  /// @brief Writes `payload` wrapped in the envelope, atomically.
  /// @errors io.
  [[nodiscard]] virtual Result<void> save(
    const std::filesystem::path &p, Schema schema, const Json &payload
  ) const = 0;

  /// @brief Deletes an artefact; a missing file is not an error.
  /// @errors io.
  [[nodiscard]] virtual Result<void> remove(const std::filesystem::path &p) const = 0;

  /// @brief Typed convenience over load: converts the payload to T.
  /// @errors As load, plus parse if the payload does not convert to T.
  template <class T>
  [[nodiscard]] Result<T> load_as(const std::filesystem::path &p, Schema expected) const {
    ABISTUDY_TRY(Json j, load(p, expected));
    try {
      return j.get<T>();
    } catch (const Json::exception &e) {
      return fail(
        ErrorCode::parse, "'{}': payload does not match {}: {}", p.string(), expected.id, e.what()
      );
    }
  }
};

} // namespace abistudy::ports
