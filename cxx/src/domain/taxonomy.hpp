#pragma once
// =============================================================================
// The change taxonomy and its mapping onto resilience mechanisms. This is the
// vocabulary the whole study is expressed in: adapters produce ChangeKind
// values, the domain aggregates them, reports consume them. Nothing else in
// the code base defines what a "break" or a "mechanism" is.
// =============================================================================

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/types.hpp"

namespace abistudy {

/// @brief One kind of observable change between two releases of a library.
///        Layout kinds are counted per affected TYPE; symbol kinds per SYMBOL;
///        header kinds per DEFINITION.
enum class ChangeKind : std::uint8_t {
  // -- layout (visible in DWARF)
  field_added_to_struct,
  field_removed_from_struct,
  field_type_changed,
  member_offset_changed,
  type_size_changed,
  base_class_changed,
  // -- enums (visible in DWARF)
  enum_case_added,
  enum_case_removed,
  // -- dispatch (visible in DWARF): a virtual slot inserted, moved, or a
  //    method changing virtuality. A REMOVED virtual is a symbol removal.
  vtable_changed,
  // -- symbols (visible in ELF)
  function_signature_changed,
  symbol_removed,
  symbol_added,
  symbol_version_renamed,
  // -- header bodies (invisible to every ABI tool)
  inline_body_changed,
  macro_value_changed,
};

/// @brief All kinds, in report order.
inline constexpr std::array all_change_kinds = {
  ChangeKind::field_added_to_struct,  ChangeKind::field_removed_from_struct,
  ChangeKind::field_type_changed,     ChangeKind::member_offset_changed,
  ChangeKind::type_size_changed,      ChangeKind::base_class_changed,
  ChangeKind::enum_case_added,        ChangeKind::enum_case_removed,
  ChangeKind::vtable_changed,         ChangeKind::function_signature_changed,
  ChangeKind::symbol_removed,         ChangeKind::symbol_added,
  ChangeKind::symbol_version_renamed, ChangeKind::inline_body_changed,
  ChangeKind::macro_value_changed,
};

/// @brief Stable snake_case name, used in JSON and reports.
/// @post  Never null. Round-trips through parse_change_kind.
[[nodiscard]] constexpr std::string_view to_string(ChangeKind k) noexcept {
  switch (k) {
  case ChangeKind::field_added_to_struct:
    return "field_added_to_struct";
  case ChangeKind::field_removed_from_struct:
    return "field_removed_from_struct";
  case ChangeKind::field_type_changed:
    return "field_type_changed";
  case ChangeKind::member_offset_changed:
    return "member_offset_changed";
  case ChangeKind::type_size_changed:
    return "type_size_changed";
  case ChangeKind::base_class_changed:
    return "base_class_changed";
  case ChangeKind::enum_case_added:
    return "enum_case_added";
  case ChangeKind::enum_case_removed:
    return "enum_case_removed";
  case ChangeKind::vtable_changed:
    return "vtable_changed";
  case ChangeKind::function_signature_changed:
    return "function_signature_changed";
  case ChangeKind::symbol_removed:
    return "symbol_removed";
  case ChangeKind::symbol_added:
    return "symbol_added";
  case ChangeKind::symbol_version_renamed:
    return "symbol_version_renamed";
  case ChangeKind::inline_body_changed:
    return "inline_body_changed";
  case ChangeKind::macro_value_changed:
    return "macro_value_changed";
  }
  return "?";
}

/// @brief Inverse of to_string.
/// @returns nullopt for an unknown name.
[[nodiscard]] constexpr std::optional<ChangeKind> parse_change_kind(std::string_view s) noexcept {
  for (const auto k : all_change_kinds) {
    if (to_string(k) == s)
      return k;
  }
  return std::nullopt;
}

/// @brief Kinds that describe the memory layout of a type.
[[nodiscard]] constexpr bool is_layout_kind(ChangeKind k) noexcept {
  switch (k) {
  case ChangeKind::field_added_to_struct:
  case ChangeKind::field_removed_from_struct:
  case ChangeKind::field_type_changed:
  case ChangeKind::member_offset_changed:
  case ChangeKind::type_size_changed:
  case ChangeKind::base_class_changed:
    return true;
  default:
    return false;
  }
}

/// @brief Kinds that come from the header stage, not from DWARF/ELF.
[[nodiscard]] constexpr bool is_header_kind(ChangeKind k) noexcept {
  return k == ChangeKind::inline_body_changed || k == ChangeKind::macro_value_changed;
}

/// @brief Does this change break an ALREADY-COMPILED client at load/call time?
///        This is what abidiff means by an ABI break. Enum-case changes move no
///        byte and are therefore not binary breaks; header-body changes are
///        breaks only for clients that inlined the old body.
[[nodiscard]] constexpr bool is_binary_breaking(ChangeKind k) noexcept {
  switch (k) {
  case ChangeKind::field_added_to_struct:
  case ChangeKind::field_removed_from_struct:
  case ChangeKind::field_type_changed:
  case ChangeKind::member_offset_changed:
  case ChangeKind::type_size_changed:
  case ChangeKind::base_class_changed:
  case ChangeKind::vtable_changed:
  case ChangeKind::function_signature_changed:
  case ChangeKind::symbol_removed:
    return true;
  default:
    return false;
  }
}

/// @brief Must a boundary tolerate this change for a client to keep working
///        WITHOUT recompilation? Superset of is_binary_breaking: adds enum
///        cases (exhaustive switches).
[[nodiscard]] constexpr bool is_evolution_relevant(ChangeKind k) noexcept {
  return is_binary_breaking(k) || k == ChangeKind::enum_case_added ||
         k == ChangeKind::enum_case_removed;
}

/// @brief The widest framing: evolution-relevant plus changed inline bodies
///        (stale copies in clients). Macro values are deliberately NOT here:
///        a changed value is a hazard only for the few macros that encode
///        layout or behaviour, and version stamps dominate the count.
[[nodiscard]] constexpr bool is_evolution_or_inline(ChangeKind k) noexcept {
  return is_evolution_relevant(k) || k == ChangeKind::inline_body_changed;
}

/// @brief The resilience mechanism that would make a change invisible to
///        clients, if any.
enum class Mechanism : std::uint8_t {
  opaque_layout,        ///< Offsets and sizes fetched at run time.
  non_frozen_enum,      ///< Clients carry a default path for unknown cases.
  resilient_dispatch,   ///< Method slots resolved indirectly.
  no_implicit_inlining, ///< Bodies cross the boundary only when opted in.
  none,                 ///< Source-level API change; no indirection helps.
  not_applicable,       ///< Additive or policy-driven; already compatible.
};

/// @brief The four mechanisms, in report order.
inline constexpr std::array all_mechanisms = {
  Mechanism::opaque_layout,
  Mechanism::non_frozen_enum,
  Mechanism::resilient_dispatch,
  Mechanism::no_implicit_inlining,
};

/// @brief Human name for reports.
[[nodiscard]] constexpr std::string_view to_string(Mechanism m) noexcept {
  switch (m) {
  case Mechanism::opaque_layout:
    return "opaque layout";
  case Mechanism::non_frozen_enum:
    return "non-frozen enum";
  case Mechanism::resilient_dispatch:
    return "resilient dispatch";
  case Mechanism::no_implicit_inlining:
    return "no implicit cross-module inlining";
  case Mechanism::none:
    return "cannot help";
  case Mechanism::not_applicable:
    return "n/a";
  }
  return "?";
}

/// @brief Which mechanism absorbs `k`. Total function over ChangeKind.
///
///        A resilient layout resolves offsets and sizes at run time, so it
///        absorbs additions, moves and growth -- but a REMOVED or RETYPED
///        field has no accessor left for an old client to call, and a
///        REMOVED enum case is an API removal. Those are `none`, as in the
///        Swift library-evolution model these mechanisms are taken from.
[[nodiscard]] constexpr Mechanism mechanism_for(ChangeKind k) noexcept {
  switch (k) {
  case ChangeKind::field_added_to_struct:
  case ChangeKind::member_offset_changed:
  case ChangeKind::type_size_changed:
  case ChangeKind::base_class_changed:
    return Mechanism::opaque_layout;
  case ChangeKind::enum_case_added:
    return Mechanism::non_frozen_enum;
  case ChangeKind::vtable_changed:
    return Mechanism::resilient_dispatch;
  case ChangeKind::inline_body_changed:
  case ChangeKind::macro_value_changed:
    return Mechanism::no_implicit_inlining;
  case ChangeKind::field_removed_from_struct:
  case ChangeKind::field_type_changed:
  case ChangeKind::enum_case_removed:
  case ChangeKind::function_signature_changed:
  case ChangeKind::symbol_removed:
    return Mechanism::none;
  case ChangeKind::symbol_added:
  case ChangeKind::symbol_version_renamed:
    return Mechanism::not_applicable;
  }
  return Mechanism::none;
}

/// @brief True if a resilient boundary absorbs `k`.
[[nodiscard]] constexpr bool is_absorbed(ChangeKind k) noexcept {
  const auto m = mechanism_for(k);
  return m != Mechanism::none && m != Mechanism::not_applicable;
}

/// @brief Event counts per kind for one library or one transition.
/// @invariant Absent key == zero. Never stores a zero count.
class ChangeCounts {
public:
  /// @brief Adds `n` events of kind `k` (no-op for n == 0).
  void add(ChangeKind k, std::uint32_t n = 1) {
    if (n)
      counts_[k] += n;
  }

