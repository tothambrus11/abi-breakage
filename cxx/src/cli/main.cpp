// =============================================================================
// abistudy command line: the composition root. Parses arguments, wires the
// adapters into the ports, dispatches to a stage, and turns the stage's
// Result into an exit code. No analysis logic lives here.
// =============================================================================

#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "adapters/abigail/comparer.hpp"
#include "adapters/fs/artifact_store.hpp"
#include "adapters/libarchive/extract.hpp"
#include "adapters/libclang/indexer.hpp"
#include "adapters/posix/process.hpp"
#include "adapters/snapshot/source.hpp"
#include "app/stages.hpp"
#include "core/error.hpp"

namespace {

using namespace abistudy;
using namespace abistudy::app;

constexpr std::string_view usage =
  R"(abistudy -- which kinds of ABI change happen between consecutive library releases

usage: abistudy <stage> [--work DIR] [options]

stages (run in this order; each is idempotent and resumable):
  select    choose the corpus from Debian popcon            --c-limit N --cxx-limit N --scan N
  resolve   pick consecutive upstream releases per library  --releases N --max-scan N
  diff      compare shared objects with libabigail          --workers N --pair-timeout SECONDS --no-headers --trace
                                                           --max-extracted-mb N --child-memory-mb N
                                                           --big-pair-mb N --deadline-minutes N
  headers   compare header bodies/macros with libclang      --max-files N
  analyze   aggregate into summary.json and report.txt
  report    render report.html from summary.json
  all       select, resolve, diff, headers, analyze, report

common:
  --work DIR          study directory (default ./study)
  --api-cache-days N  age after which cached snapshot API answers are refetched (default 7)
  --help              this text

exit status: 0 success, 1 stage error, 2 usage error
)";

/// @brief Parsed command line: a stage name plus `--key value` / `--flag` options.
/// @invariant Every key is stored without its leading dashes.
struct Args {
  std::string stage;
  std::map<std::string, std::string> opts;

  [[nodiscard]] std::string get(std::string_view k, std::string_view def) const {
    const auto it = opts.find(std::string{k});
    return it == opts.end() ? std::string{def} : it->second;
  }
  [[nodiscard]] bool has(std::string_view k) const { return opts.contains(std::string{k}); }

  /// @errors invalid_argument if present but not an unsigned integer.
  [[nodiscard]] Result<std::uint32_t> uint(std::string_view k, std::uint32_t def) const {
    const auto it = opts.find(std::string{k});
    if (it == opts.end())
      return def;
    std::uint32_t v = 0;
    const auto [p, ec] =
      std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
    if (ec != std::errc{} || p != it->second.data() + it->second.size()) {
      return fail(
        ErrorCode::invalid_argument, "--{} expects an unsigned integer, got '{}'", k, it->second
      );
    }
    return v;
  }
};

/// @brief Parses argv. Values may be given as `--k v` or `--k=v`.
/// @returns nullopt if there is no stage.
std::optional<Args> parse(int argc, const char *const *argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string_view s = argv[i];
    if (s.starts_with("--")) {
      s.remove_prefix(2);
      if (const auto eq = s.find('='); eq != std::string_view::npos) {
        a.opts[std::string{s.substr(0, eq)}] = std::string{s.substr(eq + 1)};
      } else if (i + 1 < argc && !std::string_view{argv[i + 1]}.starts_with("--")) {
        a.opts[std::string{s}] = argv[++i];
      } else {
        a.opts[std::string{s}] = "";
      }
    } else if (a.stage.empty()) {
      a.stage = s;
    } else {
      return std::nullopt;
    }
  }
  if (a.stage.empty())
    return std::nullopt;
  return a;
}

void log_line(std::string_view msg) {
  std::println(stderr, "[{}] {}", fsstore::utc_now_iso8601().substr(11, 8), msg);
}

