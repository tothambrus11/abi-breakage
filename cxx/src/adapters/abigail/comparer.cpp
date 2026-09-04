#include "adapters/abigail/comparer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <map>
#include <print>
#include <set>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>

#include <abg-comparison.h>
#include <abg-config.h>
#include <abg-corpus.h>
#include <abg-dwarf-reader.h>
#include <abg-fe-iface.h>
#include <abg-ir.h>
#include <abg-suppression.h>
#include <abg-tools-utils.h>

#include "core/contracts.hpp"
#include "domain/symbols.hpp"

namespace abistudy::abigail {

using namespace ::abigail;
using namespace ::abigail::comparison;
namespace ir = ::abigail::ir;

namespace {

// ----------------------------------------------------------------------------
// Corpus loading
// ----------------------------------------------------------------------------

using DebugRoots = std::vector<std::string>;

struct Loaded {
  ir::corpus_sptr corpus;
  bool debug_info_found = false;
};

/// @brief libabigail < 2.5 takes the debug-info roots as `vector<char**>`
///        (pointers to C strings), later versions as `vector<string>`. Both are
///        served from one call site. The old API keeps the `char**` pointers
///        and dereferences them while the corpus is read, so `storage` must
///        outlive every use of the returned reader: the caller owns it.
template <class Env = ir::environment>
elf_based_reader_sptr create_reader_compat(
  const std::string &elf, const DebugRoots &roots, std::vector<char *> &storage, Env &env
) {
  if constexpr (
    std::is_invocable_v<
      decltype(&dwarf::create_reader), const std::string &, const std::vector<std::string> &, Env &,
      bool, bool>
  ) {
    return dwarf::create_reader(elf, roots, env, /*read_all_types=*/false, /*kernel=*/false);
  } else {
    storage.clear();
    storage.reserve(roots.size());
    for (const auto &r : roots)
      storage.push_back(const_cast<char *>(r.c_str())); // NOLINT(*-const-cast): API takes char**
    std::vector<char **> ptrs;
    ptrs.reserve(storage.size());
    for (auto &c : storage)
      ptrs.push_back(&c);
    return dwarf::create_reader(elf, ptrs, env, /*read_all_types=*/false, /*kernel=*/false);
  }
}

/// @brief Reads one shared object into a corpus, restricted to types declared
///        under `public_headers` when that directory is given.
Result<Loaded> load(ir::environment &env, const ports::Side &s, const DebugRoots &roots) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(s.elf, ec))
    return fail(ErrorCode::abi_reader, "'{}' is not a file", s.elf.string());

