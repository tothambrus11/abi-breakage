#include <algorithm>
#include <array>
#include <format>
#include <unordered_set>

#include "adapters/snapshot/archive_index.hpp"
#include "app/stages.hpp"
#include "core/fs.hpp"
#include "core/hash.hpp"

namespace abistudy::app {
namespace {

constexpr std::array<std::string_view, 9> skip_suffixes{"-dev", "-dbg",    "-dbgsym",
                                                        "-doc", "-common", "-data",
                                                        "-bin", "-utils",  "-tools"};
constexpr std::array<std::string_view, 3> skip_prefixes{"libpython", "libperl", "libruby"};

bool candidate(const BinaryName &n) {
  const auto &s = n.get();
  if (!s.starts_with("lib"))
    return false;
  const bool side_package =
    std::ranges::any_of(skip_suffixes, [&](auto suf) { return s.ends_with(suf); });
  const bool binding =
    std::ranges::any_of(skip_prefixes, [&](auto pre) { return s.starts_with(pre); });
  return !side_package && !binding;
}

std::string sha1_of_file(const std::filesystem::path &p) {
  const auto text = fs::read_file(p);
  return text ? sha1_hex(*text) : std::string{};
}

} // namespace

Result<void> ensure_workspace(const Workspace &ws, const Services &sv) {
  for (const auto &p :
       {ws.cache(), ws.scratch(), ws.pairs(), ws.header_indexes(), ws.header_pairs()})
    ABISTUDY_TRY_VOID(sv.store.ensure_dir(p));
  return {};
}

Result<Selection> run_select(const Workspace &ws, const Services &sv, const SelectOptions &o) {
  ABISTUDY_TRY_VOID(ensure_workspace(ws, sv));
  const auto popcon = ws.cache() / "popcon.by_inst";
  const auto packages = ws.cache() / "Packages.xz";
  ABISTUDY_TRY_VOID(sv.packages.fetch_to_file(o.popcon_url, popcon, std::chrono::hours(24)));
  ABISTUDY_TRY_VOID(sv.packages.fetch_to_file(o.packages_url, packages, std::chrono::hours(24)));
  ABISTUDY_TRY(auto ranking, archive::parse_popcon(popcon));
  ABISTUDY_TRY(auto depends, archive::parse_packages_depends(packages));
  sv.log(std::format("popcon rows: {}, archive packages: {}", ranking.size(), depends.size()));

  Selection sel;
  sel.provenance.popcon_sha1 = sha1_of_file(popcon);
  sel.provenance.packages_sha1 = sha1_of_file(packages);
  std::unordered_set<SourceName> seen;
  std::uint32_t n_c = 0;
  std::uint32_t n_cxx = 0;
  for (const auto &row : ranking) {
    if (sel.provenance.candidates_examined >= o.scan || (n_c >= o.c_limit && n_cxx >= o.cxx_limit))
      break;
    if (!candidate(row.name))
      continue;
    ++sel.provenance.candidates_examined;
    const auto dep = depends.find(row.name);
    const Language lang = dep == depends.end() ? Language::c : language_from_depends(dep->second);
    if (
      (lang == Language::c && n_c >= o.c_limit) || (lang == Language::cxx && n_cxx >= o.cxx_limit)
    ) {
      ++sel.provenance.rejected_quota;
      continue;
    }

    ABISTUDY_TRY(auto origin, sv.packages.binary_origin(row.name));
    if (!origin || seen.contains(origin->source))
      continue;
    seen.insert(origin->source);
    auto bins = sv.packages.binary_packages(origin->source, origin->newest);
    if (!bins) {
      sv.log(std::format("  skip {}: {}", origin->source, bins.error().message));
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
    if (!has_dbgsym) {
      ++sel.provenance.rejected_no_dbgsym;
      continue;
    }
    if (!has_dev) {
      ++sel.provenance.rejected_no_dev;
      continue;
    }
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
    sv.log(
      std::format(
        "  [{:>3}] {:<3} rank={:<5} {:<28} source={}", sel.libraries.size(), to_string(lang),
        row.rank, row.name, origin->source
      )
    );
  }
  ABISTUDY_TRY_VOID(sv.store.save(ws.selection(), schema_selection, sel));
  sv.log(
    std::format(
      "selected {} sources: {} C, {} C++ (examined {}, no dbgsym {}, no dev {})",
      sel.libraries.size(), n_c, n_cxx, sel.provenance.candidates_examined,
      sel.provenance.rejected_no_dbgsym, sel.provenance.rejected_no_dev
    )
  );
  return sel;
}

} // namespace abistudy::app
