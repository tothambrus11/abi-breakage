#pragma once
// =============================================================================
// Error model. Expected runtime failures travel as values in std::expected;
// nothing in this code base throws across a function boundary. Contract
// violations are not errors (see contracts.hpp): they abort.
// =============================================================================

#include <cstdint>
#include <expected>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace abistudy {

/// @brief Coarse failure category. Fine detail lives in Error::message.
enum class ErrorCode : std::uint8_t {
  io,               ///< Local filesystem: cannot read/write/create.
  network,          ///< HTTP transport failure after retries were exhausted.
  http_status,      ///< Server answered with a non-2xx status after retries.
  integrity,        ///< Downloaded bytes do not match the expected hash.
  parse,            ///< Input did not have the expected shape (JSON, Packages, deb).
  not_found,        ///< A looked-up entity does not exist (package, version, file).
  invalid_argument, ///< A user-supplied argument is malformed.
  external_tool,    ///< A subprocess (g++ in tests) failed.
  abi_reader,       ///< libabigail could not build a corpus from an ELF file.
  clang,            ///< libclang failed to create a translation unit.
  schema,           ///< An on-disk pipeline artefact has an incompatible version.
};

/// @brief Human-readable name of an ErrorCode, for logs.
/// @post  Never null; returns a static string.
[[nodiscard]] constexpr const char *to_string(ErrorCode c) noexcept {
  switch (c) {
  case ErrorCode::io:
    return "io";
  case ErrorCode::network:
    return "network";
  case ErrorCode::http_status:
    return "http_status";
  case ErrorCode::integrity:
    return "integrity";
  case ErrorCode::parse:
    return "parse";
  case ErrorCode::not_found:
    return "not_found";
  case ErrorCode::invalid_argument:
    return "invalid_argument";
  case ErrorCode::external_tool:
    return "external_tool";
  case ErrorCode::abi_reader:
    return "abi_reader";
  case ErrorCode::clang:
    return "clang";
  case ErrorCode::schema:
    return "schema";
  }
  return "unknown";
}

/// @brief A runtime failure: category, message, and where it was raised.
/// @invariant `message` is non-empty.
struct Error {
  ErrorCode code;
  std::string message;
  std::source_location where;

  /// @brief One-line rendering suitable for a log or stderr.
  [[nodiscard]] std::string str() const {
    return std::format("{}: {} [{}:{}]", to_string(code), message, where.file_name(), where.line());
  }
};

/// @brief Result alias used by every fallible function in the code base.
template <class T>
using Result = std::expected<T, Error>;

/// @brief A format string that also captures the caller's source location.
///        Needed because a defaulted std::source_location parameter cannot
///        follow a parameter pack.
template <class... Args>
struct FormatAt {
  std::format_string<Args...> fmt;
  std::source_location where;

  /// @brief Implicit from a literal, so call sites read `fail(code, "...", x)`.
  template <class S>
    requires std::convertible_to<const S &, std::string_view>
  consteval FormatAt(const S &s, std::source_location loc = std::source_location::current())
      : fmt(s), where(loc) {}
};

/// @brief Builds an unexpected Error with a formatted message, located at the
///        call site.
/// @pre   `f.fmt` is a valid std::format string for `args` (checked at compile time).
/// @post  The returned unexpected carries `code`, the rendered message and the
///        caller's location.
template <class... Args>
[[nodiscard]] std::unexpected<Error> fail(
  ErrorCode code, FormatAt<std::type_identity_t<Args>...> f, Args &&...args
) {
  return std::unexpected{Error{code, std::format(f.fmt, std::forward<Args>(args)...), f.where}};
}

/// @brief Propagates an Error from a Result of another type.
/// @pre   `!r.has_value()`
template <class U>
[[nodiscard]] std::unexpected<Error> forward_error(const Result<U> &r) {
  return std::unexpected{r.error()};
}

// NOLINTBEGIN(bugprone-macro-parentheses): `decl` is a declaration and cannot be parenthesised
#define ABISTUDY_CAT_IMPL(a, b) a##b
#define ABISTUDY_CAT(a, b) ABISTUDY_CAT_IMPL(a, b)

/// @brief Early return for Result-returning functions.
///        `ABISTUDY_TRY(auto x, expr);` binds x to the value or returns the error.
#define ABISTUDY_TRY(decl, expr)                                                                   \
  auto &&ABISTUDY_CAT(abistudy_try_, __LINE__) = (expr);                                           \
  if (!ABISTUDY_CAT(abistudy_try_, __LINE__))                                                      \
    return ::abistudy::forward_error(ABISTUDY_CAT(abistudy_try_, __LINE__));                       \
  decl = *std::move(                                                                               \
    ABISTUDY_CAT(abistudy_try_, __LINE__)                                                          \
  ) /* NOLINT(bugprone-macro-parentheses): decl is a declaration */

/// @brief Early return when only success matters (Result<void> or value unused).
#define ABISTUDY_TRY_VOID(expr)                                                                    \
  if (auto &&ABISTUDY_CAT(abistudy_tryv_, __LINE__) = (expr);                                      \
      !ABISTUDY_CAT(abistudy_tryv_, __LINE__))                                                     \
  return ::abistudy::forward_error(ABISTUDY_CAT(abistudy_tryv_, __LINE__))

// NOLINTEND(bugprone-macro-parentheses)

} // namespace abistudy
