#include "core/artifact.hpp"

#include <chrono>
#include <format>

#include "core/fs.hpp"

namespace abistudy {

Result<Json> parse_json(std::string_view text, std::string_view what) {
  try {
    return Json::parse(text);
  } catch (const Json::exception &e) {
    return fail(ErrorCode::parse, "{}: invalid JSON: {}", what, e.what());
  }
}

Result<Json> load_artifact(const std::filesystem::path &p, Schema expected) {
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

Result<void> save_artifact(const std::filesystem::path &p, Schema schema, const Json &payload) {
  Json env = {
    {"schema", schema.id},
    {"tool", tool_version()},
    {"generated_at", utc_now_iso8601()},
    {"data", payload},
  };
  return fs::write_file_atomic(p, env.dump(1));
}

std::string utc_now_iso8601() {
  const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  return std::format("{:%FT%TZ}", now);
}

} // namespace abistudy
