#include <algorithm>
#include <format>

#include "hdr/index.hpp"
#include "pipeline/materialize.hpp"
#include "pipeline/stages.hpp"

namespace abistudy::pipeline {
namespace {

std::string index_stem(const SourceName &s, const VersionString &v, Language l) {
  std::string ver = v.get();
  std::ranges::replace(ver, '/', '_');
  std::ranges::replace(ver, ':', '%');
  return std::format("{}@{}.{}", s, ver, to_string(l));
}

/// @brief Loads a release's header index, building it from a fresh download
///        of the -dev packages if the diff stage did not leave one behind.
Result<hdr::HeaderIndex> get_index(
  const Workspace &ws, const snapshot::Client &c, const SourceName &src, const Release &rel,
  Language lang, std::uint32_t max_files, const Log &log
) {
  const auto p = ws.header_indexes() / (index_stem(src, rel.source_version, lang) + ".json");
  std::error_code ec;
  if (std::filesystem::exists(p, ec)) {
    auto cached = load_artifact_as<hdr::HeaderIndex>(p, schema_header_index);
    if (cached || cached.error().code != ErrorCode::schema) {
      return cached;
    }
    // Schema 1 indexes parsed every header in the library's language; for a
    // C++ library that is exactly what schema 2 does, so they remain valid.
    if (lang == Language::cxx) {
      if (auto old = load_artifact_as<hdr::HeaderIndex>(p, Schema{"abistudy/header-index/1"})) {
        return old;
      }
    }
    log(std::format("    rebuilding {} (index schema changed)", p.filename().string()));
  }
  ABISTUDY_TRY(Materialized m, materialize(c, rel, Want{false, false, true}, ws.scratch()));
  if (m.include_root.empty()) {
    hdr::HeaderIndex empty;
    empty.language = lang;
    ABISTUDY_TRY_VOID(save_artifact(p, schema_header_index, empty));
    return empty;
  }
  hdr::Options o;
  o.language = lang;
  o.max_files = max_files;
  ABISTUDY_TRY(auto idx, hdr::index(m.include_root, o));
  log(
    std::format(
      "    indexed {} {}: {} defs, {} macros, {}/{} parsed", src, rel.upstream,
      idx.definitions.size(), idx.macros.size(), idx.coverage.parsed, idx.coverage.header_files
    )
  );
  ABISTUDY_TRY_VOID(save_artifact(p, schema_header_index, idx));
  return idx;
}

/// @brief Language of a pair as decided by the diff stage; C++ if unknown.
Language pair_language(const Workspace &ws, const PairJob &job) {
  auto pr = load_artifact_as<PairResult>(ws.pair(job.id()), schema_pair);
  if (!pr)
    return Language::cxx;
  Language l = Language::unknown;
  for (const auto &o : pr->objects) {
    if (o.language == Language::cxx)
      return Language::cxx;
    if (l == Language::unknown)
      l = o.language;
  }
  return l == Language::unknown ? Language::cxx : l;
}

} // namespace

Result<void> run_headers(
  const Workspace &ws, const snapshot::Client &c, const HeadersOptions &o, const Log &log
) {
  ABISTUDY_TRY_VOID(ws.ensure());
  ABISTUDY_TRY(Plan plan, load_artifact_as<Plan>(ws.plan(), schema_plan));
  std::size_t n = 0;
  for (const auto &job : plan.jobs) {
    ++n;
    std::error_code ec;
    if (std::filesystem::exists(ws.header_pair(job.id()), ec))
      continue;
    const Language lang = pair_language(ws, job);
    HeaderResult hr{
      .id = job.id(),
      .source = job.source,
      .upstream_1 = job.v1.upstream,
      .upstream_2 = job.v2.upstream,
      .language = lang,
      .diff = std::nullopt,
      .coverage_1 = {},
      .coverage_2 = {},
      .error = std::nullopt
    };
    auto a = get_index(ws, c, job.source, job.v1, lang, o.max_files, log);
    auto b = a ? get_index(ws, c, job.source, job.v2, lang, o.max_files, log)
               : Result<hdr::HeaderIndex>{std::unexpected{a.error()}};
    if (!a || !b) {
      hr.error = (a ? b : a).error().message;
    } else {
      hr.diff = hdr::compare(*a, *b);
      hr.coverage_1 = a->coverage;
      hr.coverage_2 = b->coverage;
    }
    ABISTUDY_TRY_VOID(save_artifact(ws.header_pair(hr.id), schema_header_pair, hr));
    if (n % 25 == 0 || hr.error) {
      log(
        std::format(
          "[{}/{}] {} bodies={} macros={}{}", n, plan.jobs.size(), hr.id,
          hr.diff ? hr.diff->inline_body_changed : 0, hr.diff ? hr.diff->macro_value_changed : 0,
          hr.error ? " ERR " + *hr.error : ""
        )
      );
    }
  }
  log("headers stage complete");
  return {};
}

} // namespace abistudy::pipeline
