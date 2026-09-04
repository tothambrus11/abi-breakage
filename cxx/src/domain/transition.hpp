#pragma once
// =============================================================================
// One release transition after rolling its shared objects and header diff
// together, under the two break definitions of REVIEW.md:
//
//   strict  -- every public event counts (the previous definition);
//   lenient -- a layout event on a type clients never hold by value counts
//              only if it is not append-only, and a removed or re-signed
//              symbol counts only if the public headers declared it.
//
// The true break rate lies between the two; everything downstream is
// computed for both. Pure functions over records.
// =============================================================================

#include <cstdint>
#include <optional>
#include <set>
#include <string>

#include "core/types.hpp"
#include "domain/header_model.hpp"
#include "domain/records.hpp"
#include "domain/taxonomy.hpp"
#include "domain/version.hpp"

namespace abistudy {

/// @brief Where the pair's removed / re-signed symbols fall (REVIEW.md §1.4).
struct SymbolStrata {
  std::uint32_t removed_declared = 0;
  std::uint32_t removed_undeclared = 0;
  std::uint32_t removed_unknown = 0;
  std::uint32_t signature_declared = 0;
  std::uint32_t signature_undeclared = 0;
  std::uint32_t signature_unknown = 0;
  std::uint32_t layout_events_excluded = 0; ///< Public layout events dropped by the lenient rule.
};

struct Transition {
  std::string id;
  SourceName source;
  Language language;
  double mangled_fraction = 0; ///< Max over the pair's objects.
  ReleaseLevel level = ReleaseLevel::other;
  ChangeCounts strict;  ///< Public ABI events plus header kinds.
  ChangeCounts lenient; ///< Same after the exposure and declared-symbol filters.
  ChangeCounts third_party;
  ChangeCounts private_node;
  ChangeCounts vague_linkage;
  std::uint32_t layout_types_strict = 0;  ///< Public types with any layout event.
  std::uint32_t layout_types_lenient = 0; ///< ...whose change breaks leniently.
  bool soname_changed = false;
  bool mass_rename = false;
  bool debug_info_complete = true; ///< DWARF present on both sides of every object.
  std::optional<HeaderDiff> headers;
  bool header_coverage_poor = false;
  SymbolStrata symbols;
};

/// @brief Rolls a pair result (and its header result, if any) into a Transition.
[[nodiscard]] Transition rollup(const PairResult &p, const HeaderResult *h);

/// @brief Which changes a framing considers.
enum class Framing : std::uint8_t {
  binary,              ///< is_binary_breaking
  evolution,           ///< + enum cases
  evolution_or_inline, ///< + changed inline bodies
};
inline constexpr std::array all_framings = {
  Framing::binary, Framing::evolution, Framing::evolution_or_inline
};
[[nodiscard]] constexpr std::string_view to_string(Framing f) noexcept {
  switch (f) {
  case Framing::binary:
    return "binary";
  case Framing::evolution:
    return "evolution";
  case Framing::evolution_or_inline:
    return "evolution_or_inline";
  }
  return "?";
}
[[nodiscard]] constexpr bool in_framing(Framing f, ChangeKind k) noexcept {
  switch (f) {
  case Framing::binary:
    return is_binary_breaking(k);
  case Framing::evolution:
    return is_evolution_relevant(k);
  case Framing::evolution_or_inline:
    return is_evolution_or_inline(k);
  }
  return false;
}

enum class BreakDefinition : std::uint8_t { strict, lenient };
inline constexpr std::array all_definitions = {BreakDefinition::strict, BreakDefinition::lenient};
[[nodiscard]] constexpr std::string_view to_string(BreakDefinition d) noexcept {
  return d == BreakDefinition::strict ? "strict" : "lenient";
}

[[nodiscard]] inline const ChangeCounts &counts_of(
  const Transition &t, BreakDefinition d
) noexcept {
  return d == BreakDefinition::strict ? t.strict : t.lenient;
}

/// @brief Kinds present in `t` (under `d`) that `f` considers.
[[nodiscard]] std::set<ChangeKind> relevant_kinds(
  const Transition &t, Framing f, BreakDefinition d
);

/// @brief True if `t` has at least one relevant kind.
[[nodiscard]] bool is_affected(const Transition &t, Framing f, BreakDefinition d);

/// @brief True if every kind in the set is absorbed by some mechanism.
[[nodiscard]] bool fully_absorbable(const std::set<ChangeKind> &kinds);

} // namespace abistudy
