#include "core/version.hpp"

#include <cctype>
#include <charconv>

#include "core/contracts.hpp"

namespace abistudy {
namespace {

/// @brief deb-version(7) character weight: '~' lowest, then end-of-string,
///        then letters, then everything else by ASCII.
/// @post  Consistent with dpkg's `order()` helper.
[[nodiscard]] int weight(char c) noexcept {
  if (c == '~')
    return -1;
  if (c == '\0')
    return 0;
  if (std::isdigit(static_cast<unsigned char>(c)))
    return 0; // digits never reach here
  if (std::isalpha(static_cast<unsigned char>(c)))
    return static_cast<unsigned char>(c);
  return static_cast<unsigned char>(c) + 256;
}

[[nodiscard]] bool is_digit(char c) noexcept {
  return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

} // namespace

std::strong_ordering compare_fragment(std::string_view a, std::string_view b) noexcept {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() || j < b.size()) {
    // Non-digit run.
    while ((i < a.size() && !is_digit(a[i])) || (j < b.size() && !is_digit(b[j]))) {
      const int wa = i < a.size() && !is_digit(a[i]) ? weight(a[i]) : 0;
      const int wb = j < b.size() && !is_digit(b[j]) ? weight(b[j]) : 0;
      if (wa != wb)
        return wa <=> wb;
      if (i < a.size() && !is_digit(a[i]))
        ++i;
      if (j < b.size() && !is_digit(b[j]))
        ++j;
    }
    // Digit run: skip leading zeros, compare by length then lexically.
    while (i < a.size() && a[i] == '0')
      ++i;
    while (j < b.size() && b[j] == '0')
      ++j;
    const std::size_t si = i;
    const std::size_t sj = j;
    while (i < a.size() && is_digit(a[i]))
      ++i;
    while (j < b.size() && is_digit(b[j]))
      ++j;
    const auto da = a.substr(si, i - si);
    const auto db = b.substr(sj, j - sj);
    if (da.size() != db.size())
      return da.size() <=> db.size();
    if (const auto c = da.compare(db); c != 0)
      return c <=> 0;
  }
  return std::strong_ordering::equal;
}

Result<DebianVersion> DebianVersion::parse(std::string_view s) {
  if (s.empty())
    return fail(ErrorCode::invalid_argument, "empty version string");
  DebianVersion v;
  v.original_ = std::string{s};

  std::string_view rest = s;
  if (const auto colon = rest.find(':'); colon != std::string_view::npos) {
    const auto ep = rest.substr(0, colon);
    std::uint32_t epoch = 0;
    const auto [ptr, ec] = std::from_chars(ep.data(), ep.data() + ep.size(), epoch);
    if (ec != std::errc{} || ptr != ep.data() + ep.size())
      return fail(ErrorCode::invalid_argument, "non-numeric epoch in '{}'", s);
    v.epoch_ = epoch;
    rest.remove_prefix(colon + 1);
  }
  if (const auto dash = rest.rfind('-'); dash != std::string_view::npos) {
    v.upstream_ = std::string{rest.substr(0, dash)};
    v.revision_ = std::string{rest.substr(dash + 1)};
  } else {
    v.upstream_ = std::string{rest};
  }
  if (v.upstream_.empty())
    return fail(ErrorCode::invalid_argument, "empty upstream version in '{}'", s);
  ABISTUDY_ENSURES(!v.upstream_.empty());
  return v;
}

bool DebianVersion::is_prerelease() const noexcept {
  return upstream_.find('~') != std::string::npos;
}

std::strong_ordering operator<=>(const DebianVersion &a, const DebianVersion &b) noexcept {
  if (const auto c = a.epoch_ <=> b.epoch_; c != nullptr)
    return c;
  if (const auto c = compare_fragment(a.upstream_, b.upstream_); c != nullptr)
    return c;
  return compare_fragment(a.revision_, b.revision_);
}

} // namespace abistudy
