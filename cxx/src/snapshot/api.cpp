#include "snapshot/api.hpp"

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

Result<std::vector<DebianVersion>> source_versions(const Client &c, const SourceName &src) {
  std::vector<DebianVersion> out;
  std::vector<std::string> bad;
  ABISTUDY_TRY_VOID(with_result(
    c, std::format("/mr/package/{}/", url_encode_segment(src.get())), [&](const Json &item) {
      const auto v = item.at("version").get<std::string>();
      if (auto dv = DebianVersion::parse(v))
        out.push_back(std::move(*dv));
      else
        bad.push_back(v);
    }
  ));
  return out; // unparsable versions are dropped: they cannot be paired anyway
}

Result<std::vector<BinaryBuild>> binary_packages(
  const Client &c, const SourceName &src, const VersionString &ver
) {
  std::vector<BinaryBuild> out;
  ABISTUDY_TRY_VOID(with_result(
    c,
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

Result<std::optional<BinaryOrigin>> binary_origin(const Client &c, const BinaryName &bin) {
  std::optional<BinaryOrigin> out;
  auto r = with_result(
    c, std::format("/mr/binary/{}/", url_encode_segment(bin.get())), [&](const Json &item) {
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

Result<std::vector<BinaryFile>> binary_files(
  const Client &c, const BinaryName &bin, const VersionString &ver
) {
  std::vector<BinaryFile> out;
  ABISTUDY_TRY_VOID(with_result(
    c,
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

Result<std::optional<FileHash>> amd64_deb_hash(
  const Client &c, const BinaryName &bin, const VersionString &ver
) {
  ABISTUDY_TRY(auto files, binary_files(c, bin, ver));
  for (const auto *const want : {"amd64", "all"}) {
    for (const auto &f : files) {
      if (f.arch.get() == want)
        return std::optional{f.hash};
    }
  }
  return std::optional<FileHash>{};
}

} // namespace abistudy::snapshot
