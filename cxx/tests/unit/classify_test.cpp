#include "abi/compare.hpp"
#include "abi/model.hpp"

#include "check.hpp"

using namespace abistudy;

int main() {
  // Every kind has a stable name that round-trips.
  for (const auto k : all_change_kinds)
    CHECK(parse_change_kind(to_string(k)) == k);
  CHECK(!parse_change_kind("nonsense"));

  // The two framings nest, and the mechanism map is total and consistent.
  for (const auto k : all_change_kinds) {
    if (is_binary_breaking(k))
      CHECK(is_evolution_relevant(k));
    const auto m = mechanism_for(k);
    CHECK(is_absorbed(k) == (m != Mechanism::none && m != Mechanism::not_applicable));
  }
  CHECK(!is_binary_breaking(ChangeKind::enum_case_added));   // moves no byte
  CHECK(is_evolution_relevant(ChangeKind::enum_case_added)); // breaks exhaustive switches
  CHECK(!is_binary_breaking(ChangeKind::inline_body_changed));
  CHECK(!is_evolution_relevant(ChangeKind::inline_body_changed));
  CHECK(is_evolution_or_inline(ChangeKind::inline_body_changed));
  CHECK(!is_evolution_or_inline(ChangeKind::macro_value_changed));
  for (const auto k : all_change_kinds) {
    if (is_evolution_relevant(k)) {
      CHECK(is_evolution_or_inline(k));
    }
  }
  CHECK(mechanism_for(ChangeKind::symbol_removed) == Mechanism::none);
  CHECK(mechanism_for(ChangeKind::symbol_added) == Mechanism::not_applicable);
  CHECK(mechanism_for(ChangeKind::vtable_changed) == Mechanism::resilient_dispatch);

  // ChangeCounts: zero is absence; merge adds; JSON round-trips.
  ChangeCounts c;
  CHECK(c.empty());
  c.add(ChangeKind::type_size_changed, 0);
  CHECK(c.empty());
  c.add(ChangeKind::type_size_changed, 2);
  ChangeCounts d;
  d.add(ChangeKind::type_size_changed);
  d.add(ChangeKind::symbol_added, 5);
  c.merge(d);
  CHECK_EQ(c.get(ChangeKind::type_size_changed), 3U);
  CHECK_EQ(c.get(ChangeKind::symbol_added), 5U);
  CHECK_EQ(c.get(ChangeKind::vtable_changed), 0U);
  const nlohmann::json j = c;
  CHECK_EQ(j.get<ChangeCounts>().get(ChangeKind::type_size_changed), 3U);

  // SONAME stems pair across a bump; private nodes are recognised case-insensitively.
  CHECK_EQ(abi::soname_stem("libssl.so.3").get(), "libssl");
  CHECK_EQ(abi::soname_stem("/usr/lib/x86_64-linux-gnu/libicuuc.so.72.1").get(), "libicuuc");
  CHECK_EQ(abi::soname_stem("libfoo.so").get(), "libfoo");
  CHECK(abi::is_private_version_node("LIBDBUS_PRIVATE_1.16.2"));
  CHECK(abi::is_private_version_node("GLIBC_PRIVATE"));
  CHECK(abi::is_private_version_node("libfoo_internal"));
  CHECK(!abi::is_private_version_node("LIBSSL_3_0_0"));
  CHECK(!abi::is_private_version_node(""));

  return test::report("classify");
}
