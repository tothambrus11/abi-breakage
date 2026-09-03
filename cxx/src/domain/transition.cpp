#include "domain/transition.hpp"

#include <algorithm>

#include "domain/symbols.hpp"

namespace abistudy {

Transition rollup(const PairResult &p, const HeaderResult *h) {
  Transition t{
    .id = p.id,
    .source = p.source,
    .language = Language::unknown,
    .mangled_fraction = 0,
    .level = release_level(p.upstream_1.get(), p.upstream_2.get()),
    .strict = {},
    .lenient = {},
    .third_party = {},
    .private_node = {},
    .vague_linkage = {},
    .layout_types_strict = 0,
    .layout_types_lenient = 0,
    .soname_changed = false,
    .mass_rename = false,
    .debug_info_complete = true,
    .headers = std::nullopt,
    .header_coverage_poor = false,
    .symbols = {}
  };
  auto declared_of = [&](const SymbolName &s) {
    if (!h)
      return Declared::unknown;
    const auto it = h->symbol_declared.find(s.get());
    return it == h->symbol_declared.end() ? Declared::unknown : it->second;
  };

  std::set<std::string> layout_strict;
  std::set<std::string> layout_lenient;
  for (const auto &o : p.objects) {
    t.strict.merge(o.public_counts);
    t.lenient.merge(o.public_counts);
    t.third_party.merge(o.third_party_counts);
    t.private_node.merge(o.private_node_counts);
    t.vague_linkage.merge(o.vague_linkage_counts);
    t.soname_changed |= o.soname_1 != o.soname_2;
    t.mass_rename |= o.mass_rename;
    t.debug_info_complete &= o.coverage.debug_info_complete();
    t.mangled_fraction = std::max(t.mangled_fraction, o.coverage.mangled_fraction());
    if (o.language == Language::cxx) {
      t.language = Language::cxx;
    } else if (t.language == Language::unknown) {
      t.language = o.language;
    }

    for (const auto &e : o.type_events) {
      if (e.third_party || !is_layout_kind(e.kind))
        continue;
      layout_strict.insert(e.type_name);
      if (layout_event_breaks_leniently(e)) {
        layout_lenient.insert(e.type_name);
      } else {
        t.lenient.subtract(e.kind, e.count);
        t.symbols.layout_events_excluded += e.count;
      }
    }
    for (const auto &e : o.symbol_events) {
      const bool removed = e.kind == ChangeKind::symbol_removed;
      const bool resigned = e.kind == ChangeKind::function_signature_changed;
      if (!removed && !resigned)
        continue;
      if (
        (e.version && is_private_version_node(e.version->get())) ||
        is_vague_linkage(e.symbol.get(), e.weak)
      )
        continue; // not a public event; tallied elsewhere
      const auto d = declared_of(e.symbol);
      auto &declared = removed ? t.symbols.removed_declared : t.symbols.signature_declared;
      auto &undeclared = removed ? t.symbols.removed_undeclared : t.symbols.signature_undeclared;
      auto &unknown = removed ? t.symbols.removed_unknown : t.symbols.signature_unknown;
      switch (d) {
      case Declared::yes:
        ++declared;
        break;
      case Declared::no:
        ++undeclared;
        t.lenient.subtract(e.kind, 1);
        break;
      case Declared::unknown:
        ++unknown;
        break;
      }
    }
  }
  t.layout_types_strict = static_cast<std::uint32_t>(layout_strict.size());
  t.layout_types_lenient = static_cast<std::uint32_t>(layout_lenient.size());

  if (h && h->diff) {
    t.headers = h->diff;
    for (auto *c : {&t.strict, &t.lenient}) {
      c->add(ChangeKind::inline_body_changed, h->diff->inline_body_changed);
      c->add(ChangeKind::macro_value_changed, h->diff->macro_value_changed_nonversion);
    }
    t.header_coverage_poor = h->coverage_1.poor() || h->coverage_2.poor();
  }
  return t;
}

std::set<ChangeKind> relevant_kinds(const Transition &t, Framing f, BreakDefinition d) {
  std::set<ChangeKind> s;
  for (const auto &[k, n] : counts_of(t, d).items()) {
    if (in_framing(f, k))
      s.insert(k);
  }
  return s;
}

bool is_affected(const Transition &t, Framing f, BreakDefinition d) {
  return std::ranges::any_of(counts_of(t, d).items(), [&](const auto &kv) {
    return in_framing(f, kv.first);
  });
}

bool fully_absorbable(const std::set<ChangeKind> &kinds) {
  return !kinds.empty() && std::ranges::all_of(kinds, [](ChangeKind k) { return is_absorbed(k); });
}

} // namespace abistudy
