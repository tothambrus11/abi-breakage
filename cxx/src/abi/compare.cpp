#include "abi/compare.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <print>
#include <regex>
#include <set>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>

#include <abg-comparison.h>
#include <abg-corpus.h>
#include <abg-dwarf-reader.h>
#include <abg-fe-iface.h>
#include <abg-ir.h>
#include <abg-suppression.h>
#include <abg-tools-utils.h>

#include "core/contracts.hpp"
#include "core/fs.hpp"

namespace abistudy::abi {

using namespace abigail;
using namespace abigail::comparison;
namespace ir = abigail::ir;

namespace {

// ----------------------------------------------------------------------------
// Corpus loading
// ----------------------------------------------------------------------------

/// @brief Debug-info root directories for libabigail (each must contain `.build-id/`).
using DebugRoots = std::vector<std::string>;

struct Loaded {
  ir::corpus_sptr corpus;
  bool debug_info_found = false;
};

/// @brief libabigail < 2.5 takes the debug-info roots as `vector<char**>`
///        (pointers to C strings), later versions as `vector<string>`. Both are
///        served from one call site.
template <class Env = ir::environment>
elf_based_reader_sptr create_reader_compat(const std::string &elf, const DebugRoots &roots, Env &env) {
  if constexpr (std::is_invocable_v<
                  decltype(&dwarf::create_reader), const std::string &,
                  const std::vector<std::string> &, Env &, bool, bool>) {
    return dwarf::create_reader(elf, roots, env, /*read_all_types=*/false, /*kernel=*/false);
  } else {
    std::vector<char *> cstrs;
    cstrs.reserve(roots.size());
    for (const auto &r : roots)
      cstrs.push_back(const_cast<char *>(r.c_str())); // NOLINT(*-const-cast): API takes char**
    std::vector<char **> ptrs;
    ptrs.reserve(cstrs.size());
    for (auto &c : cstrs)
      ptrs.push_back(&c);
    return dwarf::create_reader(elf, ptrs, env, /*read_all_types=*/false, /*kernel=*/false);
  }
}

/// @brief Reads one shared object into a corpus, restricted to types declared
///        under `public_headers` when that directory is given.
Result<Loaded> load(ir::environment &env, const Side &s, const DebugRoots &roots) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(s.elf, ec))
    return fail(ErrorCode::abi_reader, "'{}' is not a file", s.elf.string());

  elf_based_reader_sptr rdr;
  try {
    rdr = create_reader_compat(s.elf.string(), roots, env);
  } catch (const std::exception &e) {
    return fail(ErrorCode::abi_reader, "libabigail reader for '{}': {}", s.elf.string(), e.what());
  }
  if (!rdr) {
    return fail(
      ErrorCode::abi_reader, "libabigail could not create a reader for '{}'", s.elf.string()
    );
  }

  if (!s.public_headers.empty() && std::filesystem::is_directory(s.public_headers, ec)) {
    // Restrict the corpus to types defined in the package's own headers;
    // everything else (glibc, libstdc++) is not this library's ABI.
    if (auto sp = tools_utils::gen_suppr_spec_from_headers(s.public_headers.string()))
      rdr->add_suppressions(suppr::suppressions_type{sp});
  }

  fe_iface::status st = fe_iface::STATUS_UNKNOWN;
  ir::corpus_sptr c;
  try {
    c = rdr->read_corpus(st);
  } catch (const std::exception &e) {
    return fail(ErrorCode::abi_reader, "reading corpus of '{}': {}", s.elf.string(), e.what());
  }
  if (!c || (st & fe_iface::STATUS_NO_SYMBOLS_FOUND)) {
    return fail(
      ErrorCode::abi_reader, "'{}': no ELF symbols found (status {})", s.elf.string(),
      static_cast<int>(st)
    );
  }
  return Loaded{
    .corpus = c, .debug_info_found = (st & fe_iface::STATUS_DEBUG_INFO_NOT_FOUND) == 0U
  };
}

