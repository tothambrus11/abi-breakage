#pragma once
// =============================================================================
// Header indexing through the libclang C API. Measures what no ABI tool can:
// changes to the bodies C and C++ copy into every client -- header `inline`
// functions, in-class methods, templates -- and to object-like macro values.
// =============================================================================

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/error.hpp"
#include "core/types.hpp"

namespace abistudy::hdr {

/// @brief One client-visible definition found in a header.
struct Definition {
  std::string relative_path; ///< Header path relative to the include root.
  std::string name;          ///< Spelling of the declaration.
  std::string kind;          ///< libclang cursor kind name ("CXXMethod", "FunctionTemplate", ...).
  std::string decl_fingerprint; ///< Hash of name + result type + parameter types.
  std::string body_fingerprint; ///< Hash of the body's token spellings.
  std::uint32_t body_tokens{};  ///< Token count of the body (size proxy).
};

/// @brief How much of the header set libclang actually parsed. Consumers must
///        treat counts from a release with poor coverage as a lower bound.
struct ParseCoverage {
  std::uint32_t header_files = 0;     ///< Headers found under the root.
  std::uint32_t parsed = 0;           ///< Translation units created.
  std::uint32_t with_fatal_error = 0; ///< TUs that hit a fatal diagnostic.
  std::uint32_t with_errors = 0;      ///< TUs with at least one error-level diagnostic (KeepGoing
                                      ///< downgrades a missing include to this class).
  std::uint32_t parsed_as_cxx = 0;    ///< TUs parsed as C++ (per-file choice, see index()).
  std::uint32_t skipped_by_limit = 0; ///< Headers not parsed because of Options::max_files.
};

/// @brief Everything indexed for one release's public headers.
/// @invariant Keys of `definitions` are libclang USRs (or "path:name" when the
///            USR is empty), unique per definition.
struct HeaderIndex {
  Language language = Language::unknown;
  std::unordered_map<std::string, Definition> definitions;
  std::map<std::string, std::string> macros; ///< "rel/path.h::NAME" -> value fingerprint
  ParseCoverage coverage;
};

/// @brief Knobs for index().
struct Options {
  /// Default parse language. A header is parsed as C++ regardless when its
  /// extension is C++-only or its text contains C++ constructs (`class`,
  /// `template <`, `namespace`); C libraries routinely ship such headers
  /// (z3's `z3++.h`).
  Language language = Language::cxx;
  std::uint32_t max_files = 2000;      ///< Cap per release; excess is counted in coverage.
  std::vector<std::string> extra_args; ///< Additional compiler flags (e.g. -D from a .pc file).
};

/// @brief Indexes every header under `include_root`.
/// @pre   `include_root` is an existing directory.
/// @post  coverage.header_files == parsed + failed-to-create + skipped_by_limit.
///        Definitions and macros from files OUTSIDE include_root (system
///        headers pulled in by #include) are never recorded.
/// @errors clang if libclang cannot create an index at all. Individual
///         translation-unit failures are NOT errors: they are counted in coverage.
/// @thread Safe to call concurrently with distinct CXIndex instances (one is
///         created per call).
[[nodiscard]] Result<HeaderIndex> index(
  const std::filesystem::path &include_root, const Options &opt = {}
);

/// @brief Differences between two releases' header indexes.
struct HeaderDiff {
  std::uint32_t definitions_1 = 0, definitions_2 = 0, definitions_common = 0;
  std::uint32_t inline_body_changed = 0; ///< Same USR, different body fingerprint.
  std::uint32_t inline_body_changed_template = 0;
  std::uint32_t inline_decl_changed = 0; ///< Same USR, different declaration fingerprint.
  std::uint32_t definitions_added = 0, definitions_removed = 0;
  std::uint32_t macros_1 = 0, macros_2 = 0, macros_common = 0;
  std::uint32_t macro_value_changed = 0;
  /// Same, excluding names that look like version/build stamps (VERSION, _DATE,
  /// BUILD, REVISION, ...): those change every release by construction and a
  /// client that inlined them is not broken.
  std::uint32_t macro_value_changed_nonversion = 0;
  std::vector<std::string> examples; ///< Up to 8 "path::name" of changed bodies.
};

/// @brief Compares two indexes by USR.
/// @post  inline_body_changed <= definitions_common; macro_value_changed <= macros_common.
[[nodiscard]] HeaderDiff compare(const HeaderIndex &a, const HeaderIndex &b);

void to_json(nlohmann::json & /*j*/, const Definition & /*x*/);
void from_json(const nlohmann::json & /*j*/, Definition & /*x*/);
void to_json(nlohmann::json & /*j*/, const ParseCoverage & /*c*/);
void from_json(const nlohmann::json & /*j*/, ParseCoverage & /*c*/);
void to_json(nlohmann::json & /*j*/, const HeaderIndex & /*x*/);
void from_json(const nlohmann::json & /*j*/, HeaderIndex & /*x*/);
void to_json(nlohmann::json & /*j*/, const HeaderDiff & /*d*/);
void from_json(const nlohmann::json & /*j*/, HeaderDiff & /*d*/);

} // namespace abistudy::hdr
