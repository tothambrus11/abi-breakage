#pragma once
// =============================================================================
// The pipeline stages as application services. Each takes the Workspace (the
// study directory), the Services (the ports the composition root wired) and
// its options; main() only parses arguments and calls these. Re-running a
// stage is idempotent -- results that already exist are kept, so an
// interrupted run resumes where it stopped.
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>

#include "core/error.hpp"
#include "domain/records.hpp"
#include "ports/abi_comparer.hpp"
#include "ports/artifact_store.hpp"
#include "ports/extractor.hpp"
#include "ports/header_indexer.hpp"
#include "ports/log.hpp"
#include "ports/package_source.hpp"
#include "ports/process_runner.hpp"

namespace abistudy::app {

/// @brief Layout of a study directory. Paths are derived, never configured
///        individually, so stages always agree on where things are.
struct Workspace {
  std::filesystem::path root;

  [[nodiscard]] std::filesystem::path cache() const { return root / "cache"; }
  [[nodiscard]] std::filesystem::path scratch() const { return root / "scratch"; }
  [[nodiscard]] std::filesystem::path selection() const { return root / "selection.json"; }
  [[nodiscard]] std::filesystem::path plan() const { return root / "plan.json"; }
  [[nodiscard]] std::filesystem::path pairs() const { return root / "pairs"; }
  [[nodiscard]] std::filesystem::path pair(std::string_view id) const {
    return pairs() / (std::string{id} + ".json");
  }
  [[nodiscard]] std::filesystem::path header_indexes() const { return root / "headers" / "index"; }
  /// @brief The header index of one release in one language: written by the
  ///        diff stage, read by the headers stage, so the name lives here.
  [[nodiscard]] std::filesystem::path header_index(
    const SourceName &src, const VersionString &ver, Language lang
  ) const {
    std::string v = ver.get();
    std::ranges::replace(v, '/', '_');
    std::ranges::replace(v, ':', '%');
    return header_indexes() / std::format("{}@{}.{}.json", src, v, to_string(lang));
  }
  /// @brief Lock taken by every stage that materialises packages under
  ///        scratch(): the diff stage wipes the tree when it starts.
  [[nodiscard]] std::filesystem::path scratch_lock() const { return root / "scratch.lock"; }
  [[nodiscard]] std::filesystem::path header_pairs() const { return root / "headers" / "pairs"; }
  [[nodiscard]] std::filesystem::path header_pair(std::string_view id) const {
    return header_pairs() / (std::string{id} + ".json");
  }
  [[nodiscard]] std::filesystem::path summary() const { return root / "summary.json"; }
  [[nodiscard]] std::filesystem::path report_text() const { return root / "report.txt"; }
  [[nodiscard]] std::filesystem::path report_html() const { return root / "report.html"; }
};

/// @brief The ports a stage may use. References: the composition root owns
///        the adapters for the life of the process, and a Services value is
///        built once and never assigned.
// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members): non-owning, immutable bundle
struct Services {
  const ports::PackageSource &packages;
  const ports::PackageExtractor &extractor;
  const ports::AbiComparer &comparer;
  const ports::HeaderIndexer &indexer;
  const ports::ArtifactStore &store;
  const ports::ProcessRunner &runner;
  ports::Log log;
};
// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

/// @brief Creates the workspace directory tree.
/// @errors io.
[[nodiscard]] Result<void> ensure_workspace(const Workspace &ws, const Services &sv);

// ---- select -----------------------------------------------------------------

struct SelectOptions {
  std::uint32_t c_limit = 70;
  std::uint32_t cxx_limit = 50;
  std::uint32_t scan = 4000; ///< How far down popcon to look.
  std::string popcon_url = "https://popcon.debian.org/by_inst";
  std::string packages_url =
    "https://deb.debian.org/debian/dists/trixie/main/binary-amd64/Packages.xz";
};

/// @brief Chooses the corpus: the most-installed lib* packages whose source
///        ships both a -dbgsym and a -dev package, quota'd by language.
/// @post  Writes ws.selection(). Returns the selection.
/// @errors network/http_status/io/parse.
[[nodiscard]] Result<Selection> run_select(
  const Workspace &ws, const Services &sv, const SelectOptions &o
);

// ---- resolve ----------------------------------------------------------------

struct ResolveOptions {
  std::uint32_t releases = 10; ///< Most recent distinct upstream releases per library.
  std::uint32_t max_scan = 40; ///< Archive versions to examine before giving up.
};

/// @brief Turns the selection into consecutive-release PairJobs, with the
///        download size of every release recorded for scheduling.
/// @pre   ws.selection() exists.
/// @post  Writes ws.plan(). Pairs are (older, newer) by dpkg version order.
[[nodiscard]] Result<Plan> run_resolve(
  const Workspace &ws, const Services &sv, const ResolveOptions &o
);

// ---- diff -------------------------------------------------------------------

struct DiffOptions {
  std::uint32_t workers = 4; ///< Concurrent child processes.
  std::chrono::seconds pair_timeout{1200};
  bool index_headers = true; ///< Also build header indexes from the -dev trees.
  std::uint32_t header_max_files = 2000;
  bool trace = false;
  /// Pairs whose two extracted trees are estimated above this are recorded
  /// as skipped: DWARF of that size does not fit the memory budget of one reader.
  std::uint64_t max_extracted_mb = 2500;
  /// Address-space cap for each child process (prlimit --as); 0 disables.
  std::uint64_t child_memory_mb = 6000;
  /// Pairs whose download exceeds this run one at a time, largest first, so
  /// two big readers never coexist.
  std::uint64_t big_pair_download_mb = 40;
  /// Stop starting new pairs after this much wall-clock time; the rest are
  /// recorded as not attempted. 0 = no deadline.
  std::chrono::minutes deadline{0};
  /// Before scheduling, discard the records of pairs that failed for lack of
  /// memory or time (PairOutcome::failed_memory / failed_timeout) so they run
  /// again under this invocation's caps. Budget skips and deadline deferrals
  /// are kept: a larger cap does not change them.
  bool retry_failed = false;
};

/// @brief Compares one PairJob in THIS process: materialises both releases,
///        pairs shared objects by SONAME stem, runs the comparer on each, and
///        (optionally) indexes both releases' headers.
/// @post  Writes ws.pair(job.id()) and, if enabled, the two header indexes.
///        Scratch space is released before returning.
/// @errors Errors from materialisation are returned; a failing comparison of
///         one object is recorded inside the PairResult and is not an error.
[[nodiscard]] Result<PairResult> diff_one(
  const Workspace &ws, const Services &sv, const PairJob &job, const DiffOptions &o
);

/// @brief Runs diff_one for every job in ws.plan() that has no result yet, in
///        child processes (libabigail is not thread-safe), largest first.
/// @pre   `self_exe` is the path of this executable.
/// @post  Every job has a result file (possibly carrying an error string).
[[nodiscard]] Result<void> run_diff(
  const Workspace &ws, const Services &sv, const std::filesystem::path &self_exe,
  const DiffOptions &o
);

// ---- headers ----------------------------------------------------------------

struct HeadersOptions {
  std::uint32_t max_files = 2000;
};

/// @brief Produces a HeaderResult per job from the per-release header indexes
///        (building any missing index from a fresh download of the -dev
///        packages), including the declared-symbol join.
/// @post  Writes ws.header_pair(id) for every job.
[[nodiscard]] Result<void> run_headers(
  const Workspace &ws, const Services &sv, const HeadersOptions &o
);

// ---- analyze / report ---------------------------------------------------------

/// @brief Aggregates pair and header results into summary.json and report.txt.
/// @post  Both files written. Returns the report text.
[[nodiscard]] Result<std::string> run_analyze(const Workspace &ws, const Services &sv);

/// @brief Renders report.html from summary.json using the embedded template.
/// @pre   ws.summary() exists.
[[nodiscard]] Result<std::string> run_report(const Workspace &ws, const Services &sv);

} // namespace abistudy::app
