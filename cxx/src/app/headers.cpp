#include <format>
#include <optional>
#include <utility>

#include "app/materialize.hpp"
#include "app/stages.hpp"
#include "core/fs.hpp"
#include "domain/events.hpp"

namespace abistudy::app {
namespace {

/// @brief Loads a release's header index, building it from a fresh download
///        of the -dev packages if the diff stage did not leave one behind.
Result<HeaderIndex> get_index(
  const Workspace &ws, const Services &sv, const SourceName &src, const Release &rel, Language lang,
  std::uint32_t max_files
) {
  const auto p = ws.header_index(src, rel.source_version, lang);
  if (sv.store.exists(p)) {
    auto cached = sv.store.load_as<HeaderIndex>(p, schema_header_index);
    if (cached || cached.error().code != ErrorCode::schema)
      return cached;
    sv.log(std::format("    rebuilding {} (index schema changed)", p.filename().string()));
  }
  ABISTUDY_TRY(
    Materialized m,
    materialize(sv, rel, Want{.runtime = false, .dbgsym = false, .dev = true}, ws.scratch())
  );
  if (m.include_root.empty()) {
    HeaderIndex empty;
    empty.language = lang;
    ABISTUDY_TRY_VOID(sv.store.save(p, schema_header_index, empty));
    return empty;
  }
  IndexOptions o;
  o.language = lang;
  o.max_files = max_files;
  ABISTUDY_TRY(auto idx, sv.indexer.index(m.include_root, o));
  sv.log(
    std::format(
      "    indexed {} {}: {} defs, {} macros, {} symbols, {}/{} parsed", src, rel.upstream,
      idx.definitions.size(), idx.macros.size(), idx.declared_symbols.size(), idx.coverage.parsed,
      idx.coverage.header_files
    )
  );
  ABISTUDY_TRY_VOID(sv.store.save(p, schema_header_index, idx));
  return idx;
}

/// @brief The declared-symbol join (REVIEW.md §1.4): removed and re-signed
///        symbols against the OLD release's headers. Only those two kinds are
///        joined, so a symbol that moves between objects (removed from one,
///        added to another) is classified against the old headers exactly
///        once and never against the new ones.
void join_symbols(const PairResult &pr, const HeaderIndex &old_idx, HeaderResult &hr) {
  for (const auto &o : pr.objects) {
    for (const auto &e : o.symbol_events) {
      if (e.kind != ChangeKind::symbol_removed && e.kind != ChangeKind::function_signature_changed)
        continue;
      hr.symbol_declared.emplace(e.symbol.get(), symbol_declared(old_idx, e.symbol.get()));
    }
  }
}

} // namespace

Result<void> run_headers(const Workspace &ws, const Services &sv, const HeadersOptions &o) {
  ABISTUDY_TRY_VOID(ensure_workspace(ws, sv));
  // Materialises -dev packages under scratch: never while a diff wipes it.
  ABISTUDY_TRY(const fs::LockFile lock, fs::LockFile::acquire(ws.scratch_lock()));
  ABISTUDY_TRY(Plan plan, sv.store.load_as<Plan>(ws.plan(), schema_plan));
  std::size_t n = 0;
  // Jobs of one library are consecutive and share a release: the newer index
  // of job k is the older index of job k+1, so it is kept for one step.
  std::optional<std::pair<std::string, HeaderIndex>> previous; // (index path, index)
  for (const auto &job : plan.jobs) {
    ++n;
    if (sv.store.exists(ws.header_pair(job.id())))
      continue;
    auto pair = sv.store.load_as<PairResult>(ws.pair(job.id()), schema_pair);
    const PairResult *pr = pair ? &*pair : nullptr;
    const Language lang = pr ? dominant_language(pr->objects, Language::cxx) : Language::cxx;
    HeaderResult hr{
      .id = job.id(),
      .source = job.source,
      .upstream_1 = job.v1.upstream,
      .upstream_2 = job.v2.upstream,
      .language = lang,
      .diff = std::nullopt,
      .coverage_1 = {},
      .coverage_2 = {},
      .error = std::nullopt,
      .symbol_declared = {}
    };
    const auto path_1 = ws.header_index(job.source, job.v1.source_version, lang).string();
    const auto path_2 = ws.header_index(job.source, job.v2.source_version, lang).string();
    Result<HeaderIndex> a = previous && previous->first == path_1
                              ? Result<HeaderIndex>{std::move(previous->second)}
                              : get_index(ws, sv, job.source, job.v1, lang, o.max_files);
    previous.reset();
    auto b = a ? get_index(ws, sv, job.source, job.v2, lang, o.max_files)
               : Result<HeaderIndex>{std::unexpected{a.error()}};
    if (!a || !b) {
      hr.error = (a ? b : a).error().message;
    } else {
      hr.diff = compare_headers(*a, *b);
      hr.coverage_1 = a->coverage;
      hr.coverage_2 = b->coverage;
      if (pr)
        join_symbols(*pr, *a, hr);
      previous.emplace(path_2, std::move(*b));
    }
    ABISTUDY_TRY_VOID(sv.store.save(ws.header_pair(hr.id), schema_header_pair, hr));
    if (n % 25 == 0 || hr.error) {
      sv.log(
        std::format(
          "[{}/{}] {} bodies={} macros={} symbols_joined={}{}", n, plan.jobs.size(), hr.id,
          hr.diff ? hr.diff->inline_body_changed : 0, hr.diff ? hr.diff->macro_value_changed : 0,
          hr.symbol_declared.size(), hr.error ? " ERR " + *hr.error : ""
        )
      );
    }
  }
  sv.log("headers stage complete");
  return {};
}

} // namespace abistudy::app
