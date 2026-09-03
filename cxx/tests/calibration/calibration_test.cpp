// =============================================================================
// Calibration: the correctness gate for the whole study.
//
// Synthetic libraries, each with exactly one known change between v1 and v2
// (plus negative controls with no ABI change). Each is compiled with the
// system compiler, then pushed through the SAME comparer and indexer
// adapters the corpus run uses. A case passes when the expected ChangeKind
// is reported -- or, for changes that are invisible to DWARF/ELF by
// construction, when the ABI stage stays silent and the header stage sees it
// -- and when the facts the lenient definition relies on (exposure,
// append-only, vague linkage, declared symbols) come out as the source says.
// =============================================================================

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <vector>

#include "adapters/abigail/comparer.hpp"
#include "adapters/fs/artifact_store.hpp"
#include "adapters/libclang/indexer.hpp"
#include "adapters/posix/process.hpp"
#include "core/fs.hpp"
#include "domain/header_model.hpp"

using namespace abistudy;
namespace stdfs = std::filesystem;

namespace {

/// @brief Everything one case produced.
struct Observed {
  SharedObjectDiff abi;
  HeaderIndex hdr1;
  HeaderIndex hdr2;
  HeaderDiff hdr;
};

/// @brief What each ground-truth label must produce.
struct Expect {
  std::optional<ChangeKind> abi; ///< Kind in public counts; nullopt = ABI must be silent.
  std::optional<ChangeKind> hdr; ///< Header kind that must be > 0; nullopt = no requirement.
  std::function<bool(const Observed &)> also; ///< Extra facts; nullptr = none.
};

bool first_layout_event(const Observed &o, TypeExposure exposure, bool append_only) {
  return std::ranges::any_of(o.abi.type_events, [&](const TypeEvent &e) {
    return !e.third_party && is_layout_kind(e.kind) && e.exposure == exposure &&
           e.append_only == append_only;
  });
}

const std::map<std::string, Expect> &expectations() {
  static const std::map<std::string, Expect> table = {
    // The pointer-only append: strict break, not a lenient one.
    {"struct_field_added",
     {.abi = ChangeKind::field_added_to_struct,
      .hdr = {},
      .also =
        [](const Observed &o) { return first_layout_event(o, TypeExposure::by_pointer, true); }}},
    {"struct_field_added_middle",
     {.abi = ChangeKind::field_added_to_struct,
      .hdr = {},
      .also =
        [](const Observed &o) { return !first_layout_event(o, TypeExposure::by_pointer, true); }}},
    {"struct_field_added_into_padding",
     {.abi = ChangeKind::field_added_to_struct, .hdr = {}, .also = nullptr}},
    {"struct_field_added_by_value",
     {.abi = ChangeKind::field_added_to_struct,
      .hdr = {},
      .also =
        [](const Observed &o) { return first_layout_event(o, TypeExposure::by_value, true); }}},
    {"struct_field_type_changed",
     {.abi = ChangeKind::field_type_changed, .hdr = {}, .also = nullptr}},
    {"struct_field_removed",
     {.abi = ChangeKind::field_removed_from_struct, .hdr = {}, .also = nullptr}},
    {"enum_case_added", {.abi = ChangeKind::enum_case_added, .hdr = {}, .also = nullptr}},
    {"enum_case_added_widening", {.abi = ChangeKind::enum_case_added, .hdr = {}, .also = nullptr}},
    {"enum_case_removed", {.abi = ChangeKind::enum_case_removed, .hdr = {}, .also = nullptr}},
    {"function_param_type_changed",
     {.abi = ChangeKind::function_signature_changed, .hdr = {}, .also = nullptr}},
    {"function_param_added",
     {.abi = ChangeKind::function_signature_changed, .hdr = {}, .also = nullptr}},
    {"function_return_type_changed",
     {.abi = ChangeKind::function_signature_changed, .hdr = {}, .also = nullptr}},
    {"function_added", {.abi = ChangeKind::symbol_added, .hdr = {}, .also = nullptr}},
    {"function_removed",
     {.abi = ChangeKind::symbol_removed,
      .hdr = {},
      .also =
        [](const Observed &o) {
          // The removed function was declared in v1's header.
          return std::ranges::any_of(o.abi.symbol_events, [&](const SymbolEvent &e) {
            return e.kind == ChangeKind::symbol_removed &&
                   symbol_declared(o.hdr1, e.symbol.get()) == Declared::yes;
          });
        }}},
    {"undeclared_function_removed",
     {.abi = ChangeKind::symbol_removed,
      .hdr = {},
      .also =
        [](const Observed &o) {
          return std::ranges::any_of(o.abi.symbol_events, [&](const SymbolEvent &e) {
            return e.kind == ChangeKind::symbol_removed &&
                   symbol_declared(o.hdr1, e.symbol.get()) == Declared::no;
          });
        }}},
    {"vtable_virtual_added_end", {.abi = ChangeKind::vtable_changed, .hdr = {}, .also = nullptr}},
    {"vtable_virtual_added_middle",
     {.abi = ChangeKind::vtable_changed, .hdr = {}, .also = nullptr}},
    // A removed virtual is an API removal (its symbol vanishes), not a slot event.
    {"vtable_virtual_removed",
     {.abi = ChangeKind::symbol_removed,
      .hdr = {},
      .also =
        [](const Observed &o) { return !o.abi.public_counts.has(ChangeKind::vtable_changed); }}},
    {"class_made_polymorphic", {.abi = ChangeKind::vtable_changed, .hdr = {}, .also = nullptr}},
    {"base_class_added", {.abi = ChangeKind::base_class_changed, .hdr = {}, .also = nullptr}},
    {"method_added_nonvirtual", {.abi = ChangeKind::symbol_added, .hdr = {}, .also = nullptr}},
    // A template instantiation is vague linkage: every client has its own copy.
    {"template_instantiation_removed",
     {.abi = std::nullopt,
      .hdr = {},
      .also = [](
                const Observed &o
              ) { return o.abi.vague_linkage_counts.has(ChangeKind::symbol_removed); }}},
    // Invisible to every ABI tool by construction; the header stage must see them.
    {"inline_body_changed",
     {.abi = std::nullopt, .hdr = ChangeKind::inline_body_changed, .also = nullptr}},
    {"macro_value_changed",
     {.abi = std::nullopt, .hdr = ChangeKind::macro_value_changed, .also = nullptr}},
    // Negative controls.
    {"function_param_qualifier_changed", {.abi = std::nullopt, .hdr = {}, .also = nullptr}},
    {"opaque_impl_changed", {.abi = std::nullopt, .hdr = {}, .also = nullptr}},
    {"none", {.abi = std::nullopt, .hdr = {}, .also = nullptr}},
  };
  return table;
}

Result<stdfs::path> build(const stdfs::path &root, Language lang) {
  std::vector<std::string> argv{
    lang == Language::c ? "gcc" : "g++", "-shared", "-fPIC", "-g", "-O2", "-I",
    (root / "include").string()
  };
  for (const auto &e : stdfs::directory_iterator(root / "src"))
    argv.push_back(e.path().string());
  auto so = root / "lib.so";
  argv.emplace_back("-o");
  argv.push_back(so.string());
  ABISTUDY_TRY(auto r, posix::PosixProcessRunner{}.run(argv, {}));
  if (r.exit_code != 0)
    return fail(ErrorCode::external_tool, "compile failed: {}", r.err.substr(0, 400));
  return so;
}

struct Outcome {
  bool pass;
  std::string got;
};

Outcome run_case(const stdfs::path &dir) {
  auto meta = fsstore::parse_json(fs::read_file(dir / "case.json").value_or("{}"), "case.json");
  if (!meta)
    return {.pass = false, .got = meta.error().message};
  const std::string truth = meta->at("truth");
  const Language lang = meta->at("lang") == "c" ? Language::c : Language::cxx;
  const auto &table = expectations();
  const auto exp = table.find(truth);
  if (exp == table.end())
    return {.pass = false, .got = "no expectation for truth '" + truth + "'"};

  const auto v1 = dir / "v1";
  const auto v2 = dir / "v2";
  auto so1 = build(v1, lang);
  auto so2 = build(v2, lang);
  if (!so1 || !so2)
    return {.pass = false, .got = (so1 ? so2 : so1).error().message};

  const ports::Side a{.elf = *so1, .debug_info_root = {}, .public_headers = v1 / "include"};
  const ports::Side b{.elf = *so2, .debug_info_root = {}, .public_headers = v2 / "include"};
  auto d = abigail::AbigailComparer{}.compare(a, b, {});
  if (!d)
    return {.pass = false, .got = "compare: " + d.error().message};

  IndexOptions ho;
  ho.language = lang;
  const libclang::LibclangIndexer indexer;
  auto i1 = indexer.index(v1 / "include", ho);
  auto i2 = indexer.index(v2 / "include", ho);
  if (!i1 || !i2)
    return {.pass = false, .got = "index: " + (i1 ? i2 : i1).error().message};
  const Observed obs{
    .abi = std::move(*d), .hdr1 = std::move(*i1), .hdr2 = std::move(*i2), .hdr = {}
  };
  const auto hd = compare_headers(obs.hdr1, obs.hdr2);

  std::string got;
  for (const auto &[k, n] : obs.abi.public_counts.items())
    got += std::format("{}={} ", to_string(k), n);
  for (const auto &[k, n] : obs.abi.vague_linkage_counts.items())
    got += std::format("vague:{}={} ", to_string(k), n);
  for (const auto &e : obs.abi.type_events) {
    if (!e.third_party && is_layout_kind(e.kind)) {
      got += std::format(
        "[{} {}{}] ", e.type_name, to_string(e.exposure), e.append_only ? " append" : ""
      );
    }
  }
  if (hd.inline_body_changed)
    got += std::format("[hdr bodies={}] ", hd.inline_body_changed);
  if (hd.macro_value_changed)
    got += std::format("[hdr macros={}] ", hd.macro_value_changed);
  if (got.empty())
    got = "<silent>";

  bool ok =
    exp->second.abi ? obs.abi.public_counts.has(*exp->second.abi) : obs.abi.public_counts.empty();
  if (exp->second.hdr == ChangeKind::inline_body_changed)
    ok = ok && hd.inline_body_changed > 0;
  if (exp->second.hdr == ChangeKind::macro_value_changed)
    ok = ok && hd.macro_value_changed > 0;
  if (truth == "none")
    ok = ok && hd.inline_body_changed == 0 && hd.macro_value_changed == 0;
  if (exp->second.also && !exp->second.also(obs)) {
    ok = false;
    got += "(extra fact failed) ";
  }
  return {.pass = ok, .got = got};
}

} // namespace

