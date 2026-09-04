#pragma once
// =============================================================================
// The study's records: the typed payloads that flow between stages and are
// persisted as artefacts. Every payload has a schema id; a stage refuses an
// artefact whose schema differs instead of silently misreading it.
//
//   select  -> Selection            (which libraries, and why)
//   resolve -> Plan                  (which consecutive releases to compare)
//   diff    -> PairResult per pair   (ABI events per shared object)
//   headers -> HeaderResult per pair (header events, declared-symbol join)
//   analyze -> summary               (aggregates the reports are built from)
// =============================================================================

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/types.hpp"
#include "domain/events.hpp"
#include "domain/header_model.hpp"

namespace abistudy {

using Json = nlohmann::json;

/// @brief Identifies the layout of an artefact's payload.
/// @invariant `id` looks like "abistudy/<stage>/<positive integer>".
struct Schema {
  std::string_view id;
};

inline constexpr Schema schema_selection{"abistudy/selection/2"};
inline constexpr Schema schema_plan{"abistudy/plan/2"};
inline constexpr Schema schema_pair{"abistudy/pair/2"};
inline constexpr Schema schema_header_index{"abistudy/header-index/3"};
inline constexpr Schema schema_header_pair{"abistudy/header-pair/3"};
inline constexpr Schema schema_summary{"abistudy/summary/2"};

// ---- select -----------------------------------------------------------------

/// @brief One library chosen for the study, with the evidence for choosing it.
struct SelectedLibrary {
  PopconRank rank;
  InstallCount installs;
  BinaryName binary;      ///< The popcon-ranked runtime package.
  SourceName source;      ///< Its source package (the unit of study).
  Language language_hint; ///< From the archive Depends line; confirmed later from symbols.
};

/// @brief What the selection filters did, so the corpus can be described and
///        reproduced (REVIEW.md §3.1).
struct SelectionProvenance {
  std::string popcon_sha1;
  std::string packages_sha1;
  std::uint32_t candidates_examined = 0;
  std::uint32_t rejected_no_dbgsym = 0;
  std::uint32_t rejected_no_dev = 0;
  std::uint32_t rejected_quota = 0;
};

struct Selection {
  std::vector<SelectedLibrary> libraries;
  SelectionProvenance provenance;
};

// ---- resolve ----------------------------------------------------------------

/// @brief Binary versions available for one binary package in one source version.
struct BinaryVersions {
  BinaryName name;
  std::vector<VersionString> versions; ///< Newest first.
};

/// @brief One release of a source package with the packages we need from it.
struct Release {
  UpstreamVersion upstream;
  VersionString source_version;
  std::vector<BinaryName> runtime;      ///< Shared-library packages (have a -dbgsym).
  std::vector<BinaryName> dev;          ///< Header packages.
  std::vector<BinaryVersions> binaries; ///< Versions for runtime, dev and dbgsym packages.
  std::uint64_t download_bytes = 0;     ///< Sum of the .deb sizes to fetch; 0 if unknown.
};

/// @brief Two consecutive releases of one source package.
struct PairJob {
  SourceName source;
  Release v1;
  Release v2;

