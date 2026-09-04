#pragma once
// =============================================================================
// Port: compares two shared objects and classifies every difference into the
// study's taxonomy. libabigail is the only adapter today; the interface exists
// so the domain and the stages never see its IR.
// =============================================================================

#include <cstdint>
#include <filesystem>

#include "core/error.hpp"
#include "domain/events.hpp"

namespace abistudy::ports {

/// @brief One side of a comparison: where to find the ELF, its DWARF and the
///        headers that define its PUBLIC API.
struct Side {
  std::filesystem::path elf;             ///< The shared object.
  std::filesystem::path debug_info_root; ///< Directory containing `.build-id/` (from -dbgsym).
  std::filesystem::path public_headers;  ///< Include root from the -dev package (may be empty).
};

struct CompareOptions {
  /// Fraction of exported function symbols that must be Itanium-mangled for
  /// the object to count as C++. Mixed libraries below this are "c".
  double cxx_mangled_fraction = 0.2;
  /// Minimum digits-blind removed/added symbol matches to declare that the
  /// library renames symbols per release by policy (ICU: foo_72 -> foo_73).
  std::uint32_t mass_rename_min = 50;
  /// Symbol events kept per object (all are still counted).
  std::uint32_t max_symbol_events = 20000;
  /// Print every visited diff node to stderr.
  bool trace = false;
};

class AbiComparer {
public:
  virtual ~AbiComparer() = default;
  AbiComparer() = default;
  AbiComparer(const AbiComparer &) = delete;
  AbiComparer &operator=(const AbiComparer &) = delete;
  AbiComparer(AbiComparer &&) = delete;
  AbiComparer &operator=(AbiComparer &&) = delete;

  /// @brief Compares two shared objects and classifies every difference.
  /// @pre   `a.elf` and `b.elf` name existing files.
  /// @post  Every event appears in exactly one of the ChangeCounts and in the
  ///        corresponding events vector (symbol events up to the cap).
  /// @errors abi_reader if either ELF cannot be read (missing, not ELF,
  ///         corrupt DWARF). Absent debug info is NOT an error: it is reported
  ///         in Coverage and the comparison proceeds on symbols alone.
  /// @thread Implementation-defined; libabigail's is not thread-safe.
  [[nodiscard]] virtual Result<SharedObjectDiff> compare(
    const Side &a, const Side &b, const CompareOptions &opt
  ) const = 0;

  /// @brief Version of the underlying reader, for provenance.
  [[nodiscard]] virtual std::string version() const = 0;
};

} // namespace abistudy::ports
