#include "adapters/fs/artifact_store.hpp"

#include <algorithm>
#include <chrono>
#include <format>

#include "core/fs.hpp"

namespace abistudy::fsstore {

Result<Json> parse_json(std::string_view text, std::string_view what) {
  try {
    return Json::parse(text);
  } catch (const Json::exception &e) {
    return fail(ErrorCode::parse, "{}: invalid JSON: {}", what, e.what());
  }
}

std::string utc_now_iso8601() {
  const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  return std::format("{:%FT%TZ}", now);
}

bool FsArtifactStore::exists(const std::filesystem::path &p) const {
  std::error_code ec;
  return std::filesystem::is_regular_file(p, ec);
}

Result<void> FsArtifactStore::ensure_dir(const std::filesystem::path &dir) const {
  return fs::ensure_dir(dir);
}

std::vector<std::filesystem::path> FsArtifactStore::list(const std::filesystem::path &dir) const {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  for (const auto &e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.path().extension() == ".json")
      out.push_back(e.path());
  }
  std::ranges::sort(out);
  return out;
}

Result<Json> FsArtifactStore::load(const std::filesystem::path &p, Schema expected) const {
  ABISTUDY_TRY(std::string text, fs::read_file(p));
  ABISTUDY_TRY(Json j, parse_json(text, p.string()));
  if (!j.is_object() || !j.contains("schema") || !j.contains("data")) {
    return fail(
      ErrorCode::parse, "'{}' is not an abistudy artefact (no schema/data envelope)", p.string()
    );
  }
  const auto got = j["schema"].get<std::string>();
  if (got != expected.id) {
    return fail(
      ErrorCode::schema, "'{}' has schema '{}', this stage needs '{}'", p.string(), got, expected.id
    );
  }
  return std::move(j["data"]);
}

Result<void> FsArtifactStore::save(
  const std::filesystem::path &p, Schema schema, const Json &payload
) const {
  const Json env = {
    {"schema", schema.id},
    {"tool", ports::tool_version()},
    {"generated_at", utc_now_iso8601()},
    {"provenance",
     {{"abi_reader", provenance_.abi_reader}, {"header_parser", provenance_.header_parser}}},
    {"data", payload},
  };
  return fs::write_file_atomic(p, env.dump(1));
}

} // namespace abistudy::fsstore