  /// @brief Stable identifier "source@up1..up2", used as the artefact file stem.
  [[nodiscard]] std::string id() const;
  /// @brief Bytes both releases download; drives scheduling and budgets.
  [[nodiscard]] std::uint64_t download_bytes() const noexcept {
    return v1.download_bytes + v2.download_bytes;
  }
};

struct Plan {
  std::vector<PairJob> jobs;
};

// ---- diff -------------------------------------------------------------------

/// @brief Result of the diff stage for one PairJob.
struct PairResult {
  std::string id;
  SourceName source;
  UpstreamVersion upstream_1;
  UpstreamVersion upstream_2;
  std::vector<SharedObjectDiff> objects; ///< One per paired shared object.
  std::vector<std::string> unpaired_1;   ///< SONAME stems present only in v1.
  std::vector<std::string> unpaired_2;
  std::vector<std::string> object_errors; ///< Per-object compare failures ("stem: reason").
  std::optional<std::string> error;       ///< Set if the pair could not be processed at all.
  double seconds = 0;
  std::uint64_t bytes_extracted = 0;
  /// lib*.so files found outside a linkable directory (plugins), both
  /// releases, relative to the runtime package root: what the rule dropped.
  std::vector<std::string> excluded_objects;
};

/// @brief Why a PairResult carries no comparison, or that it carries one.
enum class PairOutcome : std::uint8_t {
  compared,           ///< At least one shared object was compared.
  no_linkable_object, ///< Both releases materialised, nothing to compare (plugins only, no
                      ///< lib*.so).
  skipped_budget,     ///< Recorded as too large for --max-extracted-mb before download.
  not_attempted,      ///< Deferred by --deadline-minutes.
  failed_memory,      ///< Child killed by a signal or bad_alloc: needs a larger cap.
  failed_timeout,     ///< Child exceeded --pair-timeout: needs more time.
  failed,             ///< Any other failure (reader error, missing package, spawn).
};

/// Prefixes of the `error` strings the diff stage writes. pair_outcome()
/// classifies by them, so the two never drift.
inline constexpr std::string_view pair_error_skipped = "skipped: ";
inline constexpr std::string_view pair_error_not_attempted = "not attempted: ";
inline constexpr std::string_view pair_error_timeout = "timeout after ";
inline constexpr std::string_view pair_error_killed = "killed by signal";
inline constexpr std::string_view pair_error_exit = "exit ";

/// @brief Classifies a record; pure, total.
[[nodiscard]] PairOutcome pair_outcome(const PairResult &r) noexcept;
[[nodiscard]] constexpr const char *to_string(PairOutcome o) noexcept {
  switch (o) {
  case PairOutcome::compared:
    return "compared";
  case PairOutcome::no_linkable_object:
    return "no_linkable_object";
  case PairOutcome::skipped_budget:
    return "skipped_budget";
  case PairOutcome::not_attempted:
    return "not_attempted";
  case PairOutcome::failed_memory:
    return "failed_memory";
  case PairOutcome::failed_timeout:
    return "failed_timeout";
  case PairOutcome::failed:
    return "failed";
  }
  return "failed";
}
/// @brief True for outcomes a larger memory cap or timeout can change.
[[nodiscard]] constexpr bool retryable_with_more_resources(PairOutcome o) noexcept {
  return o == PairOutcome::failed_memory || o == PairOutcome::failed_timeout;
}

// ---- headers ----------------------------------------------------------------

/// @brief Result of the header stage for one PairJob.
struct HeaderResult {
  std::string id;
  SourceName source;
  UpstreamVersion upstream_1;
  UpstreamVersion upstream_2;
  Language language;
  std::optional<HeaderDiff> diff;
  ParseCoverage coverage_1;
  ParseCoverage coverage_2;
  std::optional<std::string> error;
  /// The pair's removed / signature-changed symbols classified against the
  /// OLD release's headers (the only join the roll-up consumes).
  std::map<std::string, Declared> symbol_declared;
};

// ---- JSON -------------------------------------------------------------------
// Hand-written rather than macro-generated so that the on-disk field names are
// a deliberate, stable interface and so that non-default-constructible Strong
// members can be read back.

void to_json(Json &j, const SelectedLibrary &x);
[[nodiscard]] SelectedLibrary selected_library_from_json(const Json &j);
void to_json(Json &j, const SelectionProvenance &x);
void from_json(const Json &j, SelectionProvenance &x);
void to_json(Json &j, const Selection &x);
[[nodiscard]] Selection selection_from_json(const Json &j);
void to_json(Json &j, const BinaryVersions &x);
[[nodiscard]] BinaryVersions binary_versions_from_json(const Json &j);
void to_json(Json &j, const Release &x);
[[nodiscard]] Release release_from_json(const Json &j);
void to_json(Json &j, const PairJob &x);
[[nodiscard]] PairJob pair_job_from_json(const Json &j);
void to_json(Json &j, const Plan &x);
[[nodiscard]] Plan plan_from_json(const Json &j);
void to_json(Json &j, const PairResult &x);
[[nodiscard]] PairResult pair_result_from_json(const Json &j);
void to_json(Json &j, const HeaderResult &x);
[[nodiscard]] HeaderResult header_result_from_json(const Json &j);

} // namespace abistudy

/// @brief nlohmann glue for the non-default-constructible payloads above.
#define ABISTUDY_ADL_SERIALIZER(Type, from_fn)                                                     \
  template <>                                                                                      \
  struct nlohmann::adl_serializer<abistudy::Type> {                                                \
    static void to_json(json &j, const abistudy::Type &v) { abistudy::to_json(j, v); }             \
    static abistudy::Type from_json(const json &j) { return abistudy::from_fn(j); }                \
  };
ABISTUDY_ADL_SERIALIZER(SelectedLibrary, selected_library_from_json)
ABISTUDY_ADL_SERIALIZER(Selection, selection_from_json)
ABISTUDY_ADL_SERIALIZER(BinaryVersions, binary_versions_from_json)
ABISTUDY_ADL_SERIALIZER(Release, release_from_json)
ABISTUDY_ADL_SERIALIZER(PairJob, pair_job_from_json)
ABISTUDY_ADL_SERIALIZER(Plan, plan_from_json)
ABISTUDY_ADL_SERIALIZER(PairResult, pair_result_from_json)
ABISTUDY_ADL_SERIALIZER(HeaderResult, header_result_from_json)
ABISTUDY_ADL_SERIALIZER(SharedObjectDiff, shared_object_diff_from_json)
#undef ABISTUDY_ADL_SERIALIZER