// ----------------------------------------------------------------------------
// Small IR helpers
// ----------------------------------------------------------------------------

/// @brief Removes `const` / `volatile` tokens from a libabigail type name.
///        The input is machine-produced (get_pretty_representation), so token
///        boundaries are exactly non-identifier characters.
std::string strip_cv(std::string_view name) {
  std::string out;
  std::string tok;
  auto flush = [&] {
    if (tok != "const" && tok != "volatile")
      out += tok;
    tok.clear();
  };
  for (const char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      tok.push_back(c);
    } else {
      flush();
      if (c != ' ') {
        out.push_back(c);
      } else if (!out.empty() && out.back() != ' ') {
        out.push_back(' ');
      }
    }
  }
  flush();
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

/// @brief The name a CALLER writes for a type: top-level typedef and
///        qualifiers peeled, then all cv tokens removed. Two types with equal
///        names are the same type for signature purposes: `char*` and
///        `const char*` agree (a caller's source still compiles), `int` and
///        `long` differ, and `struct Point*` is the same name whether or not
///        Point's layout changed -- that change is counted once, on the type.
std::string peeled_name(const ir::type_base_sptr &t) {
  if (!t)
    return "<none>";
  auto p = ir::peel_qualified_or_typedef_type(t);
  return strip_cv(ir::get_pretty_representation(p ? p : t, /*internal=*/false));
}

std::string decl_file(const ir::decl_base *d) {
  if (!d)
    return {};
  const ir::location &loc = d->get_location();
  if (!loc)
    return {};
  std::string path;
  unsigned line = 0;
  unsigned col = 0;
  loc.expand(path, line, col);
  return path;
}

std::string version_of(const ir::elf_symbol_sptr &sym) {
  if (!sym)
    return {};
  return sym->get_version().str();
}

/// @brief Base name for pairing a removed symbol with an added one: the
///        qualified function name without parameters. A C++ signature change
///        renames the mangled symbol, so it surfaces as delete+add of the same
///        qualified name.
std::string pairing_name(const ir::function_decl *f) { return f->get_qualified_name(); }

std::string digits_blind(std::string_view s) {
  std::string out;
  bool in_digits = false;
  for (const char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      if (!in_digits)
        out.push_back('#');
      in_digits = true;
    } else {
      out.push_back(c);
      in_digits = false;
    }
  }
  return out;
}

// ----------------------------------------------------------------------------
// Type-level classification: a visitor over the whole diff tree that counts
// each changed TYPE once (libabigail's "leaf" view), attributing it to the
// header that declares it.
// ----------------------------------------------------------------------------

class TypeVisitor final : public diff_node_visitor {
public:
  TypeVisitor(std::vector<TypeEvent> &out, bool trace) : out_(out), trace_(trace) {}

  using diff_node_visitor::visit;

  /// The traversal reaches every node through this overload (the per-kind
  /// overloads below are kept for callers that dispatch statically);
  /// dedupe by canonical type makes double arrival harmless.
  bool visit(diff *d, bool pre) override {
    if (pre && d && trace_) {
      std::println(
        stderr, "  visit {:22} changes={}  {}", typeid(*d).name(), int(d->has_changes()),
        d->get_pretty_representation()
      );
    }
    if (!pre || !d || !d->has_changes())
      return true;
    if (auto *cd = dynamic_cast<class_diff *>(d)) {
      on_class(*cd);
    } else if (auto *ud = dynamic_cast<union_diff *>(d)) {
      on_class_or_union(*ud);
    } else if (auto *ed = dynamic_cast<enum_diff *>(d)) {
      on_enum(*ed);
    }
    return true;
  }
  bool visit(class_diff *d, bool pre) override {
    if (pre && d && d->has_changes())
      on_class(*d);
    return true;
  }
  bool visit(enum_diff *d, bool pre) override {
    if (pre && d && d->has_changes())
      on_enum(*d);
    return true;
  }

private:
  bool first_time(const ir::type_base *key) { return (key != nullptr) && seen_.insert(key).second; }