int main(int argc, char **argv) try {
  if (argc != 2) {
    std::println(stderr, "usage: abistudy_calibration <cases-dir>");
    return 2;
  }
  std::vector<stdfs::path> cases;
  for (const auto &e : stdfs::directory_iterator(argv[1])) {
    if (e.is_directory())
      cases.push_back(e.path());
  }
  std::ranges::sort(cases);

  int fails = 0;
  std::println("{:<4} {:<36} {:<34} {}", "", "case", "ground truth", "result");
  std::println("{:-<118}", "");
  for (const auto &c : cases) {
    const auto meta =
      fsstore::parse_json(fs::read_file(c / "case.json").value_or("{}"), "case.json");
    const std::string truth = meta ? meta->value("truth", "?") : "?";
    const auto o = run_case(c);
    fails += static_cast<int>(!o.pass);
    std::println(
      "{:<4} {:<36} {:<34} {}", o.pass ? "PASS" : "FAIL", c.filename().string(), truth, o.got
    );
  }
  std::println(
    "{:-<118}\n{}/{} cases pass", "", cases.size() - static_cast<std::size_t>(fails), cases.size()
  );
  return fails ? 1 : 0;
} catch (const std::exception &e) {
  static_cast<void>(std::fputs("calibration: unexpected exception: ", stderr));
  static_cast<void>(std::fputs(e.what(), stderr));
  static_cast<void>(std::fputs("\n", stderr));
  return 1;
} catch (...) {
  static_cast<void>(std::fputs("calibration: unexpected non-standard exception\n", stderr));
  return 1;
}