  // Outlives the reader: libabigail < 2.5 dereferences these while reading.
  std::vector<char *> root_storage;
  elf_based_reader_sptr rdr;
  try {
    rdr = create_reader_compat(s.elf.string(), roots, root_storage, env);
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

bool is_weak(const ir::elf_symbol_sptr &sym) {
  return sym && sym->get_binding() == ir::elf_symbol::WEAK_BINDING;
}

/// @brief The name TypeEvent::type_name carries: the pretty representation of
///        the type's declaration. Used as the key of the exposure map, so the
///        interface walk and the diff visitor must agree on it.
std::string type_key(const ir::decl_base *d) {
  return ir::get_pretty_representation(d, /*internal=*/false);
}

/// @brief libabigail's containers hold raw pointers in 2.4 and shared_ptrs
///        later; every loop over them goes through this.
template <class P>
auto *raw(const P &p) {
  if constexpr (std::is_pointer_v<P>) {
    return p;
  } else {
    return p.get();
  }
}

// ----------------------------------------------------------------------------
// Exposure of types through the exported interface (REVIEW.md §1.1)
// ----------------------------------------------------------------------------

using ExposureMap = std::unordered_map<std::string, TypeExposure>;

/// @brief Records how the class/union/enum at the bottom of `t` is reached:
///        by value (possibly through arrays and typedefs) or through at least
///        one pointer/reference.
void note_type(const ir::type_base_sptr &t, ExposureMap &map) {
  ir::type_base_sptr cur = t;
  bool by_value = true;
  for (int guard = 0; cur && guard < 32; ++guard) {
    if (auto peeled = ir::peel_qualified_or_typedef_type(cur); peeled && peeled != cur) {
      cur = peeled;
      continue;
    }
    if (const auto *ptr = ir::is_pointer_type(cur.get())) {
      cur = ptr->get_pointed_to_type();
      by_value = false;
      continue;
    }
    if (const auto *ref = ir::is_reference_type(cur.get())) {
      cur = ref->get_pointed_to_type();
      by_value = false;
      continue;
    }
    if (const auto *arr = ir::is_array_type(cur.get())) {
      cur = arr->get_element_type();
      continue;
    }
    break;
  }
  if (!cur)
    return;
  const ir::decl_base *d = nullptr;
  if (const auto *cou = ir::is_class_or_union_type(cur.get())) {
    d = cou;
  } else if (const auto *en = ir::is_enum_type(cur.get())) {
    d = en;
  }
  if (!d)
    return;
  const auto wanted = by_value ? TypeExposure::by_value : TypeExposure::by_pointer;
  auto [it, inserted] = map.emplace(type_key(d), wanted);
  if (!inserted && wanted == TypeExposure::by_value)
    it->second = TypeExposure::by_value; // by-value anywhere wins over pointer elsewhere
}

/// @brief Walks the exported functions and variables of a corpus.
void exposure_of(const ir::corpus &c, ExposureMap &map) {
  for (const auto &fp : c.get_functions()) {
    const ir::function_decl *f = raw(fp);
    if (!f)
      continue;
    note_type(f->get_return_type(), map);
    for (const auto &p : f->get_parameters()) {
      if (p)
        note_type(p->get_type(), map);
    }
  }
  for (const auto &vp : c.get_variables()) {
    if (const ir::var_decl *v = raw(vp); v != nullptr)
      note_type(v->get_type(), map);
  }
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
        stderr, "  visit {:22} changes={}  {}", typeid(*d).name(),
        static_cast<int>(d->has_changes()), d->get_pretty_representation()
      );
    }
    if (!pre || !d || !d->has_changes())
      return true;
    if (auto *cd = dynamic_cast<class_diff *>(d)) {
      on_class(*cd);
    } else if (auto *ud = dynamic_cast<union_diff *>(d)) {
      on_class_or_union(*ud, /*bases_changed=*/false);
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

  void emit(ChangeKind k, const ir::decl_base *decl, std::uint32_t n, bool append_only) {
    if (!n)
      return;
    out_.push_back(
      TypeEvent{
        .kind = k,
        .type_name = type_key(decl),
        .declared_in = decl_file(decl),
        .count = n,
        .exposure = TypeExposure::unknown,
        .third_party = false,
        .append_only = append_only
      }
    );
  }

  static const ir::type_base *canonical_key(const ir::type_base_sptr &t) {
    return t->get_canonical_type() ? t->get_canonical_type().get() : t.get();
  }

  void on_class_or_union(class_or_union_diff &d, bool bases_changed) {
    const auto f = d.first_class_or_union();
    const auto s = d.second_class_or_union();
    if (!f || !s)
      return;
    if (!first_time(canonical_key(f)))
      return;

    const auto inserted = static_cast<std::uint32_t>(d.inserted_data_members().size());
    const auto deleted = static_cast<std::uint32_t>(d.deleted_data_members().size());

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
      if (
        it != off1.end() && it->second != ir::get_data_member_offset(m) &&
        !changed_names.contains(m->get_name())
      )
        ++offset_changed;
    }
    // Append-only growth: every new member lies beyond the old end, and
    // nothing else moved, vanished or changed type.
    bool inserted_beyond_end = true;
    for (const auto &[name, m] : d.inserted_data_members()) {
      if (m && ir::get_data_member_offset(m) < f->get_size_in_bits()) {
        inserted_beyond_end = false;
        break;
      }
    }
    const bool append_only = inserted_beyond_end && deleted == 0 && type_changed == 0 &&
                             offset_changed == 0 && !bases_changed;

    emit(ChangeKind::field_added_to_struct, f.get(), inserted, append_only);
    emit(ChangeKind::field_removed_from_struct, f.get(), deleted, false);
    emit(ChangeKind::field_type_changed, f.get(), type_changed, false);
    emit(ChangeKind::member_offset_changed, f.get(), offset_changed, false);
    if (f->get_size_in_bits() != s->get_size_in_bits())
      emit(ChangeKind::type_size_changed, f.get(), 1, append_only);
  }

