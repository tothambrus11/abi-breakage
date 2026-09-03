#include "domain/records.hpp"

#include <format>

namespace abistudy {

Language language_from_string(std::string_view s) noexcept {
  if (s == "c")
    return Language::c;
  if (s == "cxx")
    return Language::cxx;
  return Language::unknown;
}

std::string PairJob::id() const {
  return std::format("{}@{}..{}", source, v1.upstream, v2.upstream);
}

namespace {
Declared declared_from_string(std::string_view s) noexcept {
  if (s == "declared")
    return Declared::yes;
  if (s == "undeclared")
    return Declared::no;
  return Declared::unknown;
}
} // namespace

// ---- SelectedLibrary / Selection --------------------------------------------

void to_json(Json &j, const SelectedLibrary &x) {
  j = {
    {"rank", x.rank},
    {"installs", x.installs},
    {"binary", x.binary},
    {"source", x.source},
    {"language_hint", to_string(x.language_hint)}
  };
}
SelectedLibrary selected_library_from_json(const Json &j) {
  return SelectedLibrary{
    .rank = j.at("rank").get<PopconRank>(),
    .installs = j.at("installs").get<InstallCount>(),
    .binary = j.at("binary").get<BinaryName>(),
    .source = j.at("source").get<SourceName>(),
    .language_hint = language_from_string(j.at("language_hint").get<std::string>())
  };
}
void to_json(Json &j, const SelectionProvenance &x) {
  j = {
    {"popcon_sha1", x.popcon_sha1},
    {"packages_sha1", x.packages_sha1},
    {"candidates_examined", x.candidates_examined},
    {"rejected_no_dbgsym", x.rejected_no_dbgsym},
    {"rejected_no_dev", x.rejected_no_dev},
    {"rejected_quota", x.rejected_quota}
  };
}
void from_json(const Json &j, SelectionProvenance &x) {
  x.popcon_sha1 = j.value("popcon_sha1", std::string{});
  x.packages_sha1 = j.value("packages_sha1", std::string{});
  x.candidates_examined = j.value("candidates_examined", 0U);
  x.rejected_no_dbgsym = j.value("rejected_no_dbgsym", 0U);
  x.rejected_no_dev = j.value("rejected_no_dev", 0U);
  x.rejected_quota = j.value("rejected_quota", 0U);
}
void to_json(Json &j, const Selection &x) {
  j = {{"libraries", x.libraries}, {"provenance", x.provenance}};
}
Selection selection_from_json(const Json &j) {
  return Selection{
    .libraries = j.at("libraries").get<std::vector<SelectedLibrary>>(),
    .provenance = j.value("provenance", SelectionProvenance{})
  };
}

// ---- Release / PairJob / Plan ------------------------------------------------

void to_json(Json &j, const BinaryVersions &x) { j = {{"name", x.name}, {"versions", x.versions}}; }
BinaryVersions binary_versions_from_json(const Json &j) {
  return BinaryVersions{
    .name = j.at("name").get<BinaryName>(),
    .versions = j.at("versions").get<std::vector<VersionString>>()
  };
}
void to_json(Json &j, const Release &x) {
  j = {{"upstream", x.upstream}, {"source_version", x.source_version},
       {"runtime", x.runtime},   {"dev", x.dev},
       {"binaries", x.binaries}, {"download_bytes", x.download_bytes}};
}
Release release_from_json(const Json &j) {
  return Release{
    .upstream = j.at("upstream").get<UpstreamVersion>(),
    .source_version = j.at("source_version").get<VersionString>(),
    .runtime = j.at("runtime").get<std::vector<BinaryName>>(),
    .dev = j.at("dev").get<std::vector<BinaryName>>(),
    .binaries = j.at("binaries").get<std::vector<BinaryVersions>>(),
    .download_bytes = j.value("download_bytes", std::uint64_t{0})
  };
}
void to_json(Json &j, const PairJob &x) { j = {{"source", x.source}, {"v1", x.v1}, {"v2", x.v2}}; }
PairJob pair_job_from_json(const Json &j) {
  return PairJob{
    .source = j.at("source").get<SourceName>(),
    .v1 = j.at("v1").get<Release>(),
    .v2 = j.at("v2").get<Release>()
  };
}
void to_json(Json &j, const Plan &x) { j = {{"jobs", x.jobs}}; }
Plan plan_from_json(const Json &j) { return Plan{j.at("jobs").get<std::vector<PairJob>>()}; }

// ---- PairResult ---------------------------------------------------------------

void to_json(Json &j, const PairResult &x) {
  j = {
    {"id", x.id},
    {"source", x.source},
    {"upstream", {x.upstream_1, x.upstream_2}},
    {"objects", x.objects},
    {"unpaired", {x.unpaired_1, x.unpaired_2}},
    {"object_errors", x.object_errors},
    {"seconds", x.seconds},
    {"bytes_extracted", x.bytes_extracted}
  };
  if (x.error)
    j["error"] = *x.error;
}
PairResult pair_result_from_json(const Json &j) {
  PairResult r{
    .id = j.at("id").get<std::string>(),
    .source = j.at("source").get<SourceName>(),
    .upstream_1 = UpstreamVersion{j.at("upstream")[0].get<std::string>()},
    .upstream_2 = UpstreamVersion{j.at("upstream")[1].get<std::string>()},
    .objects = {},
    .unpaired_1 = j.at("unpaired")[0].get<std::vector<std::string>>(),
    .unpaired_2 = j.at("unpaired")[1].get<std::vector<std::string>>(),
    .object_errors = j.value("object_errors", std::vector<std::string>{}),
    .error = std::nullopt,
    .seconds = j.value("seconds", 0.0),
    .bytes_extracted = j.value("bytes_extracted", std::uint64_t{0})
  };
  for (const auto &o : j.at("objects"))
    r.objects.push_back(shared_object_diff_from_json(o));
  if (j.contains("error"))
    r.error = j.at("error").get<std::string>();
  return r;
}

// ---- HeaderResult ---------------------------------------------------------------

void to_json(Json &j, const HeaderResult &x) {
  Json declared = Json::object();
  for (const auto &[sym, d] : x.symbol_declared)
    declared[sym] = to_string(d);
  j = {
    {"id", x.id},
    {"source", x.source},
    {"upstream", {x.upstream_1, x.upstream_2}},
    {"language", to_string(x.language)},
    {"coverage", {x.coverage_1, x.coverage_2}},
    {"symbol_declared", declared}
  };
  if (x.diff)
    j["diff"] = *x.diff;
  if (x.error)
    j["error"] = *x.error;
}
HeaderResult header_result_from_json(const Json &j) {
  HeaderResult r{
    .id = j.at("id").get<std::string>(),
    .source = j.at("source").get<SourceName>(),
    .upstream_1 = UpstreamVersion{j.at("upstream")[0].get<std::string>()},
    .upstream_2 = UpstreamVersion{j.at("upstream")[1].get<std::string>()},
    .language = language_from_string(j.at("language").get<std::string>()),
    .diff = std::nullopt,
    .coverage_1 = j.at("coverage")[0].get<ParseCoverage>(),
    .coverage_2 = j.at("coverage")[1].get<ParseCoverage>(),
    .error = std::nullopt,
    .symbol_declared = {}
  };
  if (j.contains("diff"))
    r.diff = j.at("diff").get<HeaderDiff>();
  if (j.contains("error"))
    r.error = j.at("error").get<std::string>();
  for (const auto &[sym, d] : j.value("symbol_declared", Json::object()).items())
    r.symbol_declared[sym] = declared_from_string(d.get<std::string>());
  return r;
}

} // namespace abistudy