  void emit(ChangeKind k, const ir::decl_base *decl, std::uint32_t n) {
    if (!n)
      return;
    out_.push_back(
      TypeEvent{
        .kind = k,
        .type_name = ir::get_pretty_representation(decl, false),
        .declared_in = decl_file(decl),
        .count = n
      }
    );
  }

  void on_class_or_union(class_or_union_diff &d) {
    const auto f = d.first_class_or_union();
    const auto s = d.second_class_or_union();
    if (!f || !s)
      return;
    const ir::type_base *key = f->get_canonical_type()
                                 ? f->get_canonical_type().get()
                                 : static_cast<const ir::type_base *>(f.get());
    if (!first_time(key))
      return;

    if (trace_) {
      std::println(
        stderr, "    members: inserted={} deleted={} changed={} subtype_changed={}",
        d.inserted_data_members().size(), d.deleted_data_members().size(),
        d.changed_data_members().size(), d.sorted_subtype_changed_data_members().size()
      );
      for (const auto &vd : d.sorted_subtype_changed_data_members()) {
        std::println(
          stderr, "      subtype: {} -> {}", vd->first_var()->get_pretty_representation(),
          vd->second_var()->get_pretty_representation()
        );
      }
    }
    emit(
      ChangeKind::field_added_to_struct, f.get(),
      static_cast<std::uint32_t>(d.inserted_data_members().size())
    );
    emit(
      ChangeKind::field_removed_from_struct, f.get(),
      static_cast<std::uint32_t>(d.deleted_data_members().size())
    );

    // libabigail splits changed members into two maps: changed_data_members
    // (name/offset/access changed) and subtype_changed_data_members (the
    // member's TYPE changed -- `int y` -> `long y` lands here because the
    // change is in the leaf type). Both are one member each.
    std::uint32_t type_changed = 0;
    std::uint32_t offset_changed = 0;
    std::unordered_set<std::string> changed_names;
    auto consider = [&](const var_diff_sptr &vd) {
      const auto v1 = vd->first_var();
      const auto v2 = vd->second_var();
      if (!v1 || !v2 || !changed_names.insert(v1->get_name()).second)
        return;
      if (peeled_name(v1->get_type()) != peeled_name(v2->get_type()))
        ++type_changed;
      if (ir::get_data_member_offset(v1) != ir::get_data_member_offset(v2))
        ++offset_changed;
    };
    for (const auto &[index, vd] : d.changed_data_members())
      consider(vd);
    for (const auto &vd : d.sorted_subtype_changed_data_members())
      consider(vd);
    // Members whose type is unchanged but which moved because an earlier
    // member was inserted are not in changed_data_members(); walk both.
    std::unordered_map<std::string, std::uint64_t> off1;
    for (const auto &m : f->get_data_members())
      off1[m->get_name()] = ir::get_data_member_offset(m);
    for (const auto &m : s->get_data_members()) {
      const auto it = off1.find(m->get_name());
      if (it != off1.end() && it->second != ir::get_data_member_offset(m) &&
          !changed_names.contains(m->get_name()))
        ++offset_changed;
    }
    emit(ChangeKind::field_type_changed, f.get(), type_changed);
    emit(ChangeKind::member_offset_changed, f.get(), offset_changed);
    if (f->get_size_in_bits() != s->get_size_in_bits())
      emit(ChangeKind::type_size_changed, f.get(), 1);
  }

