#pragma once
// Minimal test support: CHECK records a failure and continues; main() in each
// test file reports the tally. No framework dependency on purpose.
#include <cstdio>
#include <format>
#include <string_view>

namespace test {
inline int failures = 0;
inline int checks = 0;

inline void record(bool ok, std::string_view expr, const char *file, int line) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL %s:%d: %.*s\n", file, line, int(expr.size()), expr.data());
  }
}

inline int report(const char *suite) {
  std::fprintf(stderr, "%s: %d/%d checks passed\n", suite, checks - failures, checks);
  return failures ? 1 : 0;
}
} // namespace test

#define CHECK(expr) ::test::record(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::test::record((a) == (b), #a " == " #b, __FILE__, __LINE__)
