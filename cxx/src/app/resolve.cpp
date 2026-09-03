#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "app/materialize.hpp"
#include "app/stages.hpp"
#include "domain/symbols.hpp"
#include "domain/version.hpp"

namespace abistudy::app {
namespace {

constexpr std::array<std::string_view, 7> non_runtime_suffixes{"-dev",     "-doc",  "-dbg",
                                                               "-dbgsym",  "-udeb", "-tests",
                                                               "-examples"};
constexpr std::size_t max_runtime = 4;
constexpr std::size_t max_dev = 4;

/// @brief Runtime and dev package names for the popcon-selected library within
///        one source version. Generic rule: a runtime package is one with a
///        `-dbgsym` sibling, anchored to the selected binary in a version-blind
///        way so a mega-source (gcc-16: 166 candidates) contributes only the
///        library under study.
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
  const std::string target = digits_blind(want.get());
  std::vector<BinaryName> runtime;
  for (const auto &n : cands) {
    if (digits_blind(n) == target)
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
    if (digits_blind(n).starts_with(tprefix))
      preferred.emplace_back(n);
  }
  auto &dev = preferred.empty() ? devs : preferred;
  if (dev.size() > max_dev)
    dev.erase(dev.begin() + max_dev, dev.end());
  return {runtime, dev};
}

/// @brief Sum of the .deb sizes the diff stage will download for `rel`.
///        Unknown sizes count as zero; the estimate is for scheduling.
std::uint64_t download_bytes(const Services &sv, const Release &rel) {
  std::uint64_t total = 0;
  for (const auto &name : packages_for(rel, Want{})) {
    const auto it = std::ranges::find(rel.binaries, name, &BinaryVersions::name);
    if (it == rel.binaries.end() || it->versions.empty())
      continue;
    auto hash = ports::amd64_deb_hash(sv.packages, name, it->versions.front());
    if (!hash || !*hash)
      continue;
    if (auto size = sv.packages.file_size(**hash); size && *size)
      total += **size;
  }
  return total;
}

} // namespace

Result<Plan> run_resolve(const Workspace &ws, const Services &sv, const ResolveOptions &o) {
  ABISTUDY_TRY_VOID(ensure_workspace(ws, sv));
  ABISTUDY_TRY(Selection sel, sv.store.load_as<Selection>(ws.selection(), schema_selection));
  sv.log(std::format("{} selected sources", sel.libraries.size()));

  Plan plan;
  std::vector<Release *> to_size;
  for (const auto &lib : sel.libraries) {
    auto versions = sv.packages.source_versions(lib.source);
    if (!versions) {
      sv.log(std::format("{:<24} ERR {}", lib.source, versions.error().message));
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
      auto bins = sv.packages.binary_packages(lib.source, v.str());
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
        .binaries = {},
        .download_bytes = 0
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
    sv.log(
      std::format(
        "{:<24} releases={:<3} runtime={}", lib.source, picked.size(),
        picked.empty() ? std::string{"-"} : picked.back().runtime.front().get()
      )
    );
  }

  // Sizes, for scheduling and budgets. Every release appears in up to two
  // jobs; look each up once and copy.
  std::map<std::string, std::uint64_t> sizes;
  std::vector<std::pair<std::string, const Release *>> unique;
  for (const auto &j : plan.jobs) {
    for (const auto *r : {&j.v1, &j.v2}) {
      const auto key = j.source.get() + "@" + r->source_version.get();
      if (sizes.emplace(key, 0).second)
        unique.emplace_back(key, r);
    }
  }
  sv.log(std::format("looking up download sizes of {} releases", unique.size()));
  {
    std::atomic<std::size_t> next{0};
    std::mutex mx;
    auto worker = [&] {
      for (;;) {
        const auto k = next.fetch_add(1);
        if (k >= unique.size())
          return;
        const auto bytes = download_bytes(sv, *unique[k].second);
        const std::scoped_lock lk(mx);
        sizes[unique[k].first] = bytes;
      }
    };
    std::vector<std::jthread> pool;
    for (std::uint32_t t = 0; t < std::max<std::uint32_t>(1, o.size_threads); ++t)
      pool.emplace_back(worker);
  }
  for (auto &j : plan.jobs) {
    for (auto *r : {&j.v1, &j.v2})
      r->download_bytes = sizes[j.source.get() + "@" + r->source_version.get()];
  }
  ABISTUDY_TRY_VOID(sv.store.save(ws.plan(), schema_plan, plan));
  sv.log(std::format("{} consecutive-release pairs", plan.jobs.size()));
  return plan;
}

} // namespace abistudy::app