  void on_class(class_diff &d) {
    on_class_or_union(d);
    const auto f = d.first_class_decl();
    const auto s = d.second_class_decl();
    if (!f || !s)
      return;

    auto bases = static_cast<std::uint32_t>(
      d.inserted_bases().size() + d.deleted_bases().size() + d.changed_bases().size()
    );
    emit(ChangeKind::base_class_changed, f.get(), bases);

    // Every virtual-slot event counts once: a vtable appearing or
    // vanishing, a virtual method inserted or deleted, a method changing
    // virtuality, or a virtual method whose slot index moved.
    std::uint32_t vt = 0;
    if (f->has_vtable() != s->has_vtable())
      ++vt;
    for (const auto &[name, m] : d.inserted_member_fns()) {
      if (m && ir::get_member_function_is_virtual(*m))
        ++vt;
    }
    for (const auto &[name, m] : d.deleted_member_fns()) {
      if (m && ir::get_member_function_is_virtual(*m))
        ++vt;
    }
    for (const auto &fd : d.changed_member_fns()) {
      if (!fd)
        continue;
      const auto ma = fd->first_function_decl();
      const auto mb = fd->second_function_decl();
      if (!ma || !mb)
        continue;
      const bool va = ir::get_member_function_is_virtual(*ma);
      const bool vb = ir::get_member_function_is_virtual(*mb);
      const bool moved = va && ir::get_member_function_vtable_offset(*ma) !=
                                 ir::get_member_function_vtable_offset(*mb);
      if (va != vb || moved) {
        ++vt;
      }
    }
    emit(ChangeKind::vtable_changed, f.get(), vt);
  }

  void on_enum(enum_diff &d) {
    const auto f = d.first_enum();
    const auto s = d.second_enum();
    if (!f || !s)
      return;
    const ir::type_base *key = f->get_canonical_type()
                                 ? f->get_canonical_type().get()
                                 : static_cast<const ir::type_base *>(f.get());
    if (!first_time(key))
      return;
    emit(
      ChangeKind::enum_case_added, f.get(),
      static_cast<std::uint32_t>(d.inserted_enumerators().size())
    );
    emit(
      ChangeKind::enum_case_removed, f.get(),
      static_cast<std::uint32_t>(d.deleted_enumerators().size())
    );
    // A changed underlying size is a layout change of the enum type.
    if (f->get_size_in_bits() != s->get_size_in_bits())
      emit(ChangeKind::type_size_changed, f.get(), 1);
  }

  std::vector<TypeEvent> &out_;
  bool trace_;
  std::unordered_set<const ir::type_base *> seen_;
};

// ----------------------------------------------------------------------------
// Symbol-level classification
// ----------------------------------------------------------------------------

struct SymbolSide {
  std::string linkage; ///< mangled / ELF name
  std::string pretty;
  std::string pairing; ///< qualified name without params (functions) or name (variables)
  std::string version;
  bool is_function;
};

template <class P>
auto *raw(const P &p) {
  if constexpr (std::is_pointer_v<P>) {
    return p;
  } else {
    return p.get();
  }
}

/// @brief vtables, typeinfo, typeinfo names and VTTs are emitted by the
///        compiler as a consequence of class changes; they are counted there,
///        not as symbol additions/removals.
bool compiler_generated(const std::string &linkage) {
  return linkage.starts_with("_ZTV") || linkage.starts_with("_ZTI") ||
         linkage.starts_with("_ZTS") || linkage.starts_with("_ZTT");
}

template <class Map>
void collect_functions(const Map &m, std::vector<SymbolSide> &out) {
  for (const auto &[key, fn] : m) {
    const ir::function_decl *f = raw(fn);
    if (!f)
      continue;
    const std::string linkage = f->get_linkage_name();
    if (compiler_generated(linkage))
      continue;
    out.push_back(
      SymbolSide{
        .linkage = f->get_linkage_name(),
        .pretty = f->get_pretty_representation(),
        .pairing = pairing_name(f),
        .version = version_of(f->get_symbol()),
        .is_function = true
      }
    );
  }
}

