#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <thread>

#include "app/materialize.hpp"
#include "app/stages.hpp"
#include "core/fs.hpp"
#include "domain/symbols.hpp"

namespace abistudy::app {
namespace {

constexpr std::uint64_t mebibyte = std::uint64_t{1024} * 1024;
/// dbgsym .debs expand roughly threefold; libabigail's memory tracks the DWARF.
constexpr std::uint64_t extraction_ratio = 3;

std::string index_stem(const SourceName &s, const VersionString &v, Language l) {
  std::string ver = v.get();
  std::ranges::replace(ver, '/', '_');
  std::ranges::replace(ver, ':', '%');
  return std::format("{}@{}.{}", s, ver, to_string(l));
}

/// @brief Builds and stores the header index for one materialised release
///        unless it is already on disk.
Result<void> ensure_header_index(
  const Workspace &ws, const Services &sv, const SourceName &src, const Release &rel,
  const std::filesystem::path &include_root, Language lang, std::uint32_t max_files
) {
  const auto p = ws.header_indexes() / (index_stem(src, rel.source_version, lang) + ".json");
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
    .bytes_extracted = 0
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
      "skipped: {} MB of packages (~{} MB extracted) exceeds --max-extracted-mb {} (memory budget)",
      mb, mb * extraction_ratio, o.max_extracted_mb
    );
    ABISTUDY_TRY_VOID(sv.store.save(ws.pair(res.id), schema_pair, res));
    return res;
  }
  ABISTUDY_TRY(Materialized m1, materialize(sv, job.v1, Want{}, ws.scratch()));
  ABISTUDY_TRY(Materialized m2, materialize(sv, job.v2, Want{}, ws.scratch()));
  res.bytes_extracted = m1.bytes_extracted + m2.bytes_extracted;
  if (const auto mb = res.bytes_extracted / mebibyte; mb > o.max_extracted_mb) {
    res.error = std::format(
      "skipped: {} MB extracted exceeds --max-extracted-mb {} (memory budget)", mb,
      o.max_extracted_mb
    );
    ABISTUDY_TRY_VOID(sv.store.save(ws.pair(res.id), schema_pair, res));
    return res;
  }
  sv.log(
    std::format(
      "  {}: {} + {} objects, {:.0f} MB extracted", res.id, m1.shared_objects.size(),
      m2.shared_objects.size(), static_cast<double>(res.bytes_extracted) / 1e6
    )
  );

  // Pair shared objects by SONAME stem so a SONAME bump still compares.
  std::map<std::string, std::filesystem::path> by_stem_2;
  for (const auto &p : m2.shared_objects)
    by_stem_2.emplace(soname_stem(p.filename().string()).get(), p);
  std::map<std::string, std::filesystem::path> by_stem_1;
  for (const auto &p : m1.shared_objects)
    by_stem_1.emplace(soname_stem(p.filename().string()).get(), p);

  Language pair_lang = Language::unknown;
  for (const auto &[stem, p1] : by_stem_1) {
    const auto it = by_stem_2.find(stem);
    if (it == by_stem_2.end()) {
      res.unpaired_1.push_back(stem);
      continue;
    }
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
    if (d->language == Language::cxx) {
      pair_lang = Language::cxx;
    } else if (pair_lang == Language::unknown) {
      pair_lang = d->language;
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
    if (!by_stem_1.contains(stem))
      res.unpaired_2.push_back(stem);
  }
  // A pair whose every object failed to compare is a failure, not a quiet
  // no-change transition; say so in the record.
  if (res.objects.empty() && !res.object_errors.empty())
    res.error = "no object could be compared: " + res.object_errors.front().substr(0, 300);

  if (o.index_headers) {
    const Language l = pair_lang == Language::unknown ? Language::cxx : pair_lang;
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
  // Scratch left by a killed child is garbage: no other run owns it.
  fs::remove_all_noexcept(ws.scratch());
  ABISTUDY_TRY_VOID(sv.store.ensure_dir(ws.scratch()));
  ABISTUDY_TRY(Plan plan, sv.store.load_as<Plan>(ws.plan(), schema_plan));

  std::vector<std::size_t> todo;
  for (std::size_t i = 0; i < plan.jobs.size(); ++i) {
    if (!sv.store.exists(ws.pair(plan.jobs[i].id())))
      todo.push_back(i);
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
    std::size_t next = 0;
    std::size_t done = 0;
    bool big_running = false;
    std::vector<std::size_t> killed;
    std::vector<std::size_t> deferred;

    auto worker = [&] {
      for (;;) {
        std::size_t i = 0;
        {
          std::unique_lock lk(mx);
          cv.wait(lk, [&] { return next >= jobs.size() || !is_big(jobs[next]) || !big_running; });
          if (next >= jobs.size())
            return;
          i = jobs[next++];
          if (deadline && std::chrono::steady_clock::now() > *deadline) {
            deferred.push_back(i);
            continue;
          }
          if (is_big(i))
            big_running = true;
        }
        const auto &job = plan.jobs[i];
        // prlimit caps the child's address space so an oversized DWARF fails
        // fast with bad_alloc inside the child instead of dragging the whole
        // machine into the OOM killer for minutes.
        std::vector<std::string> argv;
        if (o.child_memory_mb != 0)
          argv = {"prlimit", "--as=" + std::to_string(o.child_memory_mb * mebibyte), "--"};
        argv.insert(
          argv.end(),
          {self_exe.string(), "diff", "--work", ws.root.string(), "--one", std::to_string(i)}
        );
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
        pr.error = r->timed_out
                     ? std::format("timeout after {}s", o.pair_timeout.count())
                     : ("exit " + std::to_string(r->exit_code) + ": " + r->err.substr(0, 400));
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
    // A pair killed while already running alone gains nothing from a retry.
    std::vector<std::size_t> still = killed;
    if (o.workers > 1) {
      sv.log(std::format("retrying {} killed pair(s) with one worker", killed.size()));
      auto [again, deferred2] = pass(killed, 1);
      still = again;
      deferred.insert(deferred.end(), deferred2.begin(), deferred2.end());
    }
    for (const auto i : still) {
      PairResult pr = empty_result(plan.jobs[i]);
      pr.error = "killed by signal twice (out of memory?)";
      static_cast<void>(sv.store.save(ws.pair(pr.id), schema_pair, pr));
      sv.log(std::format("{} FAILED {}", pr.id, *pr.error));
    }
  }
  for (const auto i : deferred) {
    PairResult pr = empty_result(plan.jobs[i]);
    pr.error = std::format("not attempted: --deadline-minutes {} reached", o.deadline.count());
    static_cast<void>(sv.store.save(ws.pair(pr.id), schema_pair, pr));
  }
  if (!deferred.empty())
    sv.log(std::format("{} pair(s) not attempted: deadline reached", deferred.size()));
  sv.log("diff stage complete");
  return {};
}

} // namespace abistudy::app
