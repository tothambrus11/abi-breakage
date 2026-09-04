// Domain tests: everything here runs without libabigail, libclang or the
// network. The taxonomy's invariants, the two break definitions, the strata,
// release levels, the statistics and the summary shape.

#include <cstdio>
#include <set>

#include "adapters/abigail/comparer.hpp"
#include "check.hpp"
#include "domain/events.hpp"
#include "domain/header_model.hpp"
#include "domain/records.hpp"
#include "domain/statistics.hpp"
#include "domain/summary.hpp"
#include "domain/symbols.hpp"
#include "domain/taxonomy.hpp"
#include "domain/transition.hpp"
#include "domain/version.hpp"

using namespace abistudy;

namespace {

void taxonomy() {
  for (const auto k : all_change_kinds)
    CHECK(parse_change_kind(to_string(k)) == k);
  CHECK(!parse_change_kind("nonsense"));

  // The framings nest, and the mechanism map is total and consistent.
  for (const auto k : all_change_kinds) {
    if (is_binary_breaking(k))
      CHECK(is_evolution_relevant(k));
    if (is_evolution_relevant(k))
      CHECK(is_evolution_or_inline(k));
    const auto m = mechanism_for(k);
    CHECK(is_absorbed(k) == (m != Mechanism::none && m != Mechanism::not_applicable));
  }
  CHECK(!is_binary_breaking(ChangeKind::enum_case_added));   // moves no byte
  CHECK(is_evolution_relevant(ChangeKind::enum_case_added)); // breaks exhaustive switches
  CHECK(!is_evolution_relevant(ChangeKind::inline_body_changed));
  CHECK(!is_evolution_or_inline(ChangeKind::macro_value_changed));

  // REVIEW.md §1.2: removals and retypings are not absorbable.
  CHECK(mechanism_for(ChangeKind::field_removed_from_struct) == Mechanism::none);
  CHECK(mechanism_for(ChangeKind::field_type_changed) == Mechanism::none);
  CHECK(mechanism_for(ChangeKind::enum_case_removed) == Mechanism::none);
  CHECK(mechanism_for(ChangeKind::field_added_to_struct) == Mechanism::opaque_layout);
  CHECK(mechanism_for(ChangeKind::enum_case_added) == Mechanism::non_frozen_enum);
  CHECK(mechanism_for(ChangeKind::vtable_changed) == Mechanism::resilient_dispatch);
  CHECK(mechanism_for(ChangeKind::symbol_removed) == Mechanism::none);
  CHECK(mechanism_for(ChangeKind::symbol_added) == Mechanism::not_applicable);

  // ChangeCounts: zero is absence; merge adds; subtract clamps; JSON round-trips.
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
  CHECK_EQ(c.total(), 8U);
  c.subtract(ChangeKind::type_size_changed, 10);
  CHECK(!c.has(ChangeKind::type_size_changed));
  c.subtract(ChangeKind::symbol_added, 2);
  CHECK_EQ(c.get(ChangeKind::symbol_added), 3U);
  const nlohmann::json j = c;
  CHECK(j.get<ChangeCounts>() == c);
}

void symbols() {
  CHECK_EQ(soname_stem("libssl.so.3").get(), "libssl");
  CHECK_EQ(soname_stem("/usr/lib/x86_64-linux-gnu/libicuuc.so.72.1").get(), "libicuuc");
  CHECK_EQ(soname_stem("libfoo.so").get(), "libfoo");
  CHECK(is_private_version_node("LIBDBUS_PRIVATE_1.16.2"));
  CHECK(is_private_version_node("GLIBC_PRIVATE"));
  CHECK(is_private_version_node("libfoo_internal"));
  CHECK(!is_private_version_node("LIBSSL_3_0_0"));
  CHECK(!is_private_version_node(""));
  CHECK_EQ(digits_blind("u_strlen_72"), "u_strlen_#");
  CHECK_EQ(digits_blind("abc"), "abc");

  // Pair outcomes are classified from the record, in one place.
  {
    PairResult r{};
    CHECK(pair_outcome(r) == PairOutcome::no_linkable_object);
    r.error = std::string{pair_error_skipped} + "900 MB of packages exceeds ...";
    CHECK(pair_outcome(r) == PairOutcome::skipped_budget);
    r.error = std::string{pair_error_not_attempted} + "--deadline-minutes 270 reached";
    CHECK(pair_outcome(r) == PairOutcome::not_attempted);
    r.error = std::string{pair_error_timeout} + "1200s";
    CHECK(pair_outcome(r) == PairOutcome::failed_timeout);
    r.error = std::string{pair_error_killed} + " twice under --child-memory-mb 6000";
    CHECK(pair_outcome(r) == PairOutcome::failed_memory);
    r.error = "no object could be compared: libz3: reading corpus: std::bad_alloc";
    CHECK(pair_outcome(r) == PairOutcome::failed_memory);
    r.error = std::string{pair_error_exit} + "1: reader failed";
    CHECK(pair_outcome(r) == PairOutcome::failed);
    CHECK(retryable_with_more_resources(PairOutcome::failed_memory));
    CHECK(retryable_with_more_resources(PairOutcome::failed_timeout));
    CHECK(!retryable_with_more_resources(PairOutcome::skipped_budget));
    CHECK(!retryable_with_more_resources(PairOutcome::failed));
    r.objects.push_back(SharedObjectDiff{});
    CHECK(pair_outcome(r) == PairOutcome::compared); // objects win over any error text
  }

  // One language rule for every stage.
  {
    std::vector<SharedObjectDiff> objs;
    CHECK(dominant_language(objs) == Language::cxx);
    CHECK(dominant_language(objs, Language::c) == Language::c);
    objs.push_back(SharedObjectDiff{});
    objs.back().language = Language::unknown;
    objs.push_back(SharedObjectDiff{});
    objs.back().language = Language::c;
    CHECK(dominant_language(objs) == Language::c);
    objs.push_back(SharedObjectDiff{});
    objs.back().language = Language::cxx;
    CHECK(dominant_language(objs) == Language::cxx);
  }

  // Linkable library directories vs plugin directories.
  CHECK(is_linkable_library_dir("usr/lib/x86_64-linux-gnu"));
  CHECK(is_linkable_library_dir("usr/lib/x86_64-linux-gnu/"));
  CHECK(is_linkable_library_dir("/lib/x86_64-linux-gnu"));
  CHECK(is_linkable_library_dir("usr/lib"));
  CHECK(is_linkable_library_dir("lib"));
  CHECK(is_linkable_library_dir("usr/lib64"));
  CHECK(!is_linkable_library_dir("usr/lib/x86_64-linux-gnu/sane"));
  CHECK(!is_linkable_library_dir("usr/lib/x86_64-linux-gnu/spa-0.2/aec"));
  CHECK(!is_linkable_library_dir("usr/lib/x86_64-linux-gnu/gstreamer-1.0"));
  CHECK(!is_linkable_library_dir("lib/security"));
  CHECK(!is_linkable_library_dir("usr/lib/klibc"));
  CHECK(!is_linkable_library_dir("usr/libexec"));
  CHECK(!is_linkable_library_dir("usr/share/lib"));
  CHECK(!is_linkable_library_dir(""));

  // Vague linkage: weak AND mangled.
  CHECK(is_vague_linkage("_Z5twiceIiET_S0_", true));
  CHECK(!is_vague_linkage("_Z5twiceIiET_S0_", false));
  CHECK(!is_vague_linkage("weak_c_function", true));
}

void attribution() {
  abigail::ShippedHeaders shipped;
  for (const auto *p : {"zlib.h", "foo/bar.h", "types.h", "freetype2/freetype/freetype.h"})
    shipped.add(p);
  CHECK(abigail::declared_in_own_headers("/build/zlib-1.3/zlib.h", shipped));
  CHECK(abigail::declared_in_own_headers("../include/foo/bar.h", shipped));
  CHECK(abigail::declared_in_own_headers("/usr/include/zlib.h", shipped));
  CHECK(abigail::declared_in_own_headers("/usr/include/foo/bar.h", shipped));
  // The installed tree adds a prefix directory the build tree does not have.
  CHECK(abigail::declared_in_own_headers("../include/freetype/freetype.h", shipped));
  CHECK(abigail::declared_in_own_headers("/usr/include/freetype2/freetype/freetype.h", shipped));
  CHECK(abigail::declared_in_own_headers("/usr/include/freetype/freetype.h", shipped));
  // glibc's bits/types.h is not claimed by a library shipping a top-level types.h.
  CHECK(!abigail::declared_in_own_headers("/usr/include/x86_64-linux-gnu/bits/types.h", shipped));
  CHECK(!abigail::declared_in_own_headers("/usr/include/x86_64-linux-gnu/bits/other.h", shipped));
  CHECK(!abigail::declared_in_own_headers("/build/x/include/baz.h", shipped));
  // Nothing to attribute against: keep the event.
  CHECK(abigail::declared_in_own_headers("/anything.h", {}));
  CHECK(abigail::declared_in_own_headers("", shipped));
}

void lenient_rule() {
  TypeEvent e{
    .kind = ChangeKind::field_added_to_struct,
    .type_name = "struct Point",
    .declared_in = "lib.h",
    .count = 1,
    .exposure = TypeExposure::by_pointer,
    .third_party = false,
    .append_only = true
  };
  CHECK(!layout_event_breaks_leniently(e)); // opaque-by-convention growth
  e.exposure = TypeExposure::by_value;
  CHECK(layout_event_breaks_leniently(e));
  e.exposure = TypeExposure::not_in_interface;
  CHECK(!layout_event_breaks_leniently(e));
  e.append_only = false;
  CHECK(layout_event_breaks_leniently(e));
  e.kind = ChangeKind::enum_case_added; // not a layout kind: always counts
  e.append_only = true;
  CHECK(layout_event_breaks_leniently(e));

  // JSON round trip keeps the new fields.
  const nlohmann::json j = e;
  const auto back = type_event_from_json(j);
  CHECK(back.exposure == TypeExposure::not_in_interface);
  CHECK(back.append_only);
  CHECK(back.kind == ChangeKind::enum_case_added);
}

void release_levels() {
  CHECK(release_level("1.2.3", "1.2.4") == ReleaseLevel::patch);
  CHECK(release_level("1.2.3", "1.3.0") == ReleaseLevel::minor);
  CHECK(release_level("1.2.3", "2.0") == ReleaseLevel::major);
  CHECK(release_level("1.2", "1.2.1") == ReleaseLevel::patch);
  CHECK(release_level("16-20260322", "16-20260423") == ReleaseLevel::snapshot);
  CHECK(release_level("6.5", "6.5+20250125") == ReleaseLevel::snapshot);
  CHECK(release_level("1.11.0", "1.11.0+git20250114") == ReleaseLevel::snapshot);
  CHECK(
    release_level("2.1.27+dfsg", "2.1.27+dfsg2") == ReleaseLevel::other
  );                                                           // repack, not a release
  CHECK(release_level("1.2", "1.2.0") == ReleaseLevel::other); // same numerics
  CHECK(release_level("20240101", "20240201") == ReleaseLevel::snapshot);
  CHECK(release_level("5.6.1", "5.6.1+really5.4.5") == ReleaseLevel::other);
  CHECK(release_level("v2.0", "v2.1") == ReleaseLevel::minor);
  CHECK(release_level("abc", "1.0") == ReleaseLevel::other);
  for (const auto l : all_release_levels)
    CHECK(parse_release_level(to_string(l)) == l);
}

void statistics() {
  CHECK_EQ(median({}), 0.0);
  CHECK_EQ(median({3, 1, 2}), 2.0);
  CHECK_EQ(median({4, 1, 2, 3}), 2.5);

  const std::vector<std::vector<bool>> all_true{{true, true}, {true}, {true, true, true}};
  const auto iv = cluster_bootstrap(all_true);
  CHECK(iv.lo == 1.0 && iv.hi == 1.0);
  const std::vector<std::vector<bool>> mixed{{true, false}, {false}, {true, true}, {false, false}};
  const auto m1 = cluster_bootstrap(mixed);
  const auto m2 = cluster_bootstrap(mixed);
  CHECK(m1.lo == m2.lo && m1.hi == m2.hi); // deterministic
  CHECK(m1.lo >= 0 && m1.lo <= m1.hi && m1.hi <= 1);
  CHECK(m1.lo < 0.5 && m1.hi > 0.5);
  const auto lb = library_bootstrap({true, false, true, true});
  CHECK(lb.lo >= 0 && lb.lo <= lb.hi && lb.hi <= 1);
}

TypeEvent te(
  ChangeKind kind, std::string type, std::string file, std::uint32_t count, TypeExposure exposure,
  bool third_party, bool append_only
) {
  return TypeEvent{
    .kind = kind,
    .type_name = std::move(type),
    .declared_in = std::move(file),
    .count = count,
    .exposure = exposure,
    .third_party = third_party,
    .append_only = append_only
  };
}

SymbolEvent se(ChangeKind kind, const std::string &symbol) {
  return SymbolEvent{
    .kind = kind,
    .symbol = SymbolName{symbol},
    .pretty = symbol + "()",
    .version = std::nullopt,
    .weak = false
  };
}

Definition def(std::string kind, std::string decl, std::string body) {
  return Definition{
    .relative_path = "x.h",
    .name = "f",
    .kind = std::move(kind),
    .decl_fingerprint = std::move(decl),
    .body_fingerprint = std::move(body),
    .body_tokens = 1
  };
}

SharedObjectDiff object_with(std::vector<TypeEvent> tes, std::vector<SymbolEvent> ses) {
  SharedObjectDiff o{
    .soname_1 = Soname{"libx.so.1"},
    .soname_2 = Soname{"libx.so.1"},
    .language = Language::c,
    .public_counts = {},
    .third_party_counts = {},
    .private_node_counts = {},
    .vague_linkage_counts = {},
    .symbols_version_renamed = 0,
    .mass_rename = false,
    .coverage = {},
    .type_events = std::move(tes),
    .symbol_events = std::move(ses)
  };
  o.coverage.debug_info_found_1 = o.coverage.debug_info_found_2 = true;
  o.coverage.exported_functions_1 = 10;
  for (const auto &e : o.type_events)
    (e.third_party ? o.third_party_counts : o.public_counts).add(e.kind, e.count);
  for (const auto &e : o.symbol_events)
    o.public_counts.add(e.kind);
  return o;
}

void rollup_definitions() {
  // One pointer-only append (not a lenient break), one by-value append (is),
  // one removed field (always), one third-party event, and three symbol
  // removals: declared, undeclared, unknown.
  auto o = object_with(
    {te(
       ChangeKind::field_added_to_struct, "struct A", "a.h", 1, TypeExposure::by_pointer, false,
       true
     ),
     te(ChangeKind::type_size_changed, "struct A", "a.h", 1, TypeExposure::by_pointer, false, true),
     te(
       ChangeKind::field_added_to_struct, "struct B", "b.h", 2, TypeExposure::by_value, false, true
     ),
     te(
       ChangeKind::field_removed_from_struct, "struct C", "c.h", 1, TypeExposure::by_pointer, false,
       false
     ),
     te(
       ChangeKind::field_added_to_struct, "struct _IO_FILE", "/usr/include/stdio.h", 1,
       TypeExposure::by_pointer, true, true
     )},
    {se(ChangeKind::symbol_removed, "declared_fn"), se(ChangeKind::symbol_removed, "internal_fn"),
     se(ChangeKind::symbol_removed, "mystery_fn"), se(ChangeKind::symbol_added, "new_fn")}
  );
  PairResult pr{
    .id = "x@1.0..1.1",
    .source = SourceName{"x"},
    .upstream_1 = UpstreamVersion{"1.0"},
    .upstream_2 = UpstreamVersion{"1.1"},
    .objects = {o},
    .unpaired_1 = {},
    .unpaired_2 = {},
    .object_errors = {},
    .error = std::nullopt,
    .seconds = 1,
    .bytes_extracted = 0
  };
  HeaderResult hr{
    .id = pr.id,
    .source = pr.source,
    .upstream_1 = pr.upstream_1,
    .upstream_2 = pr.upstream_2,
    .language = Language::c,
    .diff = HeaderDiff{},
    .coverage_1 = {},
    .coverage_2 = {},
    .error = std::nullopt,
    .symbol_declared = {{"declared_fn", Declared::yes}, {"internal_fn", Declared::no}}
  };
  hr.diff->inline_body_changed = 3;
  hr.diff->macro_value_changed = 2;
  hr.diff->macro_value_changed_nonversion = 1;

  const auto t = rollup(pr, &hr);
  CHECK(t.level == ReleaseLevel::minor);
  CHECK_EQ(t.strict.get(ChangeKind::field_added_to_struct), 3U);
  CHECK_EQ(t.lenient.get(ChangeKind::field_added_to_struct), 2U);
  CHECK(!t.lenient.has(ChangeKind::type_size_changed));
  CHECK_EQ(t.strict.get(ChangeKind::field_removed_from_struct), 1U);
  CHECK_EQ(t.lenient.get(ChangeKind::field_removed_from_struct), 1U);
  CHECK_EQ(t.third_party.get(ChangeKind::field_added_to_struct), 1U);
  CHECK_EQ(t.strict.get(ChangeKind::symbol_removed), 3U);
  CHECK_EQ(t.lenient.get(ChangeKind::symbol_removed), 2U); // undeclared one dropped
  CHECK_EQ(t.symbols.removed_declared, 1U);
  CHECK_EQ(t.symbols.removed_undeclared, 1U);
  CHECK_EQ(t.symbols.removed_unknown, 1U);
  CHECK_EQ(t.symbols.layout_events_excluded, 2U);
  CHECK_EQ(t.layout_types_strict, 3U);
  CHECK_EQ(t.layout_types_lenient, 2U);
  CHECK_EQ(t.strict.get(ChangeKind::inline_body_changed), 3U);
  CHECK_EQ(t.strict.get(ChangeKind::macro_value_changed), 1U); // non-version only
  CHECK(t.debug_info_complete);
  CHECK(is_affected(t, Framing::binary, BreakDefinition::strict));
  CHECK(is_affected(t, Framing::binary, BreakDefinition::lenient));
  const auto ks = relevant_kinds(t, Framing::binary, BreakDefinition::lenient);
  CHECK(ks.contains(ChangeKind::symbol_removed) && ks.contains(ChangeKind::field_added_to_struct));
  CHECK(!fully_absorbable(ks)); // a removed field / symbol cannot be absorbed
  CHECK(fully_absorbable({ChangeKind::field_added_to_struct, ChangeKind::enum_case_added}));
  CHECK(!fully_absorbable({}));

  // Without a header result every symbol is undecidable and nothing is dropped.
  const auto t2 = rollup(pr, nullptr);
  CHECK_EQ(t2.lenient.get(ChangeKind::symbol_removed), 3U);
  CHECK_EQ(t2.symbols.removed_unknown, 3U);
  CHECK(!t2.headers);

  // Summary shape and a couple of invariants.
  SummaryInputs in;
  in.transitions = {t, t2};
  in.objects = 2;
  const auto s = summarize(in, SummaryOptions{.bootstrap = {.resamples = 50, .seed = 1}});
  CHECK_EQ(s.at("corpus").at("transitions").get<int>(), 2);
  CHECK(s.at("breaks").at("binary").at("strict").at("count").get<int>() == 2);
  CHECK(!s.at("frequency").at("all").at("rows").empty());
  CHECK(s.at("symbols").at("removed_undeclared").get<int>() == 1);
  const auto text = render_text(s);
  CHECK(text.contains("BREAK RATES"));
  CHECK(text.contains("field_added_to_struct"));
}

void header_model() {
  HeaderIndex a;
  HeaderIndex b;
  a.definitions["usr1"] = def("FunctionDecl", "d1", "b1");
  b.definitions["usr1"] = def("FunctionDecl", "d1", "b2");
  a.definitions["usr2"] = def("FunctionTemplate", "d", "b");
  b.definitions["usr2"] = def("FunctionTemplate", "d", "c");
  a.macros["x.h::LIMIT"] = "1";
  b.macros["x.h::LIMIT"] = "2";
  a.macros["x.h::X_VERSION"] = "1";
  b.macros["x.h::X_VERSION"] = "2";
  const auto d = compare_headers(a, b);
  CHECK_EQ(d.inline_body_changed, 2U);
  CHECK_EQ(d.inline_body_changed_template, 1U);
  CHECK_EQ(d.macro_value_changed, 2U);
  CHECK_EQ(d.macro_value_changed_nonversion, 1U);
  CHECK(looks_like_version_macro("x.h::X_VERSION"));
  CHECK(looks_like_version_macro("x.h::LIB_VERSION_STR"));
  CHECK(looks_like_version_macro("x.h::FOO_GIT_HASH"));
  CHECK(looks_like_version_macro("x.h::BUILD_DATE"));
  CHECK(looks_like_version_macro("x.h::foo_minor"));
  CHECK(!looks_like_version_macro("x.h::LIMIT"));
  CHECK(!looks_like_version_macro("x.h::MAX_DIGITS")); // contains GIT
  CHECK(!looks_like_version_macro("x.h::MINORBITS"));  // contains MINOR
  CHECK(!looks_like_version_macro("x.h::HASH_TABLE_SIZE"));
  CHECK(!looks_like_version_macro("x.h::BUILD_BUG_ON_ZERO"));

  // Declared-symbol join: undecidable when the index knows nothing or parsed poorly.
  CHECK(symbol_declared(a, "f") == Declared::unknown);
  a.declared_symbols = {"f", "_Z1gv"};
  CHECK(symbol_declared(a, "f") == Declared::yes);
  CHECK(symbol_declared(a, "h") == Declared::no);
  a.coverage.parsed = 10;
  a.coverage.with_errors = 8;
  CHECK(a.coverage.poor());
  CHECK(symbol_declared(a, "h") == Declared::unknown);
  const nlohmann::json j = a;
  const auto back = j.get<HeaderIndex>();
  CHECK(back.declared_symbols.contains("_Z1gv"));
}

} // namespace

int main() try {
  taxonomy();
  symbols();
  attribution();
  lenient_rule();
  release_levels();
  statistics();
  rollup_definitions();
  header_model();
  return test::report("domain");
} catch (const std::exception &e) {
  static_cast<void>(std::fputs("domain: unexpected exception: ", stderr));
  static_cast<void>(std::fputs(e.what(), stderr));
  static_cast<void>(std::fputs("\n", stderr));
  return 1;
}
