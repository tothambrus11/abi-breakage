#pragma once
// =============================================================================
// ArtifactStore adapter: JSON envelopes on the local filesystem, written
// atomically.
// =============================================================================

#include <filesystem>
#include <string>
#include <string_view>

#include "ports/artifact_store.hpp"

namespace abistudy::fsstore {

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
  [[nodiscard]] Result<void> remove(const std::filesystem::path &p) const override;

private:
  ports::Provenance provenance_;
};

} // namespace abistudy::fsstore
