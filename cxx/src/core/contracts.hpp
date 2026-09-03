#pragma once
// =============================================================================
// Contract conventions for this code base
// =============================================================================
//
// Every declaration in `src/` carries a contract comment using these tags:
//
//   @brief      what the entity is for, in one sentence.
//   @pre        conditions the CALLER guarantees. Violating one is a bug in the
//               caller; it is checked with ABISTUDY_EXPECTS and aborts the
//               process. It is never reported as an Error.
//   @post       conditions the CALLEE guarantees on normal return.
//   @returns    meaning of the returned value.
//   @errors     which ErrorCode values a Result-returning function can yield
//               and when. Anything not listed cannot happen.
//   @throws     only ever "nothing": this code base does not throw across
//               function boundaries. Third-party exceptions are caught at the
//               call site and converted to Error.
//   @ownership  who owns a pointer / resource and for how long.
//   @thread     thread-safety of the entity; absent means "not thread-safe,
//               confine to one thread".
//   @complexity when it is not obviously linear in the input.
//   @invariant  on a class: what every public method preserves.
//
// The distinction that matters most: a violated @pre is a programming error
// and stops the process; an @errors condition is an expected runtime failure
// (network down, malformed archive) and flows back to the caller as a value.
//
// C++26 contract assertions (`pre(...)`, `post(...)`) are not yet available in
// the shipping GCC used to build this, so the checks are macros. The macro
// names deliberately mirror the standard vocabulary so the migration is
// mechanical once `__cpp_contracts` is defined.
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <print>
#include <source_location>

namespace abistudy::detail {

/// @brief Reports a contract violation and terminates the process.
/// @pre  `kind` and `expr` are null-terminated string literals.
/// @post Does not return.
[[noreturn]] inline void contract_violation(
  const char *kind, const char *expr, std::source_location where = std::source_location::current()
) noexcept {
  try {
    std::println(
      stderr, "{}:{}: contract violation ({}): {}\n  in {}", where.file_name(), where.line(), kind,
      expr, where.function_name()
    );
    static_cast<void>(std::fflush(stderr));
  } catch (...) { // NOLINT(bugprone-empty-catch): we are about to abort regardless
  }
  std::abort();
}

} // namespace abistudy::detail

/// @brief Precondition check. The caller promised `expr`; if false, abort.
#define ABISTUDY_EXPECTS(expr)                                                                     \
  ((expr) ? static_cast<void>(0) : ::abistudy::detail::contract_violation("precondition", #expr))

/// @brief Postcondition check. The callee promised `expr`; if false, abort.
#define ABISTUDY_ENSURES(expr)                                                                     \
  ((expr) ? static_cast<void>(0) : ::abistudy::detail::contract_violation("postcondition", #expr))

/// @brief Invariant / internal-consistency check.
#define ABISTUDY_ASSERT(expr)                                                                      \
  ((expr) ? static_cast<void>(0) : ::abistudy::detail::contract_violation("assertion", #expr))
