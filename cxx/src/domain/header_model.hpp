#pragma once
// =============================================================================
// What the header stage measures: definitions C and C++ copy into every
// client (inline functions, in-class methods, templates), object-like macro
// values, and the set of symbols the public headers declare. Pure data plus
// the comparison of two indexes; producing an index is a HeaderIndexer port.
// =============================================================================

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/types.hpp"

namespace abistudy {

/// @brief One client-visible definition found in a header.
struct Definition {
  std::string relative_path;    ///< Header path relative to the include root.
  std::string name;             ///< Spelling of the declaration.
  std::string kind;             ///< Cursor kind name ("CXXMethod", "FunctionTemplate", ...).
  std::string decl_fingerprint; ///< Hash of name + result type + parameter types.
  std::string body_fingerprint; ///< Hash of the body's token spellings.
  std::uint32_t body_tokens{};  ///< Token count of the body (size proxy).
};

/// @brief How much of the header set was actually parsed. Consumers must
///        treat counts from a release with poor coverage as a lower bound.
struct ParseCoverage {
  std::uint32_t header_files = 0;     ///< Headers found under the root.
  std::uint32_t parsed = 0;           ///< Translation units created.
  std::uint32_t with_fatal_error = 0; ///< TUs that hit a fatal diagnostic.
  std::uint32_t with_errors = 0;      ///< TUs with at least one error-level diagnostic.
  std::uint32_t parsed_as_cxx = 0;    ///< TUs parsed as C++ (per-file choice).
  std::uint32_t skipped_by_limit = 0; ///< Headers not parsed because of a file cap.

  /// @brief More than half of the units hit an error: counts are lower bounds.
  [[nodiscard]] bool poor() const noexcept {
    return parsed != 0 && std::max(with_errors, with_fatal_error) * 2 > parsed;
  }
};

/// @brief Everything indexed for one release's public headers.
/// @invariant Keys of `definitions` are USRs (or "path:name" when the USR is
///            empty), unique per definition. `declared_symbols` holds the
///            mangled (ELF) names of every function and variable the headers
///            declare, in any file, so exported symbols can be classified as
///            declared or not (REVIEW.md §1.4).
struct HeaderIndex {
  Language language = Language::unknown;
  std::unordered_map<std::string, Definition> definitions;
  std::map<std::string, std::string> macros; ///< "rel/path.h::NAME" -> value fingerprint
  std::unordered_set<std::string> declared_symbols;
  ParseCoverage coverage;
};

/// @brief Knobs for a HeaderIndexer.
struct IndexOptions {
  /// Default parse language. A header is parsed as C++ regardless when its
  /// extension is C++-only or its text contains C++ constructs.
  Language language = Language::cxx;
  std::uint32_t max_files = 2000;      ///< Cap per release; excess is counted in coverage.
  std::vector<std::string> extra_args; ///< Additional compiler flags.
};

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

  friend bool operator==(const HeaderDiff &, const HeaderDiff &) = default;
};

/// @brief Version/build stamps change every release by construction.
[[nodiscard]] bool looks_like_version_macro(std::string_view key);

/// @brief Compares two indexes by USR.
/// @post  inline_body_changed <= definitions_common; macro_value_changed <= macros_common.
[[nodiscard]] HeaderDiff compare_headers(const HeaderIndex &a, const HeaderIndex &b);

/// @brief Whether a symbol is declared by a header set, or undecidable.
enum class Declared : std::uint8_t { yes, no, unknown };
[[nodiscard]] constexpr std::string_view to_string(Declared d) noexcept {
  switch (d) {
  case Declared::yes:
    return "declared";
  case Declared::no:
    return "undeclared";
  case Declared::unknown:
    return "unknown";
  }
  return "unknown";
}

/// @brief Classifies an ELF symbol name against a release's headers. `unknown`
///        when the index declares nothing at all (parse failed outright) or
///        its coverage is poor, so absence proves nothing.
[[nodiscard]] Declared symbol_declared(const HeaderIndex &idx, std::string_view symbol);

void to_json(nlohmann::json &j, const Definition &x);
void from_json(const nlohmann::json &j, Definition &x);
void to_json(nlohmann::json &j, const ParseCoverage &c);
void from_json(const nlohmann::json &j, ParseCoverage &c);
void to_json(nlohmann::json &j, const HeaderIndex &x);
void from_json(const nlohmann::json &j, HeaderIndex &x);
void to_json(nlohmann::json &j, const HeaderDiff &d);
void from_json(const nlohmann::json &j, HeaderDiff &d);

} // namespace abistudy
