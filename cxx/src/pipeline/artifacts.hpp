#pragma once
// =============================================================================
// The typed payloads that flow between pipeline stages, and their JSON forms.
//
//   select  -> Selection            (which libraries, and why)
//   resolve -> Plan                  (which consecutive releases to compare)
//   diff    -> PairResult per pair   (ABI events per shared object)
//   headers -> HeaderResult per pair (header-body events)
//   analyze -> Summary               (aggregates the report is built from)
//
// Every payload is wrapped by core/artifact.hpp with a schema id, listed here.
// =============================================================================

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "abi/compare.hpp"
#include "core/artifact.hpp"
#include "core/types.hpp"
#include "hdr/index.hpp"

namespace abistudy {

inline constexpr Schema schema_selection{"abistudy/selection/1"};
inline constexpr Schema schema_plan{"abistudy/plan/1"};
inline constexpr Schema schema_pair{"abistudy/pair/1"};
inline constexpr Schema schema_header_index{"abistudy/header-index/2"};
inline constexpr Schema schema_header_pair{"abistudy/header-pair/2"};
inline constexpr Schema schema_summary{"abistudy/summary/1"};

// ---- select -----------------------------------------------------------------

/// @brief One library chosen for the study, with the evidence for choosing it.
struct SelectedLibrary {
  PopconRank rank;
  InstallCount installs;
  BinaryName binary;      ///< The popcon-ranked runtime package.
  SourceName source;      ///< Its source package (the unit of study).
  Language language_hint; ///< From the archive Depends line; confirmed later from symbols.
};

struct Selection {
  std::vector<SelectedLibrary> libraries;
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
};

/// @brief Two consecutive releases of one source package.
struct PairJob {
  SourceName source;
  Release v1;
  Release v2;

  /// @brief Stable identifier "source@up1..up2", used as the artefact file stem.
  [[nodiscard]] std::string id() const;
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
  std::vector<abi::SharedObjectDiff> objects; ///< One per paired shared object.
  std::vector<std::string> unpaired_1;        ///< SONAME stems present only in v1.
  std::vector<std::string> unpaired_2;
  std::vector<std::string> object_errors; ///< Per-object abi::compare failures ("stem: reason").
  std::optional<std::string> error;       ///< Set if the pair could not be processed at all.
  double seconds = 0;
};

// ---- headers ----------------------------------------------------------------

/// @brief Result of the header stage for one PairJob.
struct HeaderResult {
  std::string id;
  SourceName source;
  UpstreamVersion upstream_1;
  UpstreamVersion upstream_2;
  Language language;
  std::optional<hdr::HeaderDiff> diff;
  hdr::ParseCoverage coverage_1;
  hdr::ParseCoverage coverage_2;
  std::optional<std::string> error;
};

// ---- JSON -------------------------------------------------------------------
// Hand-written rather than macro-generated so that the on-disk field names are
// a deliberate, stable interface and so that non-default-constructible Strong
// members can be read back.

void to_json(Json & /*j*/, const SelectedLibrary & /*x*/);
SelectedLibrary selected_library_from_json(const Json & /*j*/);
void to_json(Json & /*j*/, const Selection & /*x*/);
Selection selection_from_json(const Json & /*j*/);
void to_json(Json & /*j*/, const BinaryVersions & /*x*/);
BinaryVersions binary_versions_from_json(const Json & /*j*/);
void to_json(Json & /*j*/, const Release & /*x*/);
Release release_from_json(const Json & /*j*/);
void to_json(Json & /*j*/, const PairJob & /*x*/);
PairJob pair_job_from_json(const Json & /*j*/);
void to_json(Json & /*j*/, const Plan & /*x*/);
Plan plan_from_json(const Json & /*j*/);
void to_json(Json & /*j*/, const PairResult & /*x*/);
PairResult pair_result_from_json(const Json & /*j*/);
void to_json(Json & /*j*/, const HeaderResult & /*x*/);
HeaderResult header_result_from_json(const Json & /*j*/);

/// @brief Parses a Language name written by to_string(Language).
[[nodiscard]] Language language_from_string(std::string_view s) noexcept;

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
#undef ABISTUDY_ADL_SERIALIZER