  void on_class(class_diff &d) {
    const auto f = d.first_class_decl();
    const auto s = d.second_class_decl();
    if (!f || !s)
      return;
    const auto bases = static_cast<std::uint32_t>(
      d.inserted_bases().size() + d.deleted_bases().size() + d.changed_bases().size()
    );
    const bool fresh = !seen_.contains(canonical_key(f));
    on_class_or_union(d, bases != 0);
    if (!fresh)
      return;
    emit(ChangeKind::base_class_changed, f.get(), bases, false);

    // A virtual slot inserted, a method changing virtuality, or a virtual
    // method whose slot index moved. A DELETED virtual is an API removal and
    // is counted as a removed public symbol whatever its ELF binding
    // (occupies_vtable_slot keeps inline virtuals out of the vague-linkage
    // quarantine).
    std::uint32_t vt = 0;
    if (!f->has_vtable() && s->has_vtable())
      ++vt;
    for (const auto &[name, m] : d.inserted_member_fns()) {
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
      const bool moved =
        va && vb &&
        ir::get_member_function_vtable_offset(*ma) != ir::get_member_function_vtable_offset(*mb);
      if (va != vb || moved)
        ++vt;
    }
    emit(ChangeKind::vtable_changed, f.get(), vt, false);
  }

  void on_enum(enum_diff &d) {
    const auto f = d.first_enum();
    const auto s = d.second_enum();
    if (!f || !s)
      return;
    if (!first_time(canonical_key(f)))
      return;
    emit(
      ChangeKind::enum_case_added, f.get(),
      static_cast<std::uint32_t>(d.inserted_enumerators().size()), false
    );
    emit(
      ChangeKind::enum_case_removed, f.get(),
      static_cast<std::uint32_t>(d.deleted_enumerators().size()), false
    );
    // A changed underlying size is a layout change of the enum type.
    if (f->get_size_in_bits() != s->get_size_in_bits())
      emit(ChangeKind::type_size_changed, f.get(), 1, false);
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
  bool weak;
  bool vtable_slot; ///< a virtual member function: reached through the vtable
};

/// @brief A virtual member function of one of the LIBRARY'S OWN classes
///        occupies a vtable slot consumers dispatch through, so its ELF
///        binding (usually weak for inline virtuals) says nothing about
///        whether it is ABI. Virtuals of third-party class instantiations
///        (std::_Sp_counted_ptr_inplace<...> from libstdc++) are the
///        library's private copies and stay vague linkage.
bool occupies_vtable_slot(const ir::function_decl *f, const ShippedHeaders &shipped) {
  const auto *m = ir::is_method_decl(f);
  if (m == nullptr || !ir::get_member_function_is_virtual(*m))
    return false;
  return declared_in_own_headers(decl_file(m), shipped);
}

/// @brief vtables, typeinfo, typeinfo names and VTTs are emitted by the
///        compiler as a consequence of class changes; they are counted there,
///        not as symbol additions/removals.
bool compiler_generated(const std::string &linkage) {
  return linkage.starts_with("_ZTV") || linkage.starts_with("_ZTI") ||
         linkage.starts_with("_ZTS") || linkage.starts_with("_ZTT");
}

template <class Map>
void collect_functions(const Map &m, const ShippedHeaders &shipped, std::vector<SymbolSide> &out) {
  for (const auto &[key, fn] : m) {
    const ir::function_decl *f = raw(fn);
    if (!f || compiler_generated(f->get_linkage_name()))
      continue;
    out.push_back(
      SymbolSide{
        .linkage = f->get_linkage_name(),
        .pretty = f->get_pretty_representation(),
        .pairing = f->get_qualified_name(),
        .version = version_of(f->get_symbol()),
        .is_function = true,
        .weak = is_weak(f->get_symbol()),
        .vtable_slot = occupies_vtable_slot(f, shipped)
      }
    );
  }
}

template <class Map>
void collect_variables(const Map &m, std::vector<SymbolSide> &out) {
  for (const auto &[key, v] : m) {
    const ir::var_decl *d = raw(v);
    if (!d || compiler_generated(d->get_linkage_name()))
      continue;
    out.push_back(
      SymbolSide{
        .linkage = d->get_linkage_name(),
        .pretty = d->get_pretty_representation(),
        .pairing = d->get_qualified_name(),
        .version = version_of(d->get_symbol()),
        .is_function = false,
        .weak = is_weak(d->get_symbol()),
        .vtable_slot = false
      }
    );
  }
}

/// @brief Where a symbol event is tallied.
ChangeCounts &tally_for(SharedObjectDiff &out, const SymbolSide &s) {
  if (is_private_version_node(s.version))
    return out.private_node_counts;
  if (!s.vtable_slot && is_vague_linkage(s.linkage, s.weak))
    return out.vague_linkage_counts;
  return out.public_counts;
}

void record_event(
  SharedObjectDiff &out, const ports::CompareOptions &opt, ChangeKind k, const SymbolSide &s,
  std::string pretty
) {
  if (out.symbol_events.size() >= opt.max_symbol_events) {
    out.symbol_events_truncated = true;
    return;
  }
  out.symbol_events.push_back(
    SymbolEvent{
      .kind = k,
      .symbol = SymbolName{s.linkage},
      .pretty = std::move(pretty),
      .version = s.version.empty() ? std::nullopt : std::optional{VersionNode{s.version}},
      .weak = s.weak,
      .vtable_slot = s.vtable_slot
    }
  );
}

/// @brief Splits symbol events into public/private/vague tallies and detects
///        the two symbol-level patterns the raw lists hide: signature changes
///        that the mangler turned into remove+add, and policy-driven renames.
void classify_symbols(
  corpus_diff &d, const ports::CompareOptions &opt, const ShippedHeaders &shipped,
  SharedObjectDiff &out
) {
  std::vector<SymbolSide> removed;
  std::vector<SymbolSide> added;
  collect_functions(d.deleted_functions(), shipped, removed);
  collect_functions(d.added_functions(), shipped, added);
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
      tally_for(out, removed[i]).add(ChangeKind::function_signature_changed);
      record_event(
        out, opt, ChangeKind::function_signature_changed, removed[i],
        removed[i].pretty + "  ->  " + added[it->second].pretty
      );
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
    tally_for(out, s).add(k);
    record_event(out, opt, k, s, s.pretty);
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
    const SymbolSide side{
      .linkage = a->get_linkage_name(),
      .pretty = a->get_pretty_representation(),
      .pairing = a->get_qualified_name(),
      .version = version_of(a->get_symbol()),
      .is_function = true,
      .weak = is_weak(a->get_symbol()),
      .vtable_slot = occupies_vtable_slot(a.get(), shipped)
    };
    tally_for(out, side).add(ChangeKind::function_signature_changed);
    record_event(
      out, opt, ChangeKind::function_signature_changed, side,
      a->get_pretty_representation() + "  ->  " + b->get_pretty_representation()
    );
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

/// @brief Include-relative paths of every header file below the given roots.
ShippedHeaders shipped_headers(std::initializer_list<std::filesystem::path> roots) {
  ShippedHeaders sh;
  for (const auto &root : roots) {
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec))
      continue;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec))
        sh.add(it->path().lexically_relative(root).generic_string());
    }
  }
  return sh;
}

