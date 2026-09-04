#include "domain/statistics.hpp"

#include <algorithm>
#include <random>

#include "core/contracts.hpp"

namespace abistudy {
namespace {

Interval percentile_95(std::vector<double> stats) {
  ABISTUDY_EXPECTS(!stats.empty()); // BootstrapOptions::resamples >= 1
  std::ranges::sort(stats);
  const auto n = stats.size();
  auto at = [&](double q) {
    const auto idx = static_cast<std::size_t>(q * static_cast<double>(n - 1));
    return stats[std::min(idx, n - 1)];
  };
  return Interval{.lo = at(0.025), .hi = at(0.975)};
}

} // namespace

double median(std::vector<std::uint32_t> v) {
  if (v.empty())
    return 0;
  std::ranges::sort(v);
  const auto n = v.size();
  return n % 2 ? v[n / 2] : (v[(n / 2) - 1] + v[n / 2]) / 2.0;
}

Interval cluster_bootstrap(
  const std::vector<std::vector<bool>> &clusters, const BootstrapOptions &o
) {
  ABISTUDY_EXPECTS(!clusters.empty());
  // Precompute per-cluster (successes, size) so a resample is O(#clusters).
  std::vector<std::pair<std::uint64_t, std::uint64_t>> tally;
  tally.reserve(clusters.size());
  for (const auto &c : clusters) {
    tally.emplace_back(
      static_cast<std::uint64_t>(std::ranges::count(c, true)), static_cast<std::uint64_t>(c.size())
    );
  }
  std::mt19937_64 rng(o.seed);
  std::uniform_int_distribution<std::size_t> pick(0, clusters.size() - 1);
  std::vector<double> stats;
  stats.reserve(o.resamples);
  for (std::uint32_t r = 0; r < o.resamples; ++r) {
    std::uint64_t num = 0;
    std::uint64_t den = 0;
    for (std::size_t i = 0; i < clusters.size(); ++i) {
      const auto &[s, n] = tally[pick(rng)];
      num += s;
      den += n;
    }
    stats.push_back(den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den));
  }
  const auto iv = percentile_95(std::move(stats));
  ABISTUDY_ENSURES(iv.lo >= 0 && iv.lo <= iv.hi && iv.hi <= 1);
  return iv;
}

Interval library_bootstrap(const std::vector<bool> &per_library, const BootstrapOptions &o) {
  ABISTUDY_EXPECTS(!per_library.empty());
  std::mt19937_64 rng(o.seed);
  std::uniform_int_distribution<std::size_t> pick(0, per_library.size() - 1);
  std::vector<double> stats;
  stats.reserve(o.resamples);
  for (std::uint32_t r = 0; r < o.resamples; ++r) {
    std::uint64_t num = 0;
    for (std::size_t i = 0; i < per_library.size(); ++i)
      num += per_library[pick(rng)] ? 1 : 0;
    stats.push_back(static_cast<double>(num) / static_cast<double>(per_library.size()));
  }
  return percentile_95(std::move(stats));
}

} // namespace abistudy
