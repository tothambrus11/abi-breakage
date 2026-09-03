#include <algorithm>
#include <array>
#include <format>
#include <unordered_set>

#include "pipeline/stages.hpp"
#include "snapshot/api.hpp"
#include "snapshot/archive_index.hpp"

namespace abistudy::pipeline {
namespace {

constexpr std::array<std::string_view, 9> skip_suffixes{"-dev", "-dbg",    "-dbgsym",
                                                        "-doc", "-common", "-data",
                                                        "-bin", "-utils",  "-tools"};
constexpr std::array<std::string_view, 3> skip_prefixes{"libpython", "libperl", "libruby"};

bool candidate(const BinaryName &n) {
  const auto &s = n.get();
  if (!s.starts_with("lib")) {
    return false;
  }
  const bool side_package =
    std::ranges::any_of(skip_suffixes, [&](auto suf) { return s.ends_with(suf); });
  const bool binding =
    std::ranges::any_of(skip_prefixes, [&](auto pre) { return s.starts_with(pre); });
  return !side_package && !binding;
}

} // namespace

Result<void> Workspace::ensure() const {
  for (const auto &p : {cache(), scratch(), pairs(), header_indexes(), header_pairs()})
    ABISTUDY_TRY_VOID(fs::ensure_dir(p));
  return {};
}

Result<Selection> run_select(
  const Workspace &ws, const snapshot::Client &c, const SelectOptions &o, const Log &log
) {
  ABISTUDY_TRY_VOID(ws.ensure());
  const auto popcon = ws.cache() / "popcon.by_inst";
  const auto packages = ws.cache() / "Packages.xz";
  ABISTUDY_TRY_VOID(c.fetch_to_file(o.popcon_url, popcon, std::chrono::hours(24)));
  ABISTUDY_TRY_VOID(c.fetch_to_file(o.packages_url, packages, std::chrono::hours(24)));
  ABISTUDY_TRY(auto ranking, archive::parse_popcon(popcon));
  ABISTUDY_TRY(auto depends, archive::parse_packages_depends(packages));
  log(std::format("popcon rows: {}, archive packages: {}", ranking.size(), depends.size()));

  Selection sel;
  std::unordered_set<SourceName> seen;
  std::uint32_t n_c = 0;
  std::uint32_t n_cxx = 0;
  std::uint32_t examined = 0;
  for (const auto &row : ranking) {
    if (examined >= o.scan || (n_c >= o.c_limit && n_cxx >= o.cxx_limit))
      break;
    if (!candidate(row.name))
      continue;
    ++examined;
    const auto dep = depends.find(row.name);
    const Language lang =
      dep == depends.end() ? Language::c : archive::language_from_depends(dep->second);
    if (lang == Language::c && n_c >= o.c_limit)
      continue;
    if (lang == Language::cxx && n_cxx >= o.cxx_limit)
      continue;

    ABISTUDY_TRY(auto origin, snapshot::binary_origin(c, row.name));
    if (!origin || seen.contains(origin->source))
      continue;
    seen.insert(origin->source);
    auto bins = snapshot::binary_packages(c, origin->source, origin->newest);
    if (!bins) {
      log(std::format("  skip {}: {}", origin->source, bins.error().message));
      continue;
    }
    bool has_dbgsym = false;
    bool has_dev = false;
    for (const auto &b : *bins) {
      if (b.name.get() == row.name.get() + "-dbgsym")
        has_dbgsym = true;
      if (b.name.get().ends_with("-dev"))
        has_dev = true;
    }
    if (!has_dbgsym || !has_dev)
      continue;
    sel.libraries.push_back(
      SelectedLibrary{
        .rank = row.rank,
        .installs = row.installs,
        .binary = row.name,
        .source = origin->source,
        .language_hint = lang
      }
    );
    (lang == Language::cxx ? n_cxx : n_c)++;
    log(
      std::format(
        "  [{:>3}] {:<3} rank={:<5} {:<28} source={}", sel.libraries.size(), to_string(lang),
        row.rank, row.name, origin->source
      )
    );
  }
  ABISTUDY_TRY_VOID(save_artifact(ws.selection(), schema_selection, sel));
  log(std::format("selected {} sources: {} C, {} C++", sel.libraries.size(), n_c, n_cxx));
  return sel;
}

} // namespace abistudy::pipeline
