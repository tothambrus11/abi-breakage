#pragma once
// =============================================================================
// The five pipeline stages as library functions. main() only parses arguments
// and calls these, so every stage is testable without a process boundary.
//
// All stages take a `Workspace`: one directory that holds the artefacts of a
// study. Re-running a stage is idempotent -- results that already exist are
// kept, so an interrupted run resumes where it stopped.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "core/error.hpp"
#include "core/fs.hpp"
#include "pipeline/artifacts.hpp"
#include "snapshot/client.hpp"

namespace abistudy::pipeline {

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
  [[nodiscard]] std::filesystem::path header_pairs() const { return root / "headers" / "pairs"; }
  [[nodiscard]] std::filesystem::path header_pair(std::string_view id) const {
    return header_pairs() / (std::string{id} + ".json");
  }
  [[nodiscard]] std::filesystem::path summary() const { return root / "summary.json"; }
  [[nodiscard]] std::filesystem::path report() const { return root / "report.txt"; }

  /// @brief Creates the directory tree.
  /// @errors io.
  [[nodiscard]] Result<void> ensure() const;
};

/// @brief Progress sink; stages call it with one line per notable event.
using Log = std::function<void(std::string_view)>;

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
  const Workspace &ws, const snapshot::Client & /*c*/, const SelectOptions & /*o*/, const Log &
  /*log*/
);

// ---- resolve ----------------------------------------------------------------

struct ResolveOptions {
  std::uint32_t releases = 10; ///< Most recent distinct upstream releases per library.
  std::uint32_t max_scan = 40; ///< Archive versions to examine before giving up.
};

/// @brief Turns the selection into consecutive-release PairJobs.
/// @pre   ws.selection() exists.
/// @post  Writes ws.plan(). Pairs are (older, newer) by dpkg version order.
[[nodiscard]] Result<Plan> run_resolve(
  const Workspace &ws, const snapshot::Client & /*c*/, const ResolveOptions & /*o*/, const Log &
  /*log*/
);

// ---- diff -------------------------------------------------------------------

struct DiffOptions {
  std::uint32_t workers = 4; ///< Concurrent child processes.
  std::chrono::seconds pair_timeout{3600};
  bool index_headers = true; ///< Also build header indexes from the -dev trees.
  std::uint32_t header_max_files = 2000;
  bool trace = false; ///< Forwarded to abi::Options::trace.
  /// Pairs whose two extracted trees exceed this are recorded as skipped: DWARF
  /// of that size does not fit the memory budget of one reader.
  std::uint64_t max_extracted_mb = 2500;
  /// Address-space cap for each child process (prlimit --as); 0 disables.
  std::uint64_t child_memory_mb = 5000;
};

/// @brief Compares one PairJob in THIS process: materialises both releases,
///        pairs shared objects by SONAME stem, runs abi::compare on each, and
///        (optionally) indexes both releases' headers.
/// @post  Writes ws.pair(job.id()) and, if enabled, the two header indexes.
///        Scratch space is released before returning.
/// @errors Errors from materialisation are returned; a failing abi::compare on
///         one object is recorded inside the PairResult and is not an error.
[[nodiscard]] Result<PairResult> diff_one(
  const Workspace &ws, const snapshot::Client & /*c*/, const PairJob &job,
  const DiffOptions & /*o*/, const Log &
  /*log*/
);

/// @brief Runs diff_one for every job in ws.plan() that has no result yet, in
///        `workers` child processes (libabigail is not thread-safe).
/// @pre   `self_exe` is the path of this executable.
/// @post  Every job has a result file (possibly carrying an error string).
[[nodiscard]] Result<void> run_diff(
  const Workspace &ws, const std::filesystem::path &self_exe, const DiffOptions & /*o*/, const Log &
  /*log*/
);

// ---- headers ----------------------------------------------------------------

struct HeadersOptions {
  std::uint32_t max_files = 2000;
};

/// @brief Produces a HeaderResult per job from the per-release header indexes,
///        building any missing index from a fresh download of the -dev packages.
/// @post  Writes ws.header_pair(id) for every job.
[[nodiscard]] Result<void> run_headers(
  const Workspace &ws, const snapshot::Client & /*c*/, const HeadersOptions & /*o*/, const Log &
  /*log*/
);

// ---- analyze ----------------------------------------------------------------

/// @brief Aggregates pair and header results into summary.json and report.txt.
/// @post  Both files written. Returns the report text.
[[nodiscard]] Result<std::string> run_analyze(const Workspace &ws, const Log & /*log*/);

// ---- report -----------------------------------------------------------------

/// @brief Renders report.html from summary.json using the embedded template.
/// @pre   ws.summary() exists (run_analyze has run).
/// @post  ws.root/report.html written. Returns the HTML.
/// @errors io; schema if summary.json is from another tool version; parse if a
///         figure the template needs is absent.
[[nodiscard]] Result<std::string> run_report(const Workspace &ws, const Log & /*log*/);

} // namespace abistudy::pipeline
