#include "domain/header_model.hpp"

#include <regex>

#include "core/contracts.hpp"

namespace abistudy {

bool looks_like_version_macro(std::string_view key) {
  static const std::regex stamp(
    "(VERSION|_DATE|BUILD|REVISION|RELEASE|PATCHLEVEL|COMMIT|GIT|SVN|HASH|_YEAR|_MONTH|TIMESTAMP|"
    "COPYRIGHT|MAJOR|MINOR|MICRO|_NUM$|_STR$)",
    std::regex::icase
  );
  const auto sep = key.rfind("::");
  const auto name = key.substr(sep == std::string_view::npos ? 0 : sep + 2);
  return std::regex_search(name.begin(), name.end(), stamp);
}

HeaderDiff compare_headers(const HeaderIndex &a, const HeaderIndex &b) {
  HeaderDiff d;
  d.definitions_1 = static_cast<std::uint32_t>(a.definitions.size());
  d.definitions_2 = static_cast<std::uint32_t>(b.definitions.size());
  for (const auto &[usr, da] : a.definitions) {
    const auto it = b.definitions.find(usr);
    if (it == b.definitions.end()) {
      ++d.definitions_removed;
      continue;
    }
    ++d.definitions_common;
    const auto &db = it->second;
    if (da.body_fingerprint != db.body_fingerprint) {
      ++d.inline_body_changed;
      if (da.kind.contains("Template"))
        ++d.inline_body_changed_template;
      if (d.examples.size() < 8)
        d.examples.push_back(da.relative_path + "::" + da.name);
    }
    if (da.decl_fingerprint != db.decl_fingerprint)
      ++d.inline_decl_changed;
  }
  for (const auto &[usr, db] : b.definitions) {
    if (!a.definitions.contains(usr))
      ++d.definitions_added;
  }

  d.macros_1 = static_cast<std::uint32_t>(a.macros.size());
  d.macros_2 = static_cast<std::uint32_t>(b.macros.size());
  for (const auto &[k, va] : a.macros) {
    const auto it = b.macros.find(k);
    if (it == b.macros.end())
      continue;
    ++d.macros_common;
    if (it->second != va) {
      ++d.macro_value_changed;
      if (!looks_like_version_macro(k))
        ++d.macro_value_changed_nonversion;
    }
  }
  ABISTUDY_ENSURES(d.inline_body_changed <= d.definitions_common);
  ABISTUDY_ENSURES(d.macro_value_changed <= d.macros_common);
  return d;
}

Declared symbol_declared(const HeaderIndex &idx, std::string_view symbol) {
  if (idx.declared_symbols.empty() || idx.coverage.poor())
    return Declared::unknown;
  return idx.declared_symbols.contains(std::string{symbol}) ? Declared::yes : Declared::no;
}

// ----------------------------------------------------------------------------
// JSON
// ----------------------------------------------------------------------------

void to_json(nlohmann::json &j, const Definition &x) {
  j = {
    {"path", x.relative_path},
    {"name", x.name},
    {"kind", x.kind},
    {"decl", x.decl_fingerprint},
    {"body", x.body_fingerprint},
    {"tokens", x.body_tokens}
  };
}
void from_json(const nlohmann::json &j, Definition &x) {
  x.relative_path = j.at("path");
  x.name = j.at("name");
  x.kind = j.at("kind");
  x.decl_fingerprint = j.at("decl");
  x.body_fingerprint = j.at("body");
  x.body_tokens = j.at("tokens");
}
void to_json(nlohmann::json &j, const ParseCoverage &c) {
  j = {{"header_files", c.header_files},         {"parsed", c.parsed},
       {"with_fatal_error", c.with_fatal_error}, {"with_errors", c.with_errors},
       {"parsed_as_cxx", c.parsed_as_cxx},       {"skipped_by_limit", c.skipped_by_limit}};
}
void from_json(const nlohmann::json &j, ParseCoverage &c) {
  c.header_files = j.at("header_files");
  c.parsed = j.at("parsed");
  c.with_fatal_error = j.at("with_fatal_error");
  c.with_errors = j.value("with_errors", 0U);
  c.parsed_as_cxx = j.value("parsed_as_cxx", 0U);
  c.skipped_by_limit = j.at("skipped_by_limit");
}
void to_json(nlohmann::json &j, const HeaderIndex &x) {
  j = {
    {"language", to_string(x.language)},
    {"definitions", x.definitions},
    {"macros", x.macros},
    {"declared_symbols", x.declared_symbols},
    {"coverage", x.coverage}
  };
}
void from_json(const nlohmann::json &j, HeaderIndex &x) {
  const auto l = j.at("language").get<std::string>();
  if (l == "c") {
    x.language = Language::c;
  } else if (l == "cxx") {
    x.language = Language::cxx;
  } else {
    x.language = Language::unknown;
  }
  x.definitions = j.at("definitions").get<std::unordered_map<std::string, Definition>>();
  x.macros = j.at("macros").get<std::map<std::string, std::string>>();
  x.declared_symbols =
    j.value("declared_symbols", nlohmann::json::array()).get<std::unordered_set<std::string>>();
  x.coverage = j.at("coverage").get<ParseCoverage>();
}
void to_json(nlohmann::json &j, const HeaderDiff &d) {
  j = {
    {"definitions", {d.definitions_1, d.definitions_2}},
    {"definitions_common", d.definitions_common},
    {"inline_body_changed", d.inline_body_changed},
    {"inline_body_changed_template", d.inline_body_changed_template},
    {"inline_decl_changed", d.inline_decl_changed},
    {"definitions_added", d.definitions_added},
    {"definitions_removed", d.definitions_removed},
    {"macros", {d.macros_1, d.macros_2}},
    {"macros_common", d.macros_common},
    {"macro_value_changed", d.macro_value_changed},
    {"macro_value_changed_nonversion", d.macro_value_changed_nonversion},
    {"examples", d.examples}
  };
}
void from_json(const nlohmann::json &j, HeaderDiff &d) {
  d.definitions_1 = j.at("definitions")[0];
  d.definitions_2 = j.at("definitions")[1];
  d.definitions_common = j.at("definitions_common");
  d.inline_body_changed = j.at("inline_body_changed");
  d.inline_body_changed_template = j.at("inline_body_changed_template");
  d.inline_decl_changed = j.at("inline_decl_changed");
  d.definitions_added = j.at("definitions_added");
  d.definitions_removed = j.at("definitions_removed");
  d.macros_1 = j.at("macros")[0];
  d.macros_2 = j.at("macros")[1];
  d.macros_common = j.at("macros_common");
  d.macro_value_changed = j.at("macro_value_changed");
  d.macro_value_changed_nonversion =
    j.value("macro_value_changed_nonversion", d.macro_value_changed);
  d.examples = j.at("examples").get<std::vector<std::string>>();
}

} // namespace abistudy
