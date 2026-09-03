// =============================================================================
// Calibration: the correctness gate for the whole study.
//
// Thirty synthetic libraries, each with exactly one known change between v1
// and v2 (plus negative controls with no ABI change). Each is compiled with
// the system compiler, then pushed through the SAME abi::compare and
// hdr::index code the corpus run uses. A case passes when the expected
// ChangeKind is reported -- or, for changes that are invisible to DWARF/ELF
// by construction, when the ABI stage stays silent and the header stage sees it.
// =============================================================================

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <vector>

#include "abi/compare.hpp"
#include "core/artifact.hpp"
#include "core/fs.hpp"
#include "core/process.hpp"
#include "hdr/index.hpp"

using namespace abistudy;
namespace stdfs = std::filesystem;

namespace {

/// @brief What each ground-truth label must produce.
struct Expect {
  std::optional<ChangeKind>
    abi; ///< Kind that must appear in public counts; nullopt = ABI must be silent.
  std::optional<ChangeKind> hdr; ///< Header kind that must be > 0; nullopt = no requirement.
};

const std::map<std::string, Expect> expectations = {
  {"struct_field_added", {.abi = ChangeKind::field_added_to_struct, .hdr = {}}},
  {"struct_field_added_middle", {.abi = ChangeKind::field_added_to_struct, .hdr = {}}},
  {"struct_field_added_into_padding", {.abi = ChangeKind::field_added_to_struct, .hdr = {}}},
  {"struct_field_type_changed", {.abi = ChangeKind::field_type_changed, .hdr = {}}},
  {"struct_field_removed", {.abi = ChangeKind::field_removed_from_struct, .hdr = {}}},
  {"enum_case_added", {.abi = ChangeKind::enum_case_added, .hdr = {}}},
  {"enum_case_added_widening", {.abi = ChangeKind::enum_case_added, .hdr = {}}},
  {"enum_case_removed", {.abi = ChangeKind::enum_case_removed, .hdr = {}}},
  {"function_param_type_changed", {.abi = ChangeKind::function_signature_changed, .hdr = {}}},
  {"function_param_added", {.abi = ChangeKind::function_signature_changed, .hdr = {}}},
  {"function_return_type_changed", {.abi = ChangeKind::function_signature_changed, .hdr = {}}},
  {"function_added", {.abi = ChangeKind::symbol_added, .hdr = {}}},
  {"function_removed", {.abi = ChangeKind::symbol_removed, .hdr = {}}},
  {"vtable_virtual_added_end", {.abi = ChangeKind::vtable_changed, .hdr = {}}},
  {"vtable_virtual_added_middle", {.abi = ChangeKind::vtable_changed, .hdr = {}}},
  {"vtable_virtual_removed", {.abi = ChangeKind::vtable_changed, .hdr = {}}},
  {"class_made_polymorphic", {.abi = ChangeKind::vtable_changed, .hdr = {}}},
  {"base_class_added", {.abi = ChangeKind::base_class_changed, .hdr = {}}},
  {"method_added_nonvirtual", {.abi = ChangeKind::symbol_added, .hdr = {}}},
  // Invisible to every ABI tool by construction; the header stage must see them.
  {"inline_body_changed", {.abi = std::nullopt, .hdr = ChangeKind::inline_body_changed}},
  {"macro_value_changed", {.abi = std::nullopt, .hdr = ChangeKind::macro_value_changed}},
  // Negative controls.
  {"function_param_qualifier_changed", {.abi = std::nullopt, .hdr = {}}},
  {"opaque_impl_changed", {.abi = std::nullopt, .hdr = {}}},
  {"none", {.abi = std::nullopt, .hdr = {}}},
};

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
  ABISTUDY_TRY(auto r, proc::run(argv));
  if (r.exit_code != 0)
    return fail(ErrorCode::external_tool, "compile failed: {}", r.err.substr(0, 400));
  return so;
}

struct Outcome {
  bool pass;
  std::string got;
};

Outcome run_case(const stdfs::path &dir) {
  auto meta = parse_json(fs::read_file(dir / "case.json").value_or("{}"), "case.json");
  if (!meta)
    return {.pass = false, .got = meta.error().message};
  const std::string truth = meta->at("truth");
  const Language lang = meta->at("lang") == "c" ? Language::c : Language::cxx;
  const auto exp = expectations.find(truth);
  if (exp == expectations.end())
    return {.pass = false, .got = "no expectation for truth '" + truth + "'"};

  const auto v1 = dir / "v1";
  const auto v2 = dir / "v2";
  auto so1 = build(v1, lang);
  auto so2 = build(v2, lang);
  if (!so1 || !so2)
    return {.pass = false, .got = (so1 ? so2 : so1).error().message};

  const abi::Side a{.elf = *so1, .debug_info_root = {}, .public_headers = v1 / "include"};
  const abi::Side b{.elf = *so2, .debug_info_root = {}, .public_headers = v2 / "include"};
  auto d = abi::compare(a, b);
  if (!d)
    return {.pass = false, .got = "compare: " + d.error().message};

  hdr::Options ho;
  ho.language = lang;
  auto i1 = hdr::index(v1 / "include", ho);
  auto i2 = hdr::index(v2 / "include", ho);
  if (!i1 || !i2)
    return {.pass = false, .got = "hdr::index: " + (i1 ? i2 : i1).error().message};
  const auto hd = hdr::compare(*i1, *i2);

  std::string got;
  for (const auto &[k, n] : d->public_counts.items())
    got += std::format("{}={} ", to_string(k), n);
  if (hd.inline_body_changed)
    got += std::format("[hdr bodies={}] ", hd.inline_body_changed);
  if (hd.macro_value_changed)
    got += std::format("[hdr macros={}] ", hd.macro_value_changed);
  if (got.empty())
    got = "<silent>";

  bool ok = true;
  if (exp->second.abi) {
    ok = d->public_counts.has(*exp->second.abi);
  } else {
    ok = d->public_counts.empty();
  }
  if (exp->second.hdr == ChangeKind::inline_body_changed)
    ok = ok && hd.inline_body_changed > 0;
  if (exp->second.hdr == ChangeKind::macro_value_changed)
    ok = ok && hd.macro_value_changed > 0;
  if (truth == "none")
    ok = ok && hd.inline_body_changed == 0 && hd.macro_value_changed == 0;
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
    const auto meta = parse_json(fs::read_file(c / "case.json").value_or("{}"), "case.json");
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