  /// @brief Removes up to `n` events of kind `k`; the key disappears at zero.
  void subtract(ChangeKind k, std::uint32_t n) {
    const auto it = counts_.find(k);
    if (it == counts_.end())
      return;
    if (it->second <= n) {
      counts_.erase(it);
    } else {
      it->second -= n;
    }
  }

  /// @brief Merges another tally into this one.
  void merge(const ChangeCounts &o) {
    for (const auto &[k, n] : o.counts_)
      add(k, n);
  }

  /// @brief Events of kind `k`, 0 if none.
  [[nodiscard]] std::uint32_t get(ChangeKind k) const noexcept {
    const auto it = counts_.find(k);
    return it == counts_.end() ? 0 : it->second;
  }

  /// @brief True if any event of kind `k` was recorded.
  [[nodiscard]] bool has(ChangeKind k) const noexcept { return counts_.contains(k); }

  /// @brief True if no events at all.
  [[nodiscard]] bool empty() const noexcept { return counts_.empty(); }

  /// @brief Sum over all kinds.
  [[nodiscard]] std::uint64_t total() const noexcept {
    std::uint64_t n = 0;
    for (const auto &[k, v] : counts_)
      n += v;
    return n;
  }

  /// @brief Iteration over (kind, count) pairs with count > 0.
  [[nodiscard]] const std::map<ChangeKind, std::uint32_t> &items() const noexcept {
    return counts_;
  }

  friend bool operator==(const ChangeCounts &, const ChangeCounts &) = default;

private:
  std::map<ChangeKind, std::uint32_t> counts_;
};

/// @brief JSON: an object keyed by kind name with integer counts.
void to_json(nlohmann::json &j, const ChangeCounts &c);
/// @brief Inverse; unknown kind names are a parse error.
/// @errors Throws nlohmann::json::other_error for an unknown kind (caught at the artefact
/// boundary).
void from_json(const nlohmann::json &j, ChangeCounts &c);

} // namespace abistudy