template <class Map>
void collect_variables(const Map &m, std::vector<SymbolSide> &out) {
  for (const auto &[key, v] : m) {
    const ir::var_decl *d = raw(v);
    if (!d)
      continue;
    const std::string linkage = d->get_linkage_name();
    if (compiler_generated(linkage))
      continue;
    out.push_back(
      SymbolSide{
        .linkage = d->get_linkage_name(),
        .pretty = d->get_pretty_representation(),
        .pairing = d->get_qualified_name(),
        .version = version_of(d->get_symbol()),
        .is_function = false
      }
    );
  }
}

/// @brief Splits symbol events into public/private tallies and detects the two
///        symbol-level patterns the raw lists hide: signature changes that the
///        mangler turned into remove+add, and policy-driven mass renames.
void classify_symbols(corpus_diff &d, const Options &opt, SharedObjectDiff &out) {
  std::vector<SymbolSide> removed;
  std::vector<SymbolSide> added;
  collect_functions(d.deleted_functions(), removed);
  collect_functions(d.added_functions(), added);
  collect_variables(d.deleted_variables(), removed);
  collect_variables(d.added_variables(), added);

  // 1. Signature changes hidden in remove+add (C++ mangling).
  std::multimap<std::string, std::size_t> added_by_name;
  for (std::size_t i = 0; i < added.size(); ++i)
    added_by_name.emplace(added[i].pairing, i);
  std::vector<bool> rem_used(removed.size());
  std::vector<bool> add_used(added.size());
  for (std::size_t i = 0; i < removed.size(); ++i) {
    if (!removed[i].is_function)
      continue;
    auto [lo, hi] = added_by_name.equal_range(removed[i].pairing);
    for (auto it = lo; it != hi; ++it) {
      if (add_used[it->second] || !added[it->second].is_function)
        continue;
      if (added[it->second].pretty == removed[i].pretty)
        continue; // identical text: a real remove+add of a duplicate? keep as is
      rem_used[i] = add_used[it->second] = true;
      auto &tally =
        is_private_version_node(removed[i].version) ? out.private_node_counts : out.public_counts;
      tally.add(ChangeKind::function_signature_changed);
      if (out.symbol_events.size() < 2000) {
        out.symbol_events.push_back(
          {ChangeKind::function_signature_changed, SymbolName{removed[i].linkage},
           removed[i].pretty + "  ->  " + added[it->second].pretty,
           removed[i].version.empty() ? std::nullopt
                                      : std::optional{VersionNode{removed[i].version}}}
        );
      }
      break;
    }
  }

  // 2. Mass rename by policy: digits-blind matches between what is left.
  std::multimap<std::string, std::size_t> add_blind;
  for (std::size_t i = 0; i < added.size(); ++i) {
    if (!add_used[i])
      add_blind.emplace(digits_blind(added[i].linkage), i);
  }
  std::uint32_t renamed = 0;
  for (std::size_t i = 0; i < removed.size(); ++i) {
    if (rem_used[i])
      continue;
    const auto it = add_blind.find(digits_blind(removed[i].linkage));
    if (it == add_blind.end())
      continue;
    rem_used[i] = add_used[it->second] = true;
    add_blind.erase(it);
    ++renamed;
  }
  out.symbols_version_renamed = renamed;

  // 3. Plain removals and additions.
  std::uint32_t plain = 0;
  auto record = [&](const SymbolSide &s, ChangeKind k, bool used) {
    if (used)
      return;
    ++plain;
    auto &tally = is_private_version_node(s.version) ? out.private_node_counts : out.public_counts;
    tally.add(k);
    if (out.symbol_events.size() < 2000) {
      out.symbol_events.push_back(
        {k, SymbolName{s.linkage}, s.pretty,
         s.version.empty() ? std::nullopt : std::optional{VersionNode{s.version}}}
      );
    }
  };
  for (std::size_t i = 0; i < removed.size(); ++i)
    record(removed[i], ChangeKind::symbol_removed, rem_used[i]);
  for (std::size_t i = 0; i < added.size(); ++i)
    record(added[i], ChangeKind::symbol_added, add_used[i]);
  if (renamed)
    out.public_counts.add(ChangeKind::symbol_version_renamed, renamed);
  out.mass_rename = renamed >= opt.mass_rename_min && renamed >= plain;

  // 4. Same symbol, changed declaration (C signature changes keep the name).
  for (const auto &[name, fd] : d.changed_functions()) {
    const auto a = fd->first_function_decl();
    const auto b = fd->second_function_decl();
    if (!a || !b)
      continue;
    bool sig = peeled_name(a->get_return_type()) != peeled_name(b->get_return_type()) ||
               a->get_parameters().size() != b->get_parameters().size();
    if (!sig) {
      const auto &pa = a->get_parameters();
      const auto &pb = b->get_parameters();
      for (std::size_t i = 0; i < pa.size(); ++i) {
        if (peeled_name(pa[i]->get_type()) != peeled_name(pb[i]->get_type())) {
          sig = true;
          break;
        }
      }
    }
    if (!sig)
      continue;
    const auto ver = version_of(a->get_symbol());
    auto &tally = is_private_version_node(ver) ? out.private_node_counts : out.public_counts;
    tally.add(ChangeKind::function_signature_changed);
    if (out.symbol_events.size() < 2000) {
      out.symbol_events.push_back(
        {ChangeKind::function_signature_changed, SymbolName{a->get_linkage_name()},
         a->get_pretty_representation() + "  ->  " + b->get_pretty_representation(),
         ver.empty() ? std::nullopt : std::optional{VersionNode{ver}}}
      );
    }
  }
}

