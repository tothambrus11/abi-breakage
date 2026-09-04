#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "app/materialize.hpp"
#include "app/stages.hpp"
#include "core/fs.hpp"
#include "domain/events.hpp"
#include "domain/symbols.hpp"

namespace abistudy::app {
namespace {

constexpr std::uint64_t mebibyte = std::uint64_t{1024} * 1024;
/// dbgsym .debs expand roughly threefold; libabigail's memory tracks the DWARF.
constexpr std::uint64_t extraction_ratio = 3;

/// @brief Builds and stores the header index for one materialised release
///        unless it is already on disk.
Result<void> ensure_header_index(
  const Workspace &ws, const Services &sv, const SourceName &src, const Release &rel,
  const std::filesystem::path &include_root, Language lang, std::uint32_t max_files
) {
  const auto p = ws.header_index(src, rel.source_version, lang);
  if (sv.store.exists(p) || include_root.empty())
    return {};
  IndexOptions o;
  o.language = lang;
  o.max_files = max_files;
  const auto t0 = std::chrono::steady_clock::now();
  ABISTUDY_TRY(auto idx, sv.indexer.index(include_root, o));
  const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  sv.log(
    std::format(
      "    headers {}: {} files, {} parsed, {} fatal, {} defs, {} macros, {} symbols ({:.1f}s)",
      rel.upstream, idx.coverage.header_files, idx.coverage.parsed, idx.coverage.with_fatal_error,
      idx.definitions.size(), idx.macros.size(), idx.declared_symbols.size(), secs
    )
  );
  return sv.store.save(p, schema_header_index, idx);
}

PairResult empty_result(const PairJob &job) {
  return PairResult{
    .id = job.id(),
    .source = job.source,
    .upstream_1 = job.v1.upstream,
    .upstream_2 = job.v2.upstream,
    .objects = {},
    .unpaired_1 = {},
    .unpaired_2 = {},
    .object_errors = {},
    .error = std::nullopt,
    .seconds = 0,
    .bytes_extracted = 0,
    .excluded_objects = {}
  };
}

} // namespace

Result<PairResult> diff_one(
  const Workspace &ws, const Services &sv, const PairJob &job, const DiffOptions &o
) {
  const auto t0 = std::chrono::steady_clock::now();
  PairResult res = empty_result(job);

  // Budget the pair from the plan's sizes before fetching a byte.
  if (const auto mb = job.download_bytes() / mebibyte; mb * extraction_ratio > o.max_extracted_mb) {
    res.error = std::format(
      "{}{} MB of packages (~{} MB extracted) exceeds --max-extracted-mb {} (memory budget)",
      pair_error_skipped, mb, mb * extraction_ratio, o.max_extracted_mb
    );
    ABISTUDY_TRY_VOID(sv.store.save(ws.pair(res.id), schema_pair, res));
    return res;
  }
  ABISTUDY_TRY(Materialized m1, materialize(sv, job.v1, Want{}, ws.scratch()));
  ABISTUDY_TRY(Materialized m2, materialize(sv, job.v2, Want{}, ws.scratch()));
  res.bytes_extracted = m1.bytes_extracted + m2.bytes_extracted;
  if (const auto mb = res.bytes_extracted / mebibyte; mb > o.max_extracted_mb) {
    res.error = std::format(
      "{}{} MB extracted exceeds --max-extracted-mb {} (memory budget)", pair_error_skipped, mb,
      o.max_extracted_mb
    );
    ABISTUDY_TRY_VOID(sv.store.save(ws.pair(res.id), schema_pair, res));
    return res;
  }
  sv.log(
    std::format(
      "  {}: {} + {} objects, {:.0f} MB extracted{}", res.id, m1.shared_objects.size(),
      m2.shared_objects.size(), static_cast<double>(res.bytes_extracted) / 1e6,
      m1.missing.empty() && m2.missing.empty()
        ? std::string{}
        : std::format(", missing: {} / {}", Json(m1.missing).dump(), Json(m2.missing).dump())
    )
  );
  for (const auto *m : {&m1, &m2}) {
    for (const auto &pkg : m->missing)
      res.object_errors.push_back("missing package: " + pkg);
    res.excluded_objects.insert(
      res.excluded_objects.end(), m->excluded_objects.begin(), m->excluded_objects.end()
    );
  }

  // Pair shared objects by SONAME stem so a SONAME bump still compares. A
  // stem that carries the version itself (libhunspell-1.6 -> libhunspell-1.7,
  // libOpenEXR-3_1 -> libOpenEXR-3_4) is paired digits-blind when that is
  // unambiguous on both sides: these are exactly the declared breaks, and
  // losing them would bias the declared/silent split.
  std::map<std::string, std::filesystem::path> by_stem_2;
  for (const auto &p : m2.shared_objects)
    by_stem_2.emplace(soname_stem(p.filename().string()).get(), p);
  std::map<std::string, std::filesystem::path> by_stem_1;
  for (const auto &p : m1.shared_objects)
    by_stem_1.emplace(soname_stem(p.filename().string()).get(), p);
  const auto partner = [&](const std::string &stem) {
    if (const auto it = by_stem_2.find(stem); it != by_stem_2.end())
      return it;
    const auto blind = digits_blind(stem);
    if (std::ranges::count_if(by_stem_1, [&](const auto &kv) {
          return digits_blind(kv.first) == blind;
        }) != 1)
      return by_stem_2.end();
    auto found = by_stem_2.end();
    for (auto it = by_stem_2.begin(); it != by_stem_2.end(); ++it) {
      if (digits_blind(it->first) != blind || by_stem_1.contains(it->first))
        continue;
      if (found != by_stem_2.end())
        return by_stem_2.end();
      found = it;
    }
    return found;
  };

  std::set<std::string> paired_2;
  for (const auto &[stem, p1] : by_stem_1) {
    const auto it = partner(stem);
    if (it == by_stem_2.end()) {
      res.unpaired_1.push_back(stem);
      continue;
    }
    paired_2.insert(it->first);
    const ports::Side a{
      .elf = p1, .debug_info_root = m1.debug_root, .public_headers = m1.include_root
    };
    const ports::Side b{
      .elf = it->second, .debug_info_root = m2.debug_root, .public_headers = m2.include_root
    };
    ports::CompareOptions co;
    co.trace = o.trace;
    auto d = sv.comparer.compare(a, b, co);
    if (!d) {
      sv.log(std::format("    {}: {}", stem, d.error().message));
      res.object_errors.push_back(stem + ": " + d.error().message);
      continue;
    }
    sv.log(
      std::format(
        "    {} -> {}  lang={} public={} third={} private={} vague={} renamed={}", d->soname_1,
        d->soname_2, to_string(d->language), d->public_counts.total(),
        d->third_party_counts.total(), d->private_node_counts.total(),
        d->vague_linkage_counts.total(), d->symbols_version_renamed
      )
    );
    res.objects.push_back(std::move(*d));
  }
  for (const auto &[stem, p2] : by_stem_2) {
    if (!paired_2.contains(stem))
      res.unpaired_2.push_back(stem);
  }
  // A pair whose every object failed to compare is a failure, not a quiet
  // no-change transition; say so in the record.
  if (res.objects.empty() && !res.object_errors.empty())
    res.error = "no object could be compared: " + res.object_errors.front().substr(0, 300);

  if (o.index_headers) {
    const Language l = dominant_language(res.objects, Language::cxx);
    auto index_side = [&](const Release &rel, const Materialized &m) {
      if (
        auto r =
          ensure_header_index(ws, sv, job.source, rel, m.include_root, l, o.header_max_files);
        !r
      ) {
        sv.log(std::format("    header index {}: {}", rel.upstream, r.error().message));
      }
    };
    index_side(job.v1, m1);
    index_side(job.v2, m2);
  }
  res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ABISTUDY_TRY_VOID(sv.store.save(ws.pair(res.id), schema_pair, res));
  return res;
}

Result<void> run_diff(
  const Workspace &ws, const Services &sv, const std::filesystem::path &self_exe,
  const DiffOptions &o
) {
  ABISTUDY_TRY_VOID(ensure_workspace(ws, sv));
  // The lock proves no other stage is materialising under scratch; only then
  // is what is left there garbage from a killed child.
  ABISTUDY_TRY(const fs::LockFile lock, fs::LockFile::acquire(ws.scratch_lock()));
  fs::remove_all_noexcept(ws.scratch());
  ABISTUDY_TRY_VOID(sv.store.ensure_dir(ws.scratch()));
  ABISTUDY_TRY(Plan plan, sv.store.load_as<Plan>(ws.plan(), schema_plan));

  std::vector<std::size_t> todo;
  std::uint32_t retried = 0;
  for (std::size_t i = 0; i < plan.jobs.size(); ++i) {
    const auto p = ws.pair(plan.jobs[i].id());
    if (o.retry_failed && sv.store.exists(p)) {
      auto pr = sv.store.load_as<PairResult>(p, schema_pair);
      if (pr && retryable_with_more_resources(pair_outcome(*pr))) {
        ABISTUDY_TRY_VOID(sv.store.remove(p));
        ++retried;
      }
    }
    if (!sv.store.exists(p))
      todo.push_back(i);
  }
  if (o.retry_failed) {
    sv.log(
      std::format("--retry-failed: {} failed_memory/failed_timeout record(s) discarded", retried)
    );
  }
  // Largest first: the long pole starts immediately and the tail is short.
  std::ranges::stable_sort(todo, [&](std::size_t a, std::size_t b) {
    return plan.jobs[a].download_bytes() > plan.jobs[b].download_bytes();
  });
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = o.deadline.count() > 0
                          ? std::optional{started + o.deadline}
                          : std::optional<std::chrono::steady_clock::time_point>{};
  sv.log(
    std::format(
      "{} pairs total, {} to do, {} workers, big pair > {} MB download, deadline {} min",
      plan.jobs.size(), todo.size(), o.workers, o.big_pair_download_mb, o.deadline.count()
    )
  );

  const std::uint64_t big_bytes = o.big_pair_download_mb * mebibyte;
  auto is_big = [&](std::size_t i) { return plan.jobs[i].download_bytes() > big_bytes; };

  /// One pass over `jobs` with `workers` children; at most one big pair at a
  /// time. Returns the indexes whose child was killed by a signal: with
  /// several readers in flight that is almost always the OOM killer, and
  /// such pairs deserve one more attempt alone rather than a failure record.
  auto pass = [&](const std::vector<std::size_t> &jobs, std::uint32_t workers) {
    std::mutex mx;
    std::condition_variable cv;
    // Two queues, both largest first: at most one big pair runs at a time,
    // and a worker that cannot take a big one takes the next small one.
    std::deque<std::size_t> big;
    std::deque<std::size_t> small;
    for (const auto i : jobs)
      (is_big(i) ? big : small).push_back(i);
    std::size_t done = 0;
    bool big_running = false;
    std::vector<std::size_t> killed;
    std::vector<std::size_t> deferred;

    auto worker = [&] {
      for (;;) {
        std::size_t i = 0;
        {
          std::unique_lock lk(mx);
          cv.wait(lk, [&] { return !small.empty() || big.empty() || !big_running; });
          if (small.empty() && big.empty())
            return;
          if (!big.empty() && !big_running) {
            i = big.front();
            big.pop_front();
            big_running = true;
          } else {
            i = small.front();
            small.pop_front();
          }
          if (deadline && std::chrono::steady_clock::now() > *deadline) {
            if (is_big(i))
              big_running = false;
            deferred.push_back(i);
            cv.notify_all();
            continue;
          }
        }
        const auto &job = plan.jobs[i];
        // prlimit caps the child's address space so an oversized DWARF fails
        // fast with bad_alloc inside the child instead of dragging the whole
        // machine into the OOM killer for minutes.
        std::vector<std::string> argv;
        if (o.child_memory_mb != 0)
          argv = {"prlimit", "--as=" + std::to_string(o.child_memory_mb * mebibyte), "--"};
        argv.insert(
          argv.end(), {self_exe.string(), "diff", "--work", ws.root.string(), "--one",
                       std::to_string(i), "--max-extracted-mb", std::to_string(o.max_extracted_mb),
                       "--max-files", std::to_string(o.header_max_files)}
        );
        if (!o.index_headers)
          argv.emplace_back("--no-headers");
        if (o.trace)
          argv.emplace_back("--trace");
        ports::RunOptions po;
        po.timeout = o.pair_timeout;
        const auto t0 = std::chrono::steady_clock::now();
        auto r = sv.runner.run(argv, po);
        const auto secs =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        const std::scoped_lock lk(mx);
        if (is_big(i)) {
          big_running = false;
          cv.notify_all();
        }
        const auto n = ++done;
        const auto prefix = std::format(
          "[{}/{}] {} ({} MB dl, {:.0f}s, {} MB rss)", n, jobs.size(), job.id(),
          job.download_bytes() / mebibyte, secs, r ? r->max_rss_kb / 1024 : 0
        );
        if (!r) {
          sv.log(std::format("{} spawn failed: {}", prefix, r.error().message));
          continue;
        }
        if (r->exit_code == 0 && !r->timed_out) {
          sv.log(std::format("{} ok", prefix));
          continue;
        }
        if (r->exit_code < 0 && !r->timed_out) {
          sv.log(
            std::format(
              "{} killed by signal {} (likely out of memory); will retry alone", prefix,
              -r->exit_code
            )
          );
          killed.push_back(i);
          continue;
        }
        // Record the failure as a result so the pair is not retried forever.
        PairResult pr = empty_result(job);
        pr.error =
          r->timed_out
            ? std::format("{}{}s", pair_error_timeout, o.pair_timeout.count())
            : std::format("{}{}: {}", pair_error_exit, r->exit_code, r->err.substr(0, 400));
        pr.seconds = secs;
        static_cast<void>(sv.store.save(ws.pair(pr.id), schema_pair, pr));
        sv.log(std::format("{} FAILED {}", prefix, *pr.error));
      }
    };
    {
      std::vector<std::jthread> pool;
      for (std::uint32_t w = 0; w < std::max<std::uint32_t>(1, workers); ++w)
        pool.emplace_back(worker);
    }
    return std::pair{killed, deferred};
  };

  auto [killed, deferred] = pass(todo, o.workers);
  if (!killed.empty()) {
    // A pair killed while already running alone gains nothing from a retry
    // under the same cap; `diff --retry-failed` with a larger one is the way.
    std::vector<std::size_t> still = killed;
    const bool retried_alone = o.workers > 1;
    if (retried_alone) {
      sv.log(std::format("retrying {} killed pair(s) with one worker", killed.size()));
      auto [again, deferred2] = pass(killed, 1);
      still = again;
      deferred.insert(deferred.end(), deferred2.begin(), deferred2.end());
    }
    for (const auto i : still) {
      PairResult pr = empty_result(plan.jobs[i]);
      pr.error = std::format(
        "{}{} under --child-memory-mb {} (out of memory?)", pair_error_killed,
        retried_alone ? " twice" : " once, already alone", o.child_memory_mb
      );
      static_cast<void>(sv.store.save(ws.pair(pr.id), schema_pair, pr));
      sv.log(std::format("{} FAILED {}", pr.id, *pr.error));
    }
  }
  for (const auto i : deferred) {
    PairResult pr = empty_result(plan.jobs[i]);
    pr.error =
      std::format("{}--deadline-minutes {} reached", pair_error_not_attempted, o.deadline.count());
    static_cast<void>(sv.store.save(ws.pair(pr.id), schema_pair, pr));
  }
  if (!deferred.empty())
    sv.log(std::format("{} pair(s) not attempted: deadline reached", deferred.size()));
  sv.log("diff stage complete");
  return {};
}

} // namespace abistudy::app
