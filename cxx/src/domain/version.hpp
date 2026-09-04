#pragma once
// =============================================================================
// Debian version algebra. Implements the ordering defined in deb-version(7),
// the same algorithm dpkg uses, so that "consecutive release" means what the
// archive means and not what a lexical sort happens to produce
// ("1.10" > "1.9", "2.0~rc1" < "2.0", "1:1.0" > "9.9"). Also classifies a
// transition between two upstream versions by its level (REVIEW.md §3.3).
// =============================================================================

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/error.hpp"
#include "core/types.hpp"

namespace abistudy {

/// @brief A parsed Debian version: [epoch:]upstream[-revision].
/// @invariant `upstream` is non-empty. `revision` is empty iff the source
///            string had no hyphen (a native package). `epoch` defaults to 0.
class DebianVersion {
public:
  /// @brief Parses `s` per deb-version(7).
  /// @errors invalid_argument if `s` is empty, has an empty upstream part,
  ///         or a non-numeric epoch.
  [[nodiscard]] static Result<DebianVersion> parse(std::string_view s);

  /// @brief The epoch, 0 when absent.
  [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_; }

  /// @brief The upstream_version component, never empty.
  [[nodiscard]] UpstreamVersion upstream() const { return UpstreamVersion{upstream_}; }

  /// @brief The debian_revision component; empty for native packages.
  [[nodiscard]] std::string_view revision() const noexcept { return revision_; }

  /// @brief The original string, unchanged.
  [[nodiscard]] VersionString str() const { return VersionString{original_}; }

  /// @brief True if the upstream part contains '~', the archive convention
  ///        for pre-releases ("2.0~rc1"). Such versions are not "releases"
  ///        for the purpose of pairing.
  [[nodiscard]] bool is_prerelease() const noexcept;

  /// @brief dpkg ordering: epoch, then upstream, then revision, each with
  ///        the deb-version(7) segment comparison.
  /// @post  A strict weak order; equal iff all three components compare equal.
  friend std::strong_ordering operator<=>(
    const DebianVersion & /*a*/, const DebianVersion & /*b*/
  ) noexcept;
  friend bool operator==(const DebianVersion &a, const DebianVersion &b) noexcept {
    return (a <=> b) == std::strong_ordering::equal;
  }

private:
  DebianVersion() = default;
  std::uint32_t epoch_ = 0;
  std::string upstream_;
  std::string revision_;
  std::string original_;
};

/// @brief Compares two version FRAGMENTS (upstream or revision strings) with
///        the deb-version(7) algorithm: alternating non-digit and digit runs,
///        '~' sorting before everything including the empty string, letters
///        before non-letters.
/// @post  Strict weak order consistent with `dpkg --compare-versions`.
/// @complexity O(min(|a|, |b|)).
[[nodiscard]] std::strong_ordering compare_fragment(
  std::string_view a, std::string_view b
) noexcept;

/// @brief The level of a transition between two upstream versions, read from
///        the leading dotted numeric components of each string.
enum class ReleaseLevel : std::uint8_t {
  major,    ///< First numeric component differs.
  minor,    ///< Second differs.
  patch,    ///< Third or later differs.
  snapshot, ///< A component has eight or more digits (a date stamp).
  other,    ///< Non-numeric start, or equal numerics with a different suffix.
};

inline constexpr std::array all_release_levels = {
  ReleaseLevel::major, ReleaseLevel::minor, ReleaseLevel::patch, ReleaseLevel::snapshot,
  ReleaseLevel::other
};

[[nodiscard]] constexpr std::string_view to_string(ReleaseLevel l) noexcept {
  switch (l) {
  case ReleaseLevel::major:
    return "major";
  case ReleaseLevel::minor:
    return "minor";
  case ReleaseLevel::patch:
    return "patch";
  case ReleaseLevel::snapshot:
    return "snapshot";
  case ReleaseLevel::other:
    return "other";
  }
  return "other";
}
[[nodiscard]] ReleaseLevel parse_release_level(std::string_view s) noexcept;

/// @brief Classifies the transition `from` -> `to` (upstream version strings).
///        A leading 'v' is ignored; "+dfsg"/"+really" suffixes are not parsed.
[[nodiscard]] ReleaseLevel release_level(std::string_view from, std::string_view to) noexcept;

} // namespace abistudy
