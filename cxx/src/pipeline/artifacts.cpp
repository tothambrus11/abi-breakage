#include "pipeline/artifacts.hpp"

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
void to_json(Json &j, const Selection &x) { j = {{"libraries", x.libraries}}; }
Selection selection_from_json(const Json &j) {
  return Selection{j.at("libraries").get<std::vector<SelectedLibrary>>()};
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
  j = {
    {"upstream", x.upstream},
    {"source_version", x.source_version},
    {"runtime", x.runtime},
    {"dev", x.dev},
    {"binaries", x.binaries}
  };
}
Release release_from_json(const Json &j) {
  return Release{
    .upstream = j.at("upstream").get<UpstreamVersion>(),
    .source_version = j.at("source_version").get<VersionString>(),
    .runtime = j.at("runtime").get<std::vector<BinaryName>>(),
    .dev = j.at("dev").get<std::vector<BinaryName>>(),
    .binaries = j.at("binaries").get<std::vector<BinaryVersions>>()
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

namespace {
abi::SharedObjectDiff shared_object_diff_from_json(const Json &j) {
  abi::SharedObjectDiff d{
    .soname_1 = Soname{j.at("soname")[0].get<std::string>()},
    .soname_2 = Soname{j.at("soname")[1].get<std::string>()},
    .language = language_from_string(j.at("language").get<std::string>()),
    .public_counts = j.at("public").get<ChangeCounts>(),
    .third_party_counts = j.at("third_party").get<ChangeCounts>(),
    .private_node_counts = j.at("private_node").get<ChangeCounts>(),
    .symbols_version_renamed = j.at("symbols_version_renamed").get<std::uint32_t>(),
    .mass_rename = j.at("mass_rename").get<bool>(),
    .coverage = {},
    .type_events = {},
    .symbol_events = {}
  };
  const auto &c = j.at("coverage");
  d.coverage.debug_info_found_1 = c.at("debug_info_found")[0];
  d.coverage.debug_info_found_2 = c.at("debug_info_found")[1];
  d.coverage.exported_functions_1 = c.at("exported_functions")[0];
  d.coverage.exported_functions_2 = c.at("exported_functions")[1];
  d.coverage.mangled_functions_1 = c.at("mangled_functions")[0];
  d.coverage.mangled_functions_2 = c.at("mangled_functions")[1];
  // Event lists are write-only detail for reviewers; analysis uses the tallies.
  return d;
}
} // namespace

void to_json(Json &j, const PairResult &x) {
  j = {
    {"id", x.id},
    {"source", x.source},
    {"upstream", {x.upstream_1, x.upstream_2}},
    {"objects", x.objects},
    {"unpaired", {x.unpaired_1, x.unpaired_2}},
    {"object_errors", x.object_errors},
    {"seconds", x.seconds}
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
    .unpaired_1 = {},
    .unpaired_2 = {},
    .object_errors = {},
    .error = std::nullopt,
    .seconds = 0
  };
  for (const auto &o : j.at("objects"))
    r.objects.push_back(shared_object_diff_from_json(o));
  r.unpaired_1 = j.at("unpaired")[0].get<std::vector<std::string>>();
  r.unpaired_2 = j.at("unpaired")[1].get<std::vector<std::string>>();
  r.object_errors = j.value("object_errors", std::vector<std::string>{});
  r.object_errors = j.value("object_errors", std::vector<std::string>{});
  if (j.contains("error"))
    r.error = j.at("error").get<std::string>();
  r.seconds = j.value("seconds", 0.0);
  return r;
}

// ---- HeaderResult ---------------------------------------------------------------

void to_json(Json &j, const HeaderResult &x) {
  j = {
    {"id", x.id},
    {"source", x.source},
    {"upstream", {x.upstream_1, x.upstream_2}},
    {"language", to_string(x.language)},
    {"coverage", {x.coverage_1, x.coverage_2}}
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
    .coverage_1 = {},
    .coverage_2 = {},
    .error = std::nullopt
  };
  if (j.contains("diff"))
    r.diff = j.at("diff").get<hdr::HeaderDiff>();
  r.coverage_1 = j.at("coverage")[0].get<hdr::ParseCoverage>();
  r.coverage_2 = j.at("coverage")[1].get<hdr::ParseCoverage>();
  if (j.contains("error"))
    r.error = j.at("error").get<std::string>();
  return r;
}

} // namespace abistudy
