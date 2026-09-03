#pragma once
// =============================================================================
// ABI comparison of two shared objects through the libabigail library API.
//
// The previous generation of this pipeline ran `abidiff` and parsed its text
// report. That coupled the study to a human-oriented format that changes
// between libabigail releases and hides information (e.g. which header
// declared a changed type). Here the comparison runs in-process and the
// change taxonomy is derived by walking the diff tree, so every count is
// traceable to a specific IR node.
// =============================================================================

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "abi/model.hpp"
#include "core/error.hpp"
#include "core/types.hpp"

namespace abistudy::abi {

/// @brief One side of a comparison: where to find the ELF, its DWARF and the
///        headers that define its PUBLIC API.
struct Side {
  std::filesystem::path elf;             ///< The shared object.
  std::filesystem::path debug_info_root; ///< Directory containing `.build-id/` (from -dbgsym).
  std::filesystem::path public_headers;  ///< Include root from the -dev package (may be empty).
};

/// @brief Knobs for compare().
struct Options {
  /// Fraction of exported function symbols that must be Itanium-mangled for
  /// the object to count as C++. Mixed libraries below this are "c".
  double cxx_mangled_fraction = 0.2;
  /// Minimum digits-blind removed/added symbol matches to declare that the
  /// library renames symbols per release by policy (ICU: foo_72 -> foo_73).
  std::uint32_t mass_rename_min = 50;
  /// Print every visited diff node and data-member map sizes to stderr.
  bool trace = false;
};

/// @brief One symbol-level event, kept so a reviewer can trace a count.
struct SymbolEvent {
  ChangeKind kind;
  SymbolName symbol;
  std::string pretty; ///< Demangled signature as libabigail prints it.
  std::optional<VersionNode> version;
};

/// @brief One type-level event with the header that declared the type.
struct TypeEvent {
  ChangeKind kind;
  std::string type_name;   ///< e.g. "struct Point"
  std::string declared_in; ///< Path from DWARF; empty if unknown.
  std::uint32_t count;     ///< Events of this kind on this type.
};

/// @brief Coverage facts a reviewer needs to trust the counts.
struct Coverage {
  bool debug_info_found_1 = false;
  bool debug_info_found_2 = false;
  std::uint32_t exported_functions_1 = 0;
  std::uint32_t exported_functions_2 = 0;
  std::uint32_t mangled_functions_1 = 0;
  std::uint32_t mangled_functions_2 = 0;
};

/// @brief Result of comparing one pair of shared objects.
/// @invariant `public_counts`, `third_party_counts` and `private_node_counts`
///            partition every event: a type event goes to third_party if its
///            declaring header is outside Side::public_headers; a symbol event
///            goes to private_node if its ELF version node is marked
///            PRIVATE/INTERNAL; all else is public.
struct SharedObjectDiff {
  Soname soname_1;
  Soname soname_2;
  Language language;
  ChangeCounts public_counts;
  ChangeCounts third_party_counts;
  ChangeCounts private_node_counts;
  std::uint32_t symbols_version_renamed = 0; ///< Digits-blind removed/added matches.
  bool mass_rename = false; ///< renamed >= mass_rename_min and >= all other symbol events.
  Coverage coverage;
  std::vector<TypeEvent> type_events;     ///< Every counted type event (public + third party).
  std::vector<SymbolEvent> symbol_events; ///< Every counted symbol event, capped at 2000.
};

void to_json(nlohmann::json & /*j*/, const SharedObjectDiff & /*d*/);
void to_json(nlohmann::json & /*j*/, const Coverage & /*c*/);
void to_json(nlohmann::json & /*j*/, const TypeEvent & /*e*/);
void to_json(nlohmann::json & /*j*/, const SymbolEvent & /*e*/);

/// @brief Compares two shared objects and classifies every difference.
/// @pre   `a.elf` and `b.elf` name existing files.
/// @post  Every event appears in exactly one of the three ChangeCounts and in
///        the corresponding events vector.
/// @errors abi_reader if either ELF cannot be read into a corpus (missing or
///         unreadable file, no ELF, corrupt DWARF). Absent debug info is NOT an
///         error: it is reported in Coverage and the comparison proceeds on
///         symbols alone.
/// @thread Not thread-safe: libabigail keeps per-environment state. Run
///         concurrent comparisons in separate processes.
[[nodiscard]] Result<SharedObjectDiff> compare(
  const Side &a, const Side &b, const Options &opt = {}
);

/// @brief Strips the version suffix from a SONAME: "libssl.so.3" -> "libssl".
/// @post  Never empty for a non-empty input.
[[nodiscard]] SonameStem soname_stem(std::string_view soname_or_filename);

/// @brief True if an ELF version node name marks the symbol as private
///        ("LIBDBUS_PRIVATE_1.16.2", "GLIBC_PRIVATE", "..._INTERNAL").
[[nodiscard]] bool is_private_version_node(std::string_view node) noexcept;

} // namespace abistudy::abi
