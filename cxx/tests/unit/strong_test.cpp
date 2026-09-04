#include "core/types.hpp"

#include <concepts>
#include <cstdio>
#include <format>
#include <unordered_map>

#include "check.hpp"

using namespace abistudy;

// The whole point of strong types: these must not compile against each other.
static_assert(!std::equality_comparable_with<SourceName, BinaryName>);
static_assert(!std::convertible_to<std::string, SourceName>);
static_assert(!std::convertible_to<SourceName, std::string>);
static_assert(!std::default_initializable<SourceName>);
static_assert(std::totally_ordered<SourceName>);
static_assert(StrongType<PopconRank> && !StrongType<int>);

int main() try {
  const SourceName a{"openssl"};
  const SourceName b{"openssl"};
  const SourceName c{"zlib"};
  CHECK(a == b);
  CHECK(a != c);
  CHECK(c > a);
  CHECK_EQ(a.get(), "openssl");
  CHECK_EQ(a.view(), std::string_view{"openssl"});
  CHECK_EQ(std::format("{}", a), "openssl");
  CHECK_EQ(std::format("{:>8}", PopconRank{42}), "      42");

  // JSON round trip through the one-argument adl_serializer.
  const nlohmann::json j = a;
  CHECK(j.is_string());
  CHECK(j.get<SourceName>() == a);
  const nlohmann::json jr = PopconRank{7};
  CHECK(jr.get<PopconRank>() == PopconRank{7});
  const nlohmann::json arr = std::vector<BinaryName>{BinaryName{"x"}, BinaryName{"y"}};
  CHECK_EQ(arr.get<std::vector<BinaryName>>().size(), 2U);

  // Usable as a hash key.
  std::unordered_map<Soname, int> m;
  m[Soname{"libz.so.1"}] = 1;
  CHECK(m.contains(Soname{"libz.so.1"}));
  CHECK(!m.contains(Soname{"libz.so.2"}));

  return test::report("strong");
} catch (const std::exception &e) {
  static_cast<void>(std::fputs("strong: unexpected exception: ", stderr));
  static_cast<void>(std::fputs(e.what(), stderr));
  static_cast<void>(std::fputs("\n", stderr));
  return 1;
}
