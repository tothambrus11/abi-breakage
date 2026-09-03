#include "core/version.hpp"

#include "check.hpp"

using namespace abistudy;

namespace {

std::strong_ordering cmp(std::string_view a, std::string_view b) {
  return *DebianVersion::parse(a) <=> *DebianVersion::parse(b);
}

} // namespace

int main() {
  // Orderings dpkg --compare-versions agrees with.
  CHECK(cmp("1.0", "1.1") == std::strong_ordering::less);
  CHECK(cmp("1.9", "1.10") == std::strong_ordering::less);    // numeric, not lexical
  CHECK(cmp("2.0~rc1", "2.0") == std::strong_ordering::less); // tilde sorts first
  CHECK(cmp("2.0~rc1", "2.0~rc2") == std::strong_ordering::less);
  CHECK(cmp("1:1.0", "9.9") == std::strong_ordering::greater);    // epoch wins
  CHECK(cmp("1.0-1", "1.0-2") == std::strong_ordering::less);     // revision
  CHECK(cmp("1.0+dfsg", "1.0") == std::strong_ordering::greater); // '+' sorts after end
  CHECK(cmp("1.0a", "1.0") == std::strong_ordering::greater);
  CHECK(cmp("1.0", "1.0") == std::strong_ordering::equal);
  CHECK(cmp("3.4.1-1", "3.4.1-1+b1") == std::strong_ordering::less); // binNMU

  // Components.
  const auto v = *DebianVersion::parse("1:2.3-4-5");
  CHECK_EQ(v.epoch(), 1U);
  CHECK_EQ(v.upstream().get(), "2.3-4"); // last hyphen splits
  CHECK_EQ(v.revision(), "5");
  CHECK(!v.is_prerelease());
  CHECK(DebianVersion::parse("2.0~beta1-1")->is_prerelease());
  CHECK(DebianVersion::parse("2.0")->revision().empty()); // native package

  // Errors, not exceptions.
  CHECK(!DebianVersion::parse(""));
  CHECK(!DebianVersion::parse("x:1.0"));
  CHECK(!DebianVersion::parse("1:"));
  CHECK(
    !DebianVersion::parse("") &&
    DebianVersion::parse("").error().code == ErrorCode::invalid_argument
  );

  return test::report("version");
}
