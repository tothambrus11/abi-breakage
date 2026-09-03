#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <regex>
#include <set>

#include "core/version.hpp"
#include "pipeline/stages.hpp"
#include "snapshot/api.hpp"

namespace abistudy::pipeline {
namespace {

constexpr std::array<std::string_view, 7> non_runtime_suffixes{"-dev",     "-doc",  "-dbg",
                                                               "-dbgsym",  "-udeb", "-tests",
                                                               "-examples"};
constexpr std::size_t max_runtime = 4, max_dev = 4;

/// @brief Version-blind package identity: "libicu72" and "libicu73" agree.
std::string norm_pkg(std::string_view n) {
  std::string out;
  bool in_digits = false;
  for (const char c : n) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      if (!in_digits)
        out.push_back('#');
      in_digits = true;
    } else {
      out.push_back(c);
      in_digits = false;
    }
  }
  return out;
}

/// @brief Runtime and dev package names for the popcon-selected library within
///        one source version. Generic rule: a runtime package is one with a
///        `-dbgsym` sibling, anchored to the selected binary so a mega-source
///        (gcc-16: 166 candidates) contributes only the library under study.
std::pair<std::vector<BinaryName>, std::vector<BinaryName>> roles(
  const std::set<std::string> &names, const BinaryName &want
) {
  std::vector<std::string> cands;
  for (const auto &n : names) {
    if (std::ranges::any_of(non_runtime_suffixes, [&](auto s) { return n.ends_with(s); }))
      continue;
    if (names.contains(n + "-dbgsym"))
      cands.push_back(n);
  }
  const std::string target = norm_pkg(want.get());
  std::vector<BinaryName> runtime;
  for (const auto &n : cands) {
    if (norm_pkg(n) == target)
      runtime.emplace_back(n);
  }
  if (runtime.empty()) { // renamed beyond digits (t64 transition etc.)
    std::string stem = want.get();
    while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back())))
      stem.pop_back();
    while (!stem.empty() && stem.back() == '-')
      stem.pop_back();
    for (const auto &n : cands) {
      if (n.starts_with(stem))
        runtime.emplace_back(n);
    }
  }
  if (runtime.size() > max_runtime)
    runtime.erase(runtime.begin() + max_runtime, runtime.end());

  std::vector<BinaryName> devs;
  std::vector<BinaryName> preferred;
  std::string tprefix = target;
  while (!tprefix.empty() && tprefix.back() == '#')
    tprefix.pop_back();
  for (const auto &n : names) {
    if (!n.ends_with("-dev"))
      continue;
    devs.emplace_back(n);
    if (norm_pkg(n).starts_with(tprefix))
      preferred.emplace_back(n);
  }
  auto &dev = preferred.empty() ? devs : preferred;
  if (dev.size() > max_dev)
    dev.erase(dev.begin() + max_dev, dev.end());
  return {runtime, dev};
}

} // namespace

Result<Plan> run_resolve(
  const Workspace &ws, const snapshot::Client &c, const ResolveOptions &o, const Log &log
) {
  ABISTUDY_TRY_VOID(ws.ensure());
  ABISTUDY_TRY(Selection sel, load_artifact_as<Selection>(ws.selection(), schema_selection));
  log(std::format("{} selected sources", sel.libraries.size()));

  Plan plan;
  for (const auto &lib : sel.libraries) {
    auto versions = snapshot::source_versions(c, lib.source);
    if (!versions) {
      log(std::format("{:<24} ERR {}", lib.source, versions.error().message));
      continue;
    }
    std::ranges::sort(*versions, std::greater<>{}); // newest first, dpkg order

    // One archive version per upstream release: the newest Debian revision.
    std::vector<DebianVersion> per_upstream;
    std::set<std::string> seen_up;
    for (const auto &v : *versions) {
      if (v.is_prerelease())
        continue;
      if (seen_up.insert(v.upstream().get()).second)
        per_upstream.push_back(v);
    }

    std::vector<Release> picked;
    std::uint32_t scanned = 0;
    for (const auto &v : per_upstream) {
      if (picked.size() >= o.releases || scanned++ >= o.max_scan)
        break;
      auto bins = snapshot::binary_packages(c, lib.source, v.str());
      if (!bins)
        continue;
      std::map<std::string, std::vector<VersionString>> by_name;
      std::set<std::string> names;
      for (const auto &b : *bins) {
        by_name[b.name.get()].push_back(b.version);
        names.insert(b.name.get());
      }
      auto [rt, dev] = roles(names, lib.binary);
      if (rt.empty() || dev.empty())
        continue;
      std::set<std::string> keep;
      for (const auto &r : rt) {
        keep.insert(r.get());
        keep.insert(r.get() + "-dbgsym");
      }
      for (const auto &d : dev)
        keep.insert(d.get());
      Release rel{
        .upstream = v.upstream(),
        .source_version = v.str(),
        .runtime = rt,
        .dev = dev,
        .binaries = {}
      };
      for (const auto &k : keep) {
        if (auto it = by_name.find(k); it != by_name.end()) {
          auto vs = it->second;
          std::ranges::sort(vs, [](const VersionString &a, const VersionString &b) {
            auto da = DebianVersion::parse(a.get());
            auto db = DebianVersion::parse(b.get());
            return da && db ? *da > *db : a > b;
          });
          rel.binaries.push_back(BinaryVersions{.name = BinaryName{k}, .versions = vs});
        }
      }
      picked.push_back(std::move(rel));
    }
    std::ranges::reverse(picked); // oldest -> newest
    for (std::size_t i = 1; i < picked.size(); ++i)
      plan.jobs.push_back(PairJob{.source = lib.source, .v1 = picked[i - 1], .v2 = picked[i]});
    log(
      std::format(
        "{:<24} releases={:<3} runtime={}", lib.source, picked.size(),
        picked.empty() ? std::string{"-"} : picked.back().runtime.front().get()
      )
    );
  }
  ABISTUDY_TRY_VOID(save_artifact(ws.plan(), schema_plan, plan));
  log(std::format("{} consecutive-release pairs", plan.jobs.size()));
  return plan;
}

} // namespace abistudy::pipeline
