#pragma once
// =============================================================================
// The events an ABI comparison produces for one pair of shared objects, and
// the per-event facts the two break definitions (REVIEW.md §1.1, §1.4) need:
// how a changed type is exposed through the exported interface, whether a
// layout change is append-only, and whether a symbol has vague linkage.
// Pure data; produced by an AbiComparer adapter, consumed by the domain.
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/types.hpp"
#include "domain/taxonomy.hpp"

namespace abistudy {

/// @brief How a public type reaches clients through the library's own
///        EXPORTED interface (functions and variables with ELF symbols).
enum class TypeExposure : std::uint8_t {
  by_value,         ///< Appears by value (or as an array element) in a signature.
  by_pointer,       ///< Reached only through pointers/references.
  not_in_interface, ///< Declared in a public header but in no exported signature.
  unknown,          ///< No debug info to decide.
};

[[nodiscard]] constexpr std::string_view to_string(TypeExposure e) noexcept {
  switch (e) {
  case TypeExposure::by_value:
    return "by_value";
  case TypeExposure::by_pointer:
    return "by_pointer";
  case TypeExposure::not_in_interface:
    return "not_in_interface";
  case TypeExposure::unknown:
    return "unknown";
  }
  return "unknown";
}
[[nodiscard]] TypeExposure parse_type_exposure(std::string_view s) noexcept;

/// @brief One type-level event with the header that declared the type.
struct TypeEvent {
  ChangeKind kind;
  std::string type_name;   ///< e.g. "struct Point"
  std::string declared_in; ///< Path from DWARF; empty if unknown.
  std::uint32_t count = 0; ///< Events of this kind on this type.
  TypeExposure exposure = TypeExposure::unknown;
  bool third_party = false; ///< Declared outside the library's own headers.
  /// True iff every inserted member starts at or beyond the old size and
  /// nothing was removed, retyped or moved: growth an opaque-by-convention
  /// client (pointer only, library-allocated) never notices.
  bool append_only = false;
};

/// @brief One symbol-level event, kept so a reviewer can trace a count.
struct SymbolEvent {
  ChangeKind kind;
  SymbolName symbol;
  std::string pretty; ///< Demangled signature as the reader prints it.
  std::optional<VersionNode> version;
  bool weak = false; ///< STB_WEAK binding.
};

/// @brief Coverage facts a reviewer needs to trust the counts.
struct Coverage {
  bool debug_info_found_1 = false;
  bool debug_info_found_2 = false;
  std::uint32_t exported_functions_1 = 0;
  std::uint32_t exported_functions_2 = 0;
  std::uint32_t mangled_functions_1 = 0;
  std::uint32_t mangled_functions_2 = 0;

  [[nodiscard]] bool debug_info_complete() const noexcept {
    return debug_info_found_1 && debug_info_found_2;
  }
  /// @brief Fraction of exported functions with Itanium-mangled names (0 if none).
  [[nodiscard]] double mangled_fraction() const noexcept {
    const double ex = exported_functions_1 + exported_functions_2;
    const double mg = mangled_functions_1 + mangled_functions_2;
    return ex == 0 ? 0.0 : mg / ex;
  }
};

/// @brief Result of comparing one pair of shared objects.
/// @invariant The four ChangeCounts partition every event: a type event goes
///            to third_party if its declaring header is not the library's
///            own; a symbol event goes to private_node if its ELF version
///            node is marked PRIVATE/INTERNAL, to vague_linkage if it is a
///            weak C++ symbol (template instantiation / inline emitted by
///            every client itself); all else is public.
struct SharedObjectDiff {
  Soname soname_1;
  Soname soname_2;
  Language language;
  ChangeCounts public_counts;
  ChangeCounts third_party_counts;
  ChangeCounts private_node_counts;
  ChangeCounts vague_linkage_counts;
  std::uint32_t symbols_version_renamed = 0; ///< Digits-blind removed/added matches.
  bool mass_rename = false; ///< renamed >= threshold and >= all other symbol events.
  Coverage coverage;
  std::vector<TypeEvent> type_events;     ///< Every counted type event (public + third party).
  std::vector<SymbolEvent> symbol_events; ///< Every counted public/private symbol event.
};

/// @brief Is a weak symbol of this name one every client emits itself?
///        Vague linkage exists only for C++ (templates, inline functions).
[[nodiscard]] bool is_vague_linkage(std::string_view linkage_name, bool weak) noexcept;

/// @brief The lenient break definition for a layout event: a change to a type
///        clients never hold by value, that only appends, breaks no compiled
///        client that treats the type as opaque.
[[nodiscard]] bool layout_event_breaks_leniently(const TypeEvent &e) noexcept;

void to_json(nlohmann::json &j, const Coverage &c);
void to_json(nlohmann::json &j, const TypeEvent &e);
void to_json(nlohmann::json &j, const SymbolEvent &e);
void to_json(nlohmann::json &j, const SharedObjectDiff &d);
[[nodiscard]] Coverage coverage_from_json(const nlohmann::json &j);
[[nodiscard]] TypeEvent type_event_from_json(const nlohmann::json &j);
[[nodiscard]] SymbolEvent symbol_event_from_json(const nlohmann::json &j);
[[nodiscard]] SharedObjectDiff shared_object_diff_from_json(const nlohmann::json &j);

} // namespace abistudy
