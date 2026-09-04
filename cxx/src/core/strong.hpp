#pragma once
// =============================================================================
// Strong<Rep, Tag>: a value of type Rep that is not interchangeable with any
// other Strong<Rep, OtherTag>. Used for every domain quantity that would
// otherwise be a bare std::string or integer (package names, versions,
// hashes, counts), so that passing a source-package name where a binary-
// package name is expected is a compile error rather than a wrong download.
// =============================================================================

#include <compare>
#include <concepts>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace abistudy {

/// @brief A distinct type wrapping a value of `Rep`, distinguished by `Tag`.
/// @invariant Holds exactly one `Rep` value; never in a "null" state. Every
///            operation is the corresponding operation on the wrapped value.
/// @thread    As thread-safe as `Rep` (immutable after construction).
template <class Rep, class Tag>
class Strong {
public:
  using rep_type = Rep;
  using tag_type = Tag;

  /// @brief There is no meaningful default value for a domain quantity.
  Strong() = delete ("a Strong value must be constructed from an explicit Rep");

  /// @brief Wraps `v`. Explicit so that raw values never convert silently.
  /// @post  get() == v
  constexpr explicit Strong(Rep v) noexcept(std::is_nothrow_move_constructible_v<Rep>)
      : value_(std::move(v)) {}

  /// @brief Read access to the wrapped value.
  /// @returns The wrapped value; the reference is valid for the lifetime of *this.
  [[nodiscard]] constexpr const Rep &get() const noexcept { return value_; }

  /// @brief Conversion to string_view, only when Rep is a string.
  [[nodiscard]] constexpr std::string_view view() const noexcept
    requires std::same_as<Rep, std::string>
  {
    return value_;
  }

  /// @brief Total order and equality delegate to Rep.
  friend constexpr auto operator<=>(const Strong &, const Strong &) = default;

private:
  Rep value_;
};

/// @brief Concept: T is some Strong<Rep, Tag>.
template <class T>
concept StrongType = requires {
  typename T::rep_type;
  typename T::tag_type;
} && std::same_as<T, Strong<typename T::rep_type, typename T::tag_type>>;

} // namespace abistudy

// NOLINTBEGIN(cert-dcl58-cpp,bugprone-std-namespace-modification): specialising std::formatter and
// std::hash for a program-defined type is explicitly allowed by the standard
/// @brief JSON: a Strong value serialises as its bare representation. The
///        one-argument from_json form is used because Strong is not
///        default-constructible.
template <abistudy::StrongType T>
struct nlohmann::adl_serializer<T> {
  static void to_json(json &j, const T &s) { j = s.get(); }
  static T from_json(const json &j) { return T{j.get<typename T::rep_type>()}; }
};

/// @brief std::format prints the wrapped value with Rep's own formatter.
template <abistudy::StrongType T, class CharT>
struct std::formatter<T, CharT> : std::formatter<typename T::rep_type, CharT> {
  template <class Ctx>
  auto format(const T &s, Ctx &ctx) const {
    return std::formatter<typename T::rep_type, CharT>::format(s.get(), ctx);
  }
};

/// @brief Hashing delegates to Rep so Strong types are usable as map keys.
template <abistudy::StrongType T>
struct std::hash<T> {
  [[nodiscard]] std::size_t operator()(const T &s) const noexcept {
    return std::hash<typename T::rep_type>{}(s.get());
  }
};
// NOLINTEND(cert-dcl58-cpp,bugprone-std-namespace-modification)