Result<void> run(const Args &a) {
  const Workspace ws{std::filesystem::absolute(a.get("work", "study"))};

  // ---- composition root: adapters behind ports ---------------------------------
  snapshot::ClientOptions so;
  so.cache_dir = ws.cache() / "api";
  ABISTUDY_TRY(auto cache_days, a.uint("api-cache-days", 7));
  so.api_cache_ttl = std::chrono::hours(24) * cache_days;
  ABISTUDY_TRY(auto client, snapshot::Client::create(so));
  const snapshot::SnapshotPackageSource packages{std::move(client)};
  const deb::LibarchiveExtractor extractor;
  const abigail::AbigailComparer comparer;
  const libclang::LibclangIndexer indexer;
  const fsstore::FsArtifactStore store{
    ports::Provenance{.abi_reader = comparer.version(), .header_parser = indexer.version()}
  };
  const posix::PosixProcessRunner runner;
  const Services sv{
    .packages = packages,
    .extractor = extractor,
    .comparer = comparer,
    .indexer = indexer,
    .store = store,
    .runner = runner,
    .log = log_line
  };
  ABISTUDY_TRY_VOID(ensure_workspace(ws, sv));

  const auto do_select = [&] -> Result<void> {
    SelectOptions o;
    ABISTUDY_TRY(o.c_limit, a.uint("c-limit", o.c_limit));
    ABISTUDY_TRY(o.cxx_limit, a.uint("cxx-limit", o.cxx_limit));
    ABISTUDY_TRY(o.scan, a.uint("scan", o.scan));
    ABISTUDY_TRY_VOID(run_select(ws, sv, o));
    return {};
  };
  const auto do_resolve = [&] -> Result<void> {
    ResolveOptions o;
    ABISTUDY_TRY(o.releases, a.uint("releases", o.releases));
    ABISTUDY_TRY(o.max_scan, a.uint("max-scan", o.max_scan));
    ABISTUDY_TRY_VOID(run_resolve(ws, sv, o));
    return {};
  };
  const auto do_diff = [&] -> Result<void> {
    DiffOptions o;
    ABISTUDY_TRY(o.workers, a.uint("workers", o.workers));
    ABISTUDY_TRY(auto secs, a.uint("pair-timeout", 1200));
    o.pair_timeout = std::chrono::seconds(secs);
    o.index_headers = !a.has("no-headers");
    o.trace = a.has("trace");
    ABISTUDY_TRY(auto mb, a.uint("max-extracted-mb", 2500));
    o.max_extracted_mb = mb;
    ABISTUDY_TRY(auto cm, a.uint("child-memory-mb", 6000));
    o.child_memory_mb = cm;
    ABISTUDY_TRY(auto big, a.uint("big-pair-mb", 40));
    o.big_pair_download_mb = big;
    ABISTUDY_TRY(auto dl, a.uint("deadline-minutes", 0));
    o.deadline = std::chrono::minutes(dl);
    ABISTUDY_TRY(o.header_max_files, a.uint("max-files", o.header_max_files));
    if (a.has("one")) {
      ABISTUDY_TRY(auto i, a.uint("one", 0));
      ABISTUDY_TRY(Plan plan, store.load_as<Plan>(ws.plan(), schema_plan));
      if (i >= plan.jobs.size()) {
        return fail(
          ErrorCode::invalid_argument, "--one {} out of range ({} jobs)", i, plan.jobs.size()
        );
      }
      ABISTUDY_TRY_VOID(diff_one(ws, sv, plan.jobs[i], o));
      return {};
    }
    std::error_code ec;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec)
      return fail(ErrorCode::io, "cannot resolve /proc/self/exe: {}", ec.message());
    return run_diff(ws, sv, self, o);
  };
  const auto do_headers = [&] -> Result<void> {
    HeadersOptions o;
    ABISTUDY_TRY(o.max_files, a.uint("max-files", o.max_files));
    return run_headers(ws, sv, o);
  };
  const auto do_analyze = [&] -> Result<void> {
    ABISTUDY_TRY(auto text, run_analyze(ws, sv));
    std::print("{}", text);
    return {};
  };

  if (a.stage == "select")
    return do_select();
  if (a.stage == "resolve")
    return do_resolve();
  if (a.stage == "diff")
    return do_diff();
  if (a.stage == "headers")
    return do_headers();
  if (a.stage == "analyze")
    return do_analyze();
  if (a.stage == "report") {
    ABISTUDY_TRY_VOID(run_report(ws, sv));
    return {};
  }
  if (a.stage == "all") {
    ABISTUDY_TRY_VOID(do_select());
    ABISTUDY_TRY_VOID(do_resolve());
    ABISTUDY_TRY_VOID(do_diff());
    ABISTUDY_TRY_VOID(do_headers());
    ABISTUDY_TRY_VOID(do_analyze());
    ABISTUDY_TRY_VOID(run_report(ws, sv));
    return {};
  }
  return fail(ErrorCode::invalid_argument, "unknown stage '{}'", a.stage);
}

} // namespace

int main(int argc, char **argv) try {
  const auto args = parse(argc, argv);
  if (!args || args->has("help")) {
    std::print("{}", usage);
    return args ? 0 : 2;
  }
  if (const auto r = run(*args); !r) {
    std::println(stderr, "abistudy {}: {}", args->stage, r.error().str());
    return r.error().code == ErrorCode::invalid_argument ? 2 : 1;
  }
  return 0;
} catch (const std::exception &e) { // last resort: nothing above is meant to throw
  static_cast<void>(std::fputs("abistudy: unexpected exception: ", stderr));
  static_cast<void>(std::fputs(e.what(), stderr));
  static_cast<void>(std::fputs("\n", stderr));
  return 1;
} catch (...) {
  static_cast<void>(std::fputs("abistudy: unexpected non-standard exception\n", stderr));
  return 1;
}
