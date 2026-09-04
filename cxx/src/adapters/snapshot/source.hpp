#pragma once
// =============================================================================
// PackageSource adapter over the snapshot.debian.org machine-readable API.
// Each method maps one endpoint to domain types; nothing above sees JSON.
// Endpoint reference: https://snapshot.debian.org/ ("machine-readable API").
// =============================================================================

#include "adapters/snapshot/client.hpp"
#include "ports/package_source.hpp"

namespace abistudy::snapshot {

class SnapshotPackageSource final : public ports::PackageSource {
public:
  explicit SnapshotPackageSource(Client client) : client_(std::move(client)) {}

  [[nodiscard]] Result<std::vector<DebianVersion>> source_versions(
    const SourceName &src
  ) const override;
  [[nodiscard]] Result<std::vector<BinaryBuild>> binary_packages(
    const SourceName &src, const VersionString &ver
  ) const override;
  [[nodiscard]] Result<std::optional<BinaryOrigin>> binary_origin(
    const BinaryName &bin
  ) const override;
  [[nodiscard]] Result<std::vector<BinaryFile>> binary_files(
    const BinaryName &bin, const VersionString &ver
  ) const override;
  [[nodiscard]] Result<std::optional<std::uint64_t>> file_size(const FileHash &hash) const override;
  [[nodiscard]] Result<void> download(
    const FileHash &hash, const std::filesystem::path &dest
  ) const override;
  [[nodiscard]] Result<void> fetch_to_file(
    std::string_view url, const std::filesystem::path &dest, std::chrono::seconds max_age
  ) const override;

private:
  Client client_;
};

} // namespace abistudy::snapshot
