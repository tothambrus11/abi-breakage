#include "domain/events.hpp"

#include "core/contracts.hpp"

namespace abistudy {

TypeExposure parse_type_exposure(std::string_view s) noexcept {
  for (const auto e :
       {TypeExposure::by_value, TypeExposure::by_pointer, TypeExposure::not_in_interface}) {
    if (to_string(e) == s)
      return e;
  }
  return TypeExposure::unknown;
}

bool is_vague_linkage(std::string_view linkage_name, bool weak) noexcept {
  return weak && linkage_name.starts_with("_Z");
}

bool layout_event_breaks_leniently(const TypeEvent &e) noexcept {
  if (!is_layout_kind(e.kind))
    return true;
  const bool opaque_ok =
    e.exposure == TypeExposure::by_pointer || e.exposure == TypeExposure::not_in_interface;
  return !(opaque_ok && e.append_only);
}

namespace {
Language language_from_json_string(const std::string &s) noexcept {
  if (s == "c")
    return Language::c;
  if (s == "cxx")
    return Language::cxx;
  return Language::unknown;
}
ChangeKind kind_from_json(const nlohmann::json &j) {
  const auto k = parse_change_kind(j.get<std::string>());
  if (!k)
    throw nlohmann::json::other_error::create(501, "unknown change kind", &j);
  return *k;
}
} // namespace

void to_json(nlohmann::json &j, const Coverage &c) {
  j = {
    {"debug_info_found", {c.debug_info_found_1, c.debug_info_found_2}},
    {"exported_functions", {c.exported_functions_1, c.exported_functions_2}},
    {"mangled_functions", {c.mangled_functions_1, c.mangled_functions_2}}
  };
}
Coverage coverage_from_json(const nlohmann::json &j) {
  Coverage c;
  c.debug_info_found_1 = j.at("debug_info_found")[0];
  c.debug_info_found_2 = j.at("debug_info_found")[1];
  c.exported_functions_1 = j.at("exported_functions")[0];
  c.exported_functions_2 = j.at("exported_functions")[1];
  c.mangled_functions_1 = j.at("mangled_functions")[0];
  c.mangled_functions_2 = j.at("mangled_functions")[1];
  return c;
}

void to_json(nlohmann::json &j, const TypeEvent &e) {
  j = {{"kind", to_string(e.kind)},         {"type", e.type_name},
       {"declared_in", e.declared_in},      {"count", e.count},
       {"exposure", to_string(e.exposure)}, {"third_party", e.third_party},
       {"append_only", e.append_only}};
}
TypeEvent type_event_from_json(const nlohmann::json &j) {
  return TypeEvent{
    .kind = kind_from_json(j.at("kind")),
    .type_name = j.at("type").get<std::string>(),
    .declared_in = j.value("declared_in", std::string{}),
    .count = j.at("count").get<std::uint32_t>(),
    .exposure = parse_type_exposure(j.value("exposure", std::string{"unknown"})),
    .third_party = j.value("third_party", false),
    .append_only = j.value("append_only", false)
  };
}

void to_json(nlohmann::json &j, const SymbolEvent &e) {
  j = {{"kind", to_string(e.kind)}, {"symbol", e.symbol}, {"pretty", e.pretty}, {"weak", e.weak}};
  if (e.version)
    j["version"] = *e.version;
}
SymbolEvent symbol_event_from_json(const nlohmann::json &j) {
  SymbolEvent e{
    .kind = kind_from_json(j.at("kind")),
    .symbol = SymbolName{j.at("symbol").get<std::string>()},
    .pretty = j.value("pretty", std::string{}),
    .version = std::nullopt,
    .weak = j.value("weak", false)
  };
  if (j.contains("version"))
    e.version = VersionNode{j.at("version").get<std::string>()};
  return e;
}

void to_json(nlohmann::json &j, const SharedObjectDiff &d) {
  j = {
    {"soname", {d.soname_1, d.soname_2}},
    {"language", to_string(d.language)},
    {"public", d.public_counts},
    {"third_party", d.third_party_counts},
    {"private_node", d.private_node_counts},
    {"vague_linkage", d.vague_linkage_counts},
    {"symbols_version_renamed", d.symbols_version_renamed},
    {"mass_rename", d.mass_rename},
    {"coverage", d.coverage},
    {"type_events", d.type_events},
    {"symbol_events", d.symbol_events}
  };
}
SharedObjectDiff shared_object_diff_from_json(const nlohmann::json &j) {
  SharedObjectDiff d{
    .soname_1 = Soname{j.at("soname")[0].get<std::string>()},
    .soname_2 = Soname{j.at("soname")[1].get<std::string>()},
    .language = language_from_json_string(j.at("language").get<std::string>()),
    .public_counts = j.at("public").get<ChangeCounts>(),
    .third_party_counts = j.at("third_party").get<ChangeCounts>(),
    .private_node_counts = j.at("private_node").get<ChangeCounts>(),
    .vague_linkage_counts = j.value("vague_linkage", ChangeCounts{}),
    .symbols_version_renamed = j.at("symbols_version_renamed").get<std::uint32_t>(),
    .mass_rename = j.at("mass_rename").get<bool>(),
    .coverage = coverage_from_json(j.at("coverage")),
    .type_events = {},
    .symbol_events = {}
  };
  for (const auto &e : j.value("type_events", nlohmann::json::array()))
    d.type_events.push_back(type_event_from_json(e));
  for (const auto &e : j.value("symbol_events", nlohmann::json::array()))
    d.symbol_events.push_back(symbol_event_from_json(e));
  return d;
}

} // namespace abistudy
