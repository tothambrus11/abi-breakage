#include "ports/package_source.hpp"

namespace abistudy::ports {

Result<std::optional<FileHash>> amd64_deb_hash(
  const PackageSource &src, const BinaryName &bin, const VersionString &ver
) {
  ABISTUDY_TRY(auto files, src.binary_files(bin, ver));
  for (const auto *const want : {"amd64", "all"}) {
    for (const auto &f : files) {
      if (f.arch.get() == want)
        return std::optional{f.hash};
    }
  }
  return std::optional<FileHash>{};
}

} // namespace abistudy::ports
