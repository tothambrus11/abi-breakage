#pragma once
// =============================================================================
// ArtifactStore adapter: JSON envelopes on the local filesystem, written
// atomically. Also the small JSON/time helpers other adapters share.
// =============================================================================

#include <filesystem>
#include <string>
#include <string_view>

#include "ports/artifact_store.hpp"

namespace abistudy::fsstore {

/// @brief Parses a JSON document from text.
/// @errors parse if the text is not valid JSON.
[[nodiscard]] Result<Json> parse_json(std::string_view text, std::string_view what);

/// @brief Current UTC time as ISO-8601 ("2026-09-02T14:03:11Z").
[[nodiscard]] std::string utc_now_iso8601();

class FsArtifactStore final : public ports::ArtifactStore {
public:
  explicit FsArtifactStore(ports::Provenance provenance) : provenance_(std::move(provenance)) {}

  [[nodiscard]] bool exists(const std::filesystem::path &p) const override;
  [[nodiscard]] Result<void> ensure_dir(const std::filesystem::path &dir) const override;
  [[nodiscard]] std::vector<std::filesystem::path> list(
    const std::filesystem::path &dir
  ) const override;
  [[nodiscard]] Result<Json> load(const std::filesystem::path &p, Schema expected) const override;
  [[nodiscard]] Result<void> save(
    const std::filesystem::path &p, Schema schema, const Json &payload
  ) const override;

private:
  ports::Provenance provenance_;
};

} // namespace abistudy::fsstore
