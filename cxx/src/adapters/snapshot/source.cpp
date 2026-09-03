#include "adapters/snapshot/source.hpp"

#include <format>

namespace abistudy::snapshot {
namespace {

/// @brief Extracts the "result" array of an API answer.
/// @errors parse if absent or not an array.
Result<Json> result_array(Json j, std::string_view what) {
  if (!j.is_object() || !j.contains("result") || !j["result"].is_array())
    return fail(ErrorCode::parse, "{}: API answer has no 'result' array", what);
  return std::move(j["result"]);
}

template <class F>
Result<void> with_result(const Client &c, std::string path, const F &per_item) {
  ABISTUDY_TRY(Json j, c.api(path));
  ABISTUDY_TRY(Json arr, result_array(std::move(j), path));
  try {
    for (const auto &item : arr)
      per_item(item);
  } catch (const Json::exception &e) {
    return fail(ErrorCode::parse, "{}: unexpected item shape: {}", path, e.what());
  }
  return {};
}

} // namespace

Result<std::vector<DebianVersion>> SnapshotPackageSource::source_versions(
  const SourceName &src
) const {
  std::vector<DebianVersion> out;
  ABISTUDY_TRY_VOID(with_result(
    client_, std::format("/mr/package/{}/", url_encode_segment(src.get())), [&](const Json &item) {
      if (auto dv = DebianVersion::parse(item.at("version").get<std::string>()))
        out.push_back(std::move(*dv));
      // unparsable versions are dropped: they cannot be paired anyway
    }
  ));
  return out;
}

Result<std::vector<BinaryBuild>> SnapshotPackageSource::binary_packages(
  const SourceName &src, const VersionString &ver
) const {
  std::vector<BinaryBuild> out;
  ABISTUDY_TRY_VOID(with_result(
    client_,
    std::format(
      "/mr/package/{}/{}/binpackages", url_encode_segment(src.get()), url_encode_segment(ver.get())
    ),
    [&](const Json &item) {
      out.push_back(
        BinaryBuild{
          BinaryName{item.at("name").get<std::string>()},
          VersionString{item.at("version").get<std::string>()}
        }
      );
    }
  ));
  return out;
}

Result<std::optional<BinaryOrigin>> SnapshotPackageSource::binary_origin(
  const BinaryName &bin
) const {
  std::optional<BinaryOrigin> out;
  auto r = with_result(
    client_, std::format("/mr/binary/{}/", url_encode_segment(bin.get())), [&](const Json &item) {
      if (out)
        return; // first item is the newest
      out = BinaryOrigin{
        .source = SourceName{item.value("source", bin.get())},
        .newest = VersionString{item.at("version").get<std::string>()}
      };
    }
  );
  if (!r && r.error().code == ErrorCode::not_found)
    return std::optional<BinaryOrigin>{};
  if (!r)
    return forward_error(r);
  return out;
}

Result<std::vector<BinaryFile>> SnapshotPackageSource::binary_files(
  const BinaryName &bin, const VersionString &ver
) const {
  std::vector<BinaryFile> out;
  ABISTUDY_TRY_VOID(with_result(
    client_,
    std::format(
      "/mr/binary/{}/{}/binfiles", url_encode_segment(bin.get()), url_encode_segment(ver.get())
    ),
    [&](const Json &item) {
      out.push_back(
        BinaryFile{
          FileHash{item.at("hash").get<std::string>()},
          Architecture{item.at("architecture").get<std::string>()},
          item.value("name", std::string{})
        }
      );
    }
  ));
  return out;
}

Result<std::optional<std::uint64_t>> SnapshotPackageSource::file_size(const FileHash &hash) const {
  // The cached API answer is preferred over a HEAD request: sizes are then
  // part of the pinned corpus description and cost no network on a rerun.
  std::optional<std::uint64_t> size;
  auto r = with_result(client_, std::format("/mr/file/{}/info", hash.get()), [&](const Json &item) {
    if (!size && item.contains("size") && item.at("size").is_number())
      size = item.at("size").get<std::uint64_t>();
  });
  if (r && size)
    return size;
  return client_.content_length(hash);
}

Result<void> SnapshotPackageSource::download(
  const FileHash &hash, const std::filesystem::path &dest
) const {
  return client_.download(hash, dest);
}

Result<void> SnapshotPackageSource::fetch_to_file(
  std::string_view url, const std::filesystem::path &dest, std::chrono::seconds max_age
) const {
  return client_.fetch_to_file(url, dest, max_age);
}

} // namespace abistudy::snapshot