/// @brief Language and symbol coverage from the exported function symbols.
void language_and_coverage(const ir::corpus &c, std::uint32_t &exported, std::uint32_t &mangled) {
  exported = mangled = 0;
  for (const auto &sym : c.get_sorted_fun_symbols()) {
    if (!sym)
      continue;
    ++exported;
    if (sym->get_name().starts_with("_Z"))
      ++mangled;
  }
}

} // namespace

namespace {
/// @brief Basenames of every header file below the given include roots.
std::unordered_set<std::string> header_basenames(
  std::initializer_list<std::filesystem::path> roots
) {
  std::unordered_set<std::string> names;
  for (const auto &root : roots) {
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) {
      continue;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec)) {
        names.insert(it->path().filename().string());
      }
    }
  }
  return names;
}
} // namespace

// ----------------------------------------------------------------------------

SonameStem soname_stem(std::string_view s) {
  if (const auto slash = s.rfind('/'); slash != std::string_view::npos)
    s.remove_prefix(slash + 1);
  if (const auto so = s.find(".so"); so != std::string_view::npos)
    s = s.substr(0, so);
  return SonameStem{std::string{s}};
}

bool is_private_version_node(std::string_view node) noexcept {
  std::string up;
  up.reserve(node.size());
  for (const char c : node)
    up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  return up.find("PRIVATE") != std::string::npos || up.find("INTERNAL") != std::string::npos;
}