/// @brief Number of trailing path components `a` and `b` share.
std::size_t common_suffix_components(std::string_view a, std::string_view b) {
  std::size_t n = 0;
  for (;;) {
    const auto sa = a.rfind('/');
    const auto sb = b.rfind('/');
    const auto ta = sa == std::string_view::npos ? a : a.substr(sa + 1);
    const auto tb = sb == std::string_view::npos ? b : b.substr(sb + 1);
    if (ta != tb || ta.empty())
      return n;
    ++n;
    if (sa == std::string_view::npos || sb == std::string_view::npos)
      return n;
    a = a.substr(0, sa);
    b = b.substr(0, sb);
  }
}

} // namespace

// ----------------------------------------------------------------------------

void ShippedHeaders::add(std::string relative_path) {
  const auto slash = relative_path.rfind('/');
  std::string base = slash == std::string::npos ? relative_path : relative_path.substr(slash + 1);
  by_basename[std::move(base)].push_back(std::move(relative_path));
}

bool declared_in_own_headers(std::string_view dwarf_path, const ShippedHeaders &shipped) {
  if (dwarf_path.empty() || shipped.empty())
    return true; // nothing to attribute against: keep the event public
  const auto slash = dwarf_path.rfind('/');
  const auto base = slash == std::string_view::npos ? dwarf_path : dwarf_path.substr(slash + 1);
  const auto it = shipped.by_basename.find(std::string{base});
  if (it == shipped.by_basename.end())
    return false;
  constexpr std::array<std::string_view, 2> sys_roots{"/usr/include/", "/usr/lib/gcc/"};
  std::string_view sys_rel;
  for (const auto r : sys_roots) {
    if (dwarf_path.starts_with(r)) {
      sys_rel = dwarf_path.substr(r.size());
      break;
    }
  }
  if (sys_rel.empty())
    return true;
  return std::ranges::any_of(it->second, [&](const std::string &s) {
    return s == sys_rel || common_suffix_components(dwarf_path, s) >= 2;
  });
}

