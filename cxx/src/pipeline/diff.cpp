#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <thread>

#include "abi/compare.hpp"
#include "core/process.hpp"
#include "hdr/index.hpp"
#include "pipeline/materialize.hpp"
#include "pipeline/stages.hpp"

namespace abistudy::pipeline {
namespace {

std::string index_stem(const SourceName &s, const VersionString &v, Language l) {
  std::string ver = v.get();
  std::ranges::replace(ver, '/', '_');
  std::ranges::replace(ver, ':', '%');
  return std::format("{}@{}.{}", s, ver, to_string(l));
}

/// @brief Builds and stores the header index for one materialised release
///        unless it is already on disk.
Result<void> ensure_header_index(
  const Workspace &ws, const SourceName &src, const Release &rel,
  const std::filesystem::path &include_root, Language lang, std::uint32_t max_files, const Log &log
) {
  const auto p = ws.header_indexes() / (index_stem(src, rel.source_version, lang) + ".json");
  std::error_code ec;
  if (std::filesystem::exists(p, ec))
    return {};
  if (include_root.empty())
    return {};
  hdr::Options o;
  o.language = lang;
  o.max_files = max_files;
  const auto t0 = std::chrono::steady_clock::now();
  ABISTUDY_TRY(auto idx, hdr::index(include_root, o));
  const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  log(
    std::format(
      "    headers {}: {} files, {} parsed, {} fatal, {} defs, {} macros ({:.1f}s)", rel.upstream,
      idx.coverage.header_files, idx.coverage.parsed, idx.coverage.with_fatal_error,
      idx.definitions.size(), idx.macros.size(), secs
    )
  );
  return save_artifact(p, schema_header_index, idx);
}

} // namespace

Result<PairResult> diff_one(
  const Workspace &ws, const snapshot::Client &c, const PairJob &job, const DiffOptions &o,
  const Log &log
) {
  const auto t0 = std::chrono::steady_clock::now();
  PairResult res{
    .id = job.id(),
    .source = job.source,
    .upstream_1 = job.v1.upstream,
    .upstream_2 = job.v2.upstream,
    .objects = {},
    .unpaired_1 = {},
    .unpaired_2 = {},
    .object_errors = {},
    .error = std::nullopt,
    .seconds = 0
  };

  // Budget the pair from HEAD sizes before fetching a byte: dbgsym compresses
  // roughly 3x, and libabigail's memory use tracks the extracted DWARF.
  {
    ABISTUDY_TRY(const auto s1, estimate_download_bytes(c, job.v1, Want{}));
    ABISTUDY_TRY(const auto s2, estimate_download_bytes(c, job.v2, Want{}));
    const auto mb = (s1 + s2) / (std::uint64_t{1024} * 1024);
    if (mb * 3 > o.max_extracted_mb) {
      res.error = std::format(
        "skipped: {} MB of packages (~{} MB extracted) exceeds --max-extracted-mb {} (memory "
        "budget)",
        mb, mb * 3, o.max_extracted_mb
      );
      ABISTUDY_TRY_VOID(save_artifact(ws.pair(res.id), schema_pair, res));
      return res;
    }
  }
  ABISTUDY_TRY(Materialized m1, materialize(c, job.v1, Want{}, ws.scratch()));
  ABISTUDY_TRY(Materialized m2, materialize(c, job.v2, Want{}, ws.scratch()));
  if (const auto mb = (m1.bytes_extracted + m2.bytes_extracted) / (std::uint64_t{1024} * 1024);
      mb > o.max_extracted_mb) {
    // libabigail's memory use tracks the DWARF size; a pair this large will be
    // killed by the OOM killer, so record the reason instead of attempting it.
    res.error = std::format(
      "skipped: {} MB extracted exceeds --max-extracted-mb {} (memory budget)", mb,
      o.max_extracted_mb
    );
    ABISTUDY_TRY_VOID(save_artifact(ws.pair(res.id), schema_pair, res));
    return res;
  }
  log(
    std::format(
      "  {}: {} + {} objects, {:.0f} MB extracted", res.id, m1.shared_objects.size(),
      m2.shared_objects.size(), double(m1.bytes_extracted + m2.bytes_extracted) / 1e6
    )
  );

  // Pair shared objects by SONAME stem so a SONAME bump still compares.
  std::map<std::string, std::filesystem::path> by_stem_2;
  for (const auto &p : m2.shared_objects)
    by_stem_2.emplace(abi::soname_stem(p.filename().string()).get(), p);
  std::map<std::string, std::filesystem::path> by_stem_1;
  for (const auto &p : m1.shared_objects)
    by_stem_1.emplace(abi::soname_stem(p.filename().string()).get(), p);

  Language pair_lang = Language::unknown;
  for (const auto &[stem, p1] : by_stem_1) {
    const auto it = by_stem_2.find(stem);
    if (it == by_stem_2.end()) {
      res.unpaired_1.push_back(stem);
      continue;
    }
    abi::Side a{.elf = p1, .debug_info_root = m1.debug_root, .public_headers = m1.include_root};
    abi::Side b{
      .elf = it->second, .debug_info_root = m2.debug_root, .public_headers = m2.include_root
    };
    abi::Options ao;
    ao.trace = o.trace;
    auto d = abi::compare(a, b, ao);
    if (!d) {
      log(std::format("    {}: {}", stem, d.error().message));
      res.object_errors.push_back(stem + ": " + d.error().message);
      continue;
    }
    if (d->language == Language::cxx) {
      pair_lang = Language::cxx;
    } else if (pair_lang == Language::unknown) {
      pair_lang = d->language;
    }
    log(
      std::format(
        "    {} -> {}  lang={} public={} third={} private={} renamed={}", d->soname_1, d->soname_2,
        to_string(d->language), d->public_counts.items().size(),
        d->third_party_counts.items().size(), d->private_node_counts.items().size(),
        d->symbols_version_renamed
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
  if (res.objects.empty() && !res.object_errors.empty()) {
    res.error = "no object could be compared: " + res.object_errors.front().substr(0, 300);
  }

  if (o.index_headers) {
    const Language l = pair_lang == Language::unknown ? Language::cxx : pair_lang;
    if (auto r =
          ensure_header_index(ws, job.source, job.v1, m1.include_root, l, o.header_max_files, log);
        !r)
      log(std::format("    header index v1: {}", r.error().message));
    if (auto r =
          ensure_header_index(ws, job.source, job.v2, m2.include_root, l, o.header_max_files, log);
        !r)
      log(std::format("    header index v2: {}", r.error().message));
  }
  res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ABISTUDY_TRY_VOID(save_artifact(ws.pair(res.id), schema_pair, res));
  return res;
}

Result<void> run_diff(
  const Workspace &ws, const std::filesystem::path &self_exe, const DiffOptions &o, const Log &log
) {
  ABISTUDY_TRY_VOID(ws.ensure());
  ABISTUDY_TRY(Plan plan, load_artifact_as<Plan>(ws.plan(), schema_plan));
  std::vector<std::size_t> todo;
  for (std::size_t i = 0; i < plan.jobs.size(); ++i) {
    std::error_code ec;
    if (!std::filesystem::exists(ws.pair(plan.jobs[i].id()), ec)) {
      todo.push_back(i);
    }
  }
  log(
    std::format("{} pairs total, {} to do, {} workers", plan.jobs.size(), todo.size(), o.workers)
  );

  // One pass over `jobs` with `workers` child processes. Returns the indexes
  // whose child was killed by a signal: with several libabigail readers in
  // flight that is almost always the OOM killer, and such pairs deserve one
  // more attempt alone rather than a permanent failure record.
  auto pass = [&](const std::vector<std::size_t> &jobs, std::uint32_t workers) {
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::mutex mx;
    std::vector<std::size_t> killed;
    auto worker = [&] {
      for (;;) {
        const std::size_t k = next.fetch_add(1);
        if (k >= jobs.size()) {
          return;
        }
        const std::size_t i = jobs[k];
        const auto &job = plan.jobs[i];
        // prlimit caps the child's address space so an oversized DWARF fails
        // fast with bad_alloc inside the child instead of dragging the whole
        // machine into the OOM killer for minutes.
        std::vector<std::string> argv;
        if (o.child_memory_mb != 0) {
          argv = {
            "prlimit", "--as=" + std::to_string(o.child_memory_mb * std::uint64_t{1024} * 1024),
            "--"
          };
        }
        argv.insert(
          argv.end(),
          {self_exe.string(), "diff", "--work", ws.root.string(), "--one", std::to_string(i)}
        );
        proc::Options po;
        po.timeout = o.pair_timeout;
        auto r = proc::run(argv, po);
        const std::scoped_lock lk(mx);
        const auto n = ++done;
        if (!r) {
          log(
            std::format("[{}/{}] {} spawn failed: {}", n, jobs.size(), job.id(), r.error().message)
          );
          continue;
        }
        if (r->exit_code == 0 && !r->timed_out) {
          log(std::format("[{}/{}] {} ok", n, jobs.size(), job.id()));
          continue;
        }
        if (r->exit_code < 0 && !r->timed_out) { // killed by a signal (SIGKILL: OOM; SIGABRT:
                                                 // bad_alloc under prlimit)
          log(
            std::format(
              "[{}/{}] {} killed by signal {} (likely out of memory); will retry alone", n,
              jobs.size(), job.id(), -r->exit_code
            )
          );
          killed.push_back(i);
          continue;
        }
        // Record the failure as a result so the pair is not retried forever.
        PairResult pr{
          .id = job.id(),
          .source = job.source,
          .upstream_1 = job.v1.upstream,
          .upstream_2 = job.v2.upstream,
          .objects = {},
          .unpaired_1 = {},
          .unpaired_2 = {},
          .object_errors = {},
          .error = std::nullopt,
          .seconds = 0
        };
        pr.error = r->timed_out
                     ? std::string{"timeout"}
                     : ("exit " + std::to_string(r->exit_code) + ": " + r->err.substr(0, 400));
        static_cast<void>(save_artifact(ws.pair(pr.id), schema_pair, pr));
        log(std::format("[{}/{}] {} FAILED {}", n, jobs.size(), job.id(), *pr.error));
      }
    };
    std::vector<std::jthread> pool;
    for (std::uint32_t w = 0; w < std::max<std::uint32_t>(1, workers); ++w) {
      pool.emplace_back(worker);
    }
    pool.clear(); // joins
    return killed;
  };

  const auto killed = pass(todo, o.workers);
  if (!killed.empty()) {
    // A pair killed while already running alone gains nothing from a retry.
    std::vector<std::size_t> still = killed;
    if (o.workers > 1) {
      log(std::format("retrying {} killed pair(s) with one worker", killed.size()));
      still = pass(killed, 1);
    }
    for (const auto i : still) {
      const auto &job = plan.jobs[i];
      PairResult pr{
        .id = job.id(),
        .source = job.source,
        .upstream_1 = job.v1.upstream,
        .upstream_2 = job.v2.upstream,
        .objects = {},
        .unpaired_1 = {},
        .unpaired_2 = {},
        .object_errors = {},
        .error = std::string{"killed by signal twice (out of memory?)"},
        .seconds = 0
      };
      static_cast<void>(save_artifact(ws.pair(pr.id), schema_pair, pr));
      log(std::format("{} FAILED {}", job.id(), *pr.error));
    }
  }
  log("diff stage complete");
  return {};
}

} // namespace abistudy::pipeline