Result<SharedObjectDiff> compare(const Side &a, const Side &b, const Options &opt) {
  ABISTUDY_EXPECTS(!a.elf.empty() && !b.elf.empty());

  ir::environment env;
  DebugRoots ra;
  DebugRoots rb;
  if (!a.debug_info_root.empty())
    ra.push_back(a.debug_info_root.string());
  if (!b.debug_info_root.empty())
    rb.push_back(b.debug_info_root.string());
  ABISTUDY_TRY(Loaded la, load(env, a, ra));
  ABISTUDY_TRY(Loaded lb, load(env, b, rb));

  SharedObjectDiff out{
    .soname_1 = Soname{la.corpus->get_soname()},
    .soname_2 = Soname{lb.corpus->get_soname()},
    .language = Language::unknown,
    .public_counts = {},
    .third_party_counts = {},
    .private_node_counts = {},
    .symbols_version_renamed = 0,
    .mass_rename = false,
    .coverage = {},
    .type_events = {},
    .symbol_events = {}
  };
  out.coverage.debug_info_found_1 = la.debug_info_found;
  out.coverage.debug_info_found_2 = lb.debug_info_found;
  language_and_coverage(
    *la.corpus, out.coverage.exported_functions_1, out.coverage.mangled_functions_1
  );
  language_and_coverage(
    *lb.corpus, out.coverage.exported_functions_2, out.coverage.mangled_functions_2
  );
  {
    const double ex = out.coverage.exported_functions_1 + out.coverage.exported_functions_2;
    const double mg = out.coverage.mangled_functions_1 + out.coverage.mangled_functions_2;
    if (ex == 0) {
      out.language = Language::unknown;
    } else if (mg / ex >= opt.cxx_mangled_fraction) {
      out.language = Language::cxx;
    } else {
      out.language = Language::c;
    }
  }

  diff_context_sptr ctxt(new diff_context);
  ctxt->show_leaf_changes_only(true);
  ctxt->show_impacted_interfaces(false);
  // Deliberately NOT switching off the "harmless" categories: enum-case
  // additions live there, and they are a change this study counts.

  corpus_diff_sptr d;
  try {
    d = compute_diff(la.corpus, lb.corpus, ctxt);
  } catch (const std::exception &e) {
    return fail(ErrorCode::abi_reader, "compute_diff: {}", e.what());
  }
  if (!d)
    return fail(ErrorCode::abi_reader, "compute_diff returned no result");

  std::vector<TypeEvent> type_events;
  TypeVisitor tv(type_events, opt.trace);
  d->traverse(tv);

  // Attribute each type to the package that declares it. DWARF records the
  // path the type was compiled from -- a build-tree path such as
  // /build/foo-1.2/include/foo.h -- so the test is by header BASENAME against
  // the files shipped in the -dev packages, which is also how libabigail's
  // own header suppression matches.
  const auto own_headers = header_basenames({a.public_headers, b.public_headers});
  for (const auto &ev : type_events) {
    const bool own =
      own_headers.empty() || ev.declared_in.empty() ||
      own_headers.contains(std::filesystem::path(ev.declared_in).filename().string());
    (own ? out.public_counts : out.third_party_counts).add(ev.kind, ev.count);
  }
  out.type_events = std::move(type_events);
  classify_symbols(*d, opt, out);
  return out;
}

// ----------------------------------------------------------------------------
// JSON
// ----------------------------------------------------------------------------

void to_json(nlohmann::json &j, const Coverage &c) {
  j = {
    {"debug_info_found", {c.debug_info_found_1, c.debug_info_found_2}},
    {"exported_functions", {c.exported_functions_1, c.exported_functions_2}},
    {"mangled_functions", {c.mangled_functions_1, c.mangled_functions_2}}
  };
}
void to_json(nlohmann::json &j, const TypeEvent &e) {
  j = {
    {"kind", to_string(e.kind)},
    {"type", e.type_name},
    {"declared_in", e.declared_in},
    {"count", e.count}
  };
}
void to_json(nlohmann::json &j, const SymbolEvent &e) {
  j = {{"kind", to_string(e.kind)}, {"symbol", e.symbol}, {"pretty", e.pretty}};
  if (e.version)
    j["version"] = *e.version;
}
void to_json(nlohmann::json &j, const SharedObjectDiff &d) {
  j = {
    {"soname", {d.soname_1, d.soname_2}},
    {"language", to_string(d.language)},
    {"public", d.public_counts},
    {"third_party", d.third_party_counts},
    {"private_node", d.private_node_counts},
    {"symbols_version_renamed", d.symbols_version_renamed},
    {"mass_rename", d.mass_rename},
    {"coverage", d.coverage},
    {"type_events", d.type_events},
    {"symbol_events", d.symbol_events}
  };
}

} // namespace abistudy::abi