std::string AbigailComparer::version() const {
  std::string maj;
  std::string min;
  std::string rev;
  std::string suf;
  ::abigail::abigail_get_library_version(maj, min, rev, suf);
  return std::format("libabigail {}.{}.{}{}", maj, min, rev, suf);
}

Result<SharedObjectDiff> AbigailComparer::compare(
  const ports::Side &a, const ports::Side &b, const ports::CompareOptions &opt
) const {
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
    .vague_linkage_counts = {},
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
  if (out.coverage.exported_functions_1 + out.coverage.exported_functions_2 == 0) {
    out.language = Language::unknown;
  } else {
    out.language =
      out.coverage.mangled_fraction() >= opt.cxx_mangled_fraction ? Language::cxx : Language::c;
  }

  auto ctxt = std::make_shared<diff_context>();
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

  // Exposure through the exported interface, from both sides (a type may
  // enter the interface only in v2).
  ExposureMap exposure;
  const bool can_expose = la.debug_info_found || lb.debug_info_found;
  if (can_expose) {
    exposure_of(*la.corpus, exposure);
    exposure_of(*lb.corpus, exposure);
  }
  // Attribute each type to the package that declares it (REVIEW.md §1.6).
  const auto shipped = shipped_headers({a.public_headers, b.public_headers});
  for (auto &ev : type_events) {
    ev.third_party = !declared_in_own_headers(ev.declared_in, shipped);
    if (can_expose) {
      const auto it = exposure.find(ev.type_name);
      ev.exposure = it == exposure.end() ? TypeExposure::not_in_interface : it->second;
    }
    (ev.third_party ? out.third_party_counts : out.public_counts).add(ev.kind, ev.count);
  }
  out.type_events = std::move(type_events);
  classify_symbols(*d, opt, shipped, out);
  return out;
}

} // namespace abistudy::abigail
