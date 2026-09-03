#include "domain/summary.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <sstream>

namespace abistudy {
namespace {

using Ptrs = std::vector<const Transition *>;

template <class Pred>
Ptrs where(const Ptrs &ts, Pred p) {
  Ptrs out;
  for (const auto *t : ts) {
    if (p(*t))
      out.push_back(t);
  }
  return out;
}

std::set<SourceName> libraries_of(const Ptrs &ts) {
  std::set<SourceName> s;
  for (const auto *t : ts)
    s.insert(t->source);
  return s;
}

/// @brief Outcomes grouped by library, in a stable order, for the bootstrap.
template <class Pred>
std::vector<std::vector<bool>> clusters(const Ptrs &ts, Pred p) {
  std::map<SourceName, std::vector<bool>> by;
  for (const auto *t : ts)
    by[t->source].push_back(p(*t));
  std::vector<std::vector<bool>> out;
  out.reserve(by.size());
  for (auto &[k, v] : by)
    out.push_back(std::move(v));
  return out;
}

Json interval(const Interval &iv) { return Json::array({iv.lo, iv.hi}); }

double ratio(std::size_t num, std::size_t den) {
  return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
}

/// @brief A transition-level proportion with its cluster-bootstrap interval,
///        plus the library-level share with its own interval.
template <class Pred>
Json proportion(const Ptrs &ts, Pred p, const BootstrapOptions &bo) {
  const auto hits = where(ts, p);
  Json j = {{"n", ts.size()}, {"count", hits.size()}, {"share", ratio(hits.size(), ts.size())}};
  if (!ts.empty()) {
    const auto cl = clusters(ts, p);
    j["ci"] = interval(cluster_bootstrap(cl, bo));
    std::vector<bool> per_lib;
    per_lib.reserve(cl.size());
    for (const auto &c : cl)
      per_lib.push_back(std::ranges::any_of(c, [](bool b) { return b; }));
    const auto lib_hits = static_cast<std::size_t>(std::ranges::count(per_lib, true));
    j["libraries"] = cl.size();
    j["libraries_count"] = lib_hits;
    j["libraries_share"] = ratio(lib_hits, cl.size());
    j["libraries_ci"] = interval(library_bootstrap(per_lib, bo));
  }
  return j;
}

Json frequency(const Ptrs &ts, const BootstrapOptions &bo) {
  const auto dwarf = where(ts, [](const Transition &t) { return t.debug_info_complete; });
  Json rows = Json::array();
  for (const auto k : all_change_kinds) {
    // Layout, enum and vtable kinds are only observable with DWARF on both sides.
    const bool needs_dwarf =
      !is_header_kind(k) && k != ChangeKind::symbol_added && k != ChangeKind::symbol_removed &&
      k != ChangeKind::symbol_version_renamed && k != ChangeKind::function_signature_changed;
    const auto &pop = needs_dwarf ? dwarf : ts;
    std::vector<std::uint32_t> per;
    std::uint64_t events = 0;
    for (const auto *t : pop) {
      if (const auto n = t->strict.get(k)) {
        per.push_back(n);
        events += n;
      }
    }
    if (per.empty())
      continue;
    Json row = {
      {"kind", to_string(k)},
      {"mechanism", to_string(mechanism_for(k))},
      {"absorbed", is_absorbed(k)},
      {"strict", proportion(
                   pop, [k](const Transition &t) { return t.strict.has(k); }, bo
                 )},
      {"lenient", proportion(
                    pop, [k](const Transition &t) { return t.lenient.has(k); }, bo
                  )},
      {"events", events},
      {"median", median(per)},
      {"max", *std::ranges::max_element(per)}
    };
    rows.push_back(std::move(row));
  }
  Json layout = {
    {"strict", proportion(
                 dwarf, [](const Transition &t) { return t.layout_types_strict > 0; }, bo
               )},
    {"lenient",
     proportion(dwarf, [](const Transition &t) { return t.layout_types_lenient > 0; }, bo)}
  };
  return {
    {"n", ts.size()},
    {"n_dwarf", dwarf.size()},
    {"libraries", libraries_of(ts).size()},
    {"rows", rows},
    {"layout_types", layout}
  };
}

Json breaks(const Ptrs &ts, Framing f, BreakDefinition d, const BootstrapOptions &bo) {
  auto affected = [f, d](const Transition &t) { return is_affected(t, f, d); };
  Json j = proportion(ts, affected, bo);
  const auto hit = where(ts, affected);
  const auto declared = where(hit, [](const Transition &t) { return t.soname_changed; });
  j["declared"] = declared.size();
  j["silent"] = hit.size() - declared.size();
  Json by_level = Json::object();
  for (const auto l : all_release_levels) {
    const auto pop = where(ts, [l](const Transition &t) { return t.level == l; });
    if (pop.empty())
      continue;
    by_level[std::string{to_string(l)}] = proportion(pop, affected, bo);
  }
  j["by_level"] = by_level;
  return j;
}

Json rescue(const Ptrs &ts, Framing f, BreakDefinition d) {
  const auto affected = where(ts, [&](const Transition &t) { return is_affected(t, f, d); });
  Ptrs fully;
  for (const auto *t : affected) {
    if (fully_absorbable(relevant_kinds(*t, f, d)))
      fully.push_back(t);
  }
  Json mechs = Json::array();
  for (const auto m : all_mechanisms) {
    std::uint32_t needed = 0;
    std::uint32_t sole = 0;
    for (const auto *t : fully) {
      const auto ks = relevant_kinds(*t, f, d);
      const bool uses =
        std::ranges::any_of(ks, [&](ChangeKind k) { return mechanism_for(k) == m; });
      const bool only =
        uses && std::ranges::all_of(ks, [&](ChangeKind k) { return mechanism_for(k) == m; });
      needed += uses ? 1 : 0;
      sole += only ? 1 : 0;
    }
    mechs.push_back({{"mechanism", to_string(m)}, {"needed_by", needed}, {"sole_reason", sole}});
  }
  return {
    {"affected", affected.size()},
    {"fully_rescued", fully.size()},
    {"fully_share", ratio(fully.size(), affected.size())},
    {"mechanisms", mechs}
  };
}

Json header_section(const Ptrs &ts) {
  const auto withh = where(ts, [](const Transition &t) { return t.headers.has_value(); });
  auto count = [&](auto pred) { return where(withh, pred).size(); };
  std::uint64_t body_events = 0;
  std::uint64_t macro_events = 0;
  std::vector<std::pair<std::string, std::uint32_t>> top;
  for (const auto *t : withh) {
    body_events += t->headers.value().inline_body_changed;
    macro_events += t->headers.value().macro_value_changed;
    if (t->headers.value().inline_body_changed)
      top.emplace_back(t->id, t->headers.value().inline_body_changed);
  }
  std::ranges::sort(top, std::greater<>{}, [](const auto &p) { return p.second; });
  if (top.size() > 10)
    top.resize(10);
  Json topj = Json::array();
  for (const auto &[id, n] : top)
    topj.push_back({{"id", id}, {"bodies", n}});
  return {
    {"with_data", withh.size()},
    {"with_definitions",
     count([](const Transition &t) { return t.headers.value().definitions_common > 0; })},
    {"body_changed",
     count([](const Transition &t) { return t.headers.value().inline_body_changed > 0; })},
    {"body_events", body_events},
    {"macro_changed",
     count([](const Transition &t) { return t.headers.value().macro_value_changed > 0; })},
    {"macro_events", macro_events},
    {"macro_changed_nonversion", count([](const Transition &t) {
       return t.headers.value().macro_value_changed_nonversion > 0;
     })},
    {"poor_coverage", count([](const Transition &t) { return t.header_coverage_poor; })},
    {"top", topj}
  };
}

Json symbol_section(const Ptrs &ts) {
  SymbolStrata sum;
  std::uint32_t with_join = 0;
  for (const auto *t : ts) {
    const auto &s = t->symbols;
    sum.removed_declared += s.removed_declared;
    sum.removed_undeclared += s.removed_undeclared;
    sum.removed_unknown += s.removed_unknown;
    sum.signature_declared += s.signature_declared;
    sum.signature_undeclared += s.signature_undeclared;
    sum.signature_unknown += s.signature_unknown;
    sum.layout_events_excluded += s.layout_events_excluded;
    with_join += (s.removed_declared + s.removed_undeclared + s.signature_declared +
                  s.signature_undeclared) > 0
                   ? 1
                   : 0;
  }
  return {
    {"removed_declared", sum.removed_declared},
    {"removed_undeclared", sum.removed_undeclared},
    {"removed_unknown", sum.removed_unknown},
    {"signature_declared", sum.signature_declared},
    {"signature_undeclared", sum.signature_undeclared},
    {"signature_unknown", sum.signature_unknown},
    {"layout_events_excluded", sum.layout_events_excluded},
    {"transitions_with_join", with_join}
  };
}

Json per_library(const Ptrs &ts) {
  struct Row {
    Language language = Language::unknown;
    std::uint32_t transitions = 0, strict = 0, lenient = 0;
  };
  std::map<SourceName, Row> rows;
  for (const auto *t : ts) {
    auto &r = rows[t->source];
    if (t->language == Language::cxx || r.language == Language::unknown)
      r.language = t->language;
    ++r.transitions;
    r.strict += is_affected(*t, Framing::binary, BreakDefinition::strict) ? 1 : 0;
    r.lenient += is_affected(*t, Framing::binary, BreakDefinition::lenient) ? 1 : 0;
  }
  Json out = Json::array();
  for (const auto &[src, r] : rows) {
    out.push_back(
      {{"source", src},
       {"language", to_string(r.language)},
       {"transitions", r.transitions},
       {"breaks_strict", r.strict},
       {"breaks_lenient", r.lenient}}
    );
  }
  return out;
}

Json sensitivity(const Ptrs &ts, const std::vector<double> &thresholds) {
  Json out = Json::object();
  for (const auto th : thresholds) {
    std::size_t cxx = 0;
    for (const auto *t : ts)
      cxx += t->mangled_fraction >= th ? 1 : 0;
    out[std::format("{:.2f}", th)] = {{"cxx", cxx}, {"c", ts.size() - cxx}};
  }
  return out;
}

} // namespace

Json summarize(const SummaryInputs &in, const SummaryOptions &o) {
  Ptrs all;
  all.reserve(in.transitions.size());
  for (const auto &t : in.transitions)
    all.push_back(&t);
  const auto mass = where(all, [](const Transition &t) { return t.mass_rename; });
  const auto good = where(all, [](const Transition &t) { return !t.mass_rename; });
  const auto c_only = where(good, [](const Transition &t) { return t.language == Language::c; });
  const auto cxx_only =
    where(good, [](const Transition &t) { return t.language == Language::cxx; });

  Json by_level = Json::object();
  for (const auto l : all_release_levels) {
    by_level[std::string{to_string(l)}] =
      where(good, [l](const Transition &t) { return t.level == l; }).size();
  }
  Json s;
  s["corpus"] = {
    {"transitions", good.size()},
    {"libraries", libraries_of(good).size()},
    {"objects", in.objects},
    {"c_transitions", c_only.size()},
    {"c_libraries", libraries_of(c_only).size()},
    {"cxx_transitions", cxx_only.size()},
    {"cxx_libraries", libraries_of(cxx_only).size()},
    {"mass_rename_excluded", mass.size()},
    {"mass_rename_libraries", Json(libraries_of(mass))},
    {"errored", in.errored},
    {"not_attempted", in.not_attempted},
    {"debug_info_incomplete",
     where(good, [](const Transition &t) { return !t.debug_info_complete; }).size()},
    {"by_level", by_level}
  };

  ChangeCounts tp;
  ChangeCounts pv;
  ChangeCounts vg;
  for (const auto *t : all) {
    tp.merge(t->third_party);
    pv.merge(t->private_node);
    vg.merge(t->vague_linkage);
  }
  s["filtered"] = {{"third_party", tp}, {"private_node", pv}, {"vague_linkage", vg}};

  s["frequency"] = {
    {"all", frequency(good, o.bootstrap)},
    {"c", frequency(c_only, o.bootstrap)},
    {"cxx", frequency(cxx_only, o.bootstrap)}
  };

  Json br = Json::object();
  Json rs = Json::object();
  for (const auto f : all_framings) {
    Json bd = Json::object();
    Json rd = Json::object();
    for (const auto d : all_definitions) {
      bd[std::string{to_string(d)}] = breaks(good, f, d, o.bootstrap);
      rd[std::string{to_string(d)}] = rescue(good, f, d);
    }
    br[std::string{to_string(f)}] = bd;
    rs[std::string{to_string(f)}] = rd;
  }
  s["breaks"] = br;
  s["rescue"] = rs;
  s["headers"] = header_section(good);
  s["symbols"] = symbol_section(good);
  s["sensitivity"] = {{"language_threshold", sensitivity(good, o.language_thresholds)}};
  s["libraries"] = per_library(good);

  Json per = Json::array();
  for (const auto &t : in.transitions) {
    per.push_back(
      {{"id", t.id},
       {"source", t.source},
       {"language", to_string(t.language)},
       {"level", to_string(t.level)},
       {"strict", t.strict},
       {"lenient", t.lenient},
       {"layout_types", {t.layout_types_strict, t.layout_types_lenient}},
       {"soname_changed", t.soname_changed},
       {"mass_rename", t.mass_rename},
       {"debug_info_complete", t.debug_info_complete},
       {"header_coverage_poor", t.header_coverage_poor}}
    );
  }
  s["transitions"] = per;
  return s;
}

// ----------------------------------------------------------------------------
// Text rendering
// ----------------------------------------------------------------------------

namespace {

std::string pct(const Json &p) {
  return std::format("{:5.1f}%", 100.0 * p.at("share").get<double>());
}
std::string ci(const Json &p) {
  if (!p.contains("ci"))
    return "";
  const auto &c = p.at("ci");
  return std::format("[{:4.1f}, {:4.1f}]", 100.0 * c[0].get<double>(), 100.0 * c[1].get<double>());
}
std::string lib_share(const Json &p) {
  if (!p.contains("libraries_share"))
    return "";
  return std::format(
    "{:>3}/{:<3} {:5.1f}%", p.at("libraries_count").get<std::uint64_t>(),
    p.at("libraries").get<std::uint64_t>(), 100.0 * p.at("libraries_share").get<double>()
  );
}

void rule(std::ostringstream &o, char c = '=') { o << std::string(118, c) << '\n'; }

void frequency_text(std::ostringstream &o, std::string_view title, const Json &f) {
  o << '\n';
  rule(o);
  o << std::format(
    "{}\n  n = {} transitions ({} libraries); layout/enum/vtable rows over the {} with DWARF on "
    "both sides\n",
    title, f.at("n").get<std::uint64_t>(), f.at("libraries").get<std::uint64_t>(),
    f.at("n_dwarf").get<std::uint64_t>()
  );
  rule(o);
  o << std::format(
    "{:<27}{:>7} {:>7} {:>14} {:>7} {:>13}   {:>9} {:>6} {:>5}  {}\n", "change kind", "strict", "%",
    "95% CI", "lenient", "libraries", "events", "median", "max", "mechanism"
  );
  rule(o, '-');
  for (const auto &r : f.at("rows")) {
    const auto &st = r.at("strict");
    const auto &le = r.at("lenient");
    o << std::format(
      "{:<27}{:>7} {:>7} {:>14} {:>7} {:>13}   {:>9} {:>6.0f} {:>5}  {}\n",
      r.at("kind").get<std::string>(), st.at("count").get<std::uint64_t>(), pct(st), ci(st),
      le.at("count").get<std::uint64_t>(), lib_share(st), r.at("events").get<std::uint64_t>(),
      r.at("median").get<double>(), r.at("max").get<std::uint64_t>(),
      r.at("mechanism").get<std::string>()
    );
  }
  const auto &lt = f.at("layout_types");
  o << std::format(
    "{:<27}{:>7} {:>7} {:>14} {:>7} {:>13}   (types with any layout change; primary layout "
    "measure)\n",
    "[layout-changed types]", lt.at("strict").at("count").get<std::uint64_t>(),
    pct(lt.at("strict")), ci(lt.at("strict")), lt.at("lenient").at("count").get<std::uint64_t>(),
    lib_share(lt.at("strict"))
  );
}

} // namespace

std::string render_text(const Json &s) {
  std::ostringstream o;
  const auto &c = s.at("corpus");
  rule(o);
  o << "CORPUS\n";
  rule(o);
  o << std::format("release transitions analysed          : {}\n", c.at("transitions").get<int>());
  o << std::format("  libraries (Debian source packages)  : {}\n", c.at("libraries").get<int>());
  o << std::format("  shared objects compared             : {}\n", c.at("objects").get<int>());
  o << std::format(
    "  C transitions                       : {}  ({} libraries)\n",
    c.at("c_transitions").get<int>(), c.at("c_libraries").get<int>()
  );
  o << std::format(
    "  C++ transitions                     : {}  ({} libraries)\n",
    c.at("cxx_transitions").get<int>(), c.at("cxx_libraries").get<int>()
  );
  o << std::format("  by release level                    : {}\n", c.at("by_level").dump());
  o << std::format(
    "  excluded: mass symbol rename policy : {}  {}\n", c.at("mass_rename_excluded").get<int>(),
    c.at("mass_rename_libraries").dump()
  );
  o << std::format(
    "  excluded: pair failed / no objects  : {}   not attempted (budget/deadline): {}\n",
    c.at("errored").get<int>(), c.at("not_attempted").get<int>()
  );
  o << std::format(
    "  debug info missing on a side        : {}  (symbol-only; excluded from layout/enum rows)\n",
    c.at("debug_info_incomplete").get<int>()
  );

  const auto &fl = s.at("filtered");
  o << "\nEVENTS EXCLUDED BY THE ATTRIBUTION FILTERS\n";
  o << std::format(
    "  types not declared in the library's headers : {}\n", fl.at("third_party").dump()
  );
  o << std::format(
    "  symbols in private ELF version nodes        : {}\n", fl.at("private_node").dump()
  );
  o << std::format(
    "  vague-linkage (weak C++) symbols            : {}\n", fl.at("vague_linkage").dump()
  );

  frequency_text(
    o, "FREQUENCY OF CHANGE KINDS ACROSS CONSECUTIVE RELEASES  (all libraries)",
    s.at("frequency").at("all")
  );
  frequency_text(o, "C LIBRARIES ONLY", s.at("frequency").at("c"));
  frequency_text(o, "C++ LIBRARIES ONLY", s.at("frequency").at("cxx"));

  o << '\n';
  rule(o);
  o << "BREAK RATES  (strict = every public event; lenient = opaque-by-convention growth and "
       "undeclared symbols excluded)\n";
  rule(o);
  o << std::format(
    "{:<22}{:<9}{:>9} {:>7} {:>14} {:>16} {:>14}   {:>8} {:>7}\n", "framing", "def.", "affected",
    "%", "95% CI", "libraries", "lib 95% CI", "declared", "silent"
  );
  rule(o, '-');
  for (const auto &[f, defs] : s.at("breaks").items()) {
    for (const auto &[d, b] : defs.items()) {
      o << std::format(
        "{:<22}{:<9}{:>9} {:>7} {:>14} {:>16} {:>14}   {:>8} {:>7}\n", f, d,
        b.at("count").get<std::uint64_t>(), pct(b), ci(b), lib_share(b),
        b.contains("libraries_ci")
          ? std::format(
              "[{:4.1f}, {:4.1f}]", 100.0 * b.at("libraries_ci")[0].get<double>(),
              100.0 * b.at("libraries_ci")[1].get<double>()
            )
          : "",
        b.at("declared").get<std::uint64_t>(), b.at("silent").get<std::uint64_t>()
      );
    }
  }
  o << "\nBINARY BREAKS BY RELEASE LEVEL  (strict / lenient)\n";
  rule(o, '-');
  const auto &bl_s = s.at("breaks").at("binary").at("strict").at("by_level");
  const auto &bl_l = s.at("breaks").at("binary").at("lenient").at("by_level");
  for (const auto &[level, b] : bl_s.items()) {
    const auto &l = bl_l.at(level);
    o << std::format(
      "  {:<10} n={:<5} strict {:>4} {:>7} {:>14}   lenient {:>4} {:>7} {:>14}\n", level,
      b.at("n").get<std::uint64_t>(), b.at("count").get<std::uint64_t>(), pct(b), ci(b),
      l.at("count").get<std::uint64_t>(), pct(l), ci(l)
    );
  }

  o << '\n';
  rule(o);
  o << "WOULD A RESILIENT BOUNDARY HAVE SAVED THE RELEASE?\n";
  rule(o);
  o << std::format(
    "{:<22}{:<9}{:>9} {:>8} {:>8}   {:<26}{:<26}{:<26}{}\n", "framing", "def.", "affected",
    "rescued", "%", "opaque layout", "non-frozen enum", "resilient dispatch", "opt-in inlining"
  );
  rule(o, '-');
  for (const auto &[f, defs] : s.at("rescue").items()) {
    for (const auto &[d, r] : defs.items()) {
      std::string cells;
      for (const auto &m : r.at("mechanisms")) {
        cells += std::format(
          "{:<26}", std::format(
                      "{} (sole {})", m.at("needed_by").get<std::uint64_t>(),
                      m.at("sole_reason").get<std::uint64_t>()
                    )
        );
      }
      o << std::format(
        "{:<22}{:<9}{:>9} {:>8} {:>7.1f}%   {}\n", f, d, r.at("affected").get<std::uint64_t>(),
        r.at("fully_rescued").get<std::uint64_t>(), 100.0 * r.at("fully_share").get<double>(), cells
      );
    }
  }

  const auto &sy = s.at("symbols");
  o << '\n';
  rule(o);
  o << "SYMBOL STRATA  (public removals / signature changes joined against the OLD release's "
       "headers)\n";
  rule(o);
  o << std::format(
    "  removed:   declared {:>6}  undeclared {:>6}  undecidable {:>6}\n",
    sy.at("removed_declared").get<int>(), sy.at("removed_undeclared").get<int>(),
    sy.at("removed_unknown").get<int>()
  );
  o << std::format(
    "  re-signed: declared {:>6}  undeclared {:>6}  undecidable {:>6}\n",
    sy.at("signature_declared").get<int>(), sy.at("signature_undeclared").get<int>(),
    sy.at("signature_unknown").get<int>()
  );
  o << std::format(
    "  layout events excluded by the lenient rule (append-only growth of pointer-only types): {}\n",
    sy.at("layout_events_excluded").get<int>()
  );

  const auto &h = s.at("headers");
  o << '\n';
  rule(o);
  o << "INLINE / TEMPLATE BODY AND MACRO CHURN  (from -dev headers via libclang; invisible to ABI "
       "tools; token-level = upper bound)\n";
  rule(o);
  const double wd = h.at("with_data").get<double>();
  auto hp = [&](const char *k) {
    return std::format(
      "{:>6} {:5.1f}%", h.at(k).get<std::uint64_t>(),
      wd == 0 ? 0.0 : 100.0 * h.at(k).get<double>() / wd
    );
  };
  o << std::format(
    "  transitions with header data                    : {}\n", h.at("with_data").get<int>()
  );
  o << std::format(
    "  ...shipping >=1 inlinable definition            : {}\n", hp("with_definitions")
  );
  o << std::format(
    "  changing >=1 inline/template body               : {}   ({} bodies)\n", hp("body_changed"),
    h.at("body_events").get<int>()
  );
  o << std::format(
    "  changing >=1 public macro value                 : {}   ({} macros)\n", hp("macro_changed"),
    h.at("macro_events").get<int>()
  );
  o << std::format(
    "  ...excluding version/build stamps               : {}\n", hp("macro_changed_nonversion")
  );
  o << std::format("  POOR parse coverage (counts are lower bounds)   : {}\n", hp("poor_coverage"));
  o << "  most changed bodies in one release:\n";
  for (const auto &t : h.at("top")) {
    o << std::format(
      "    {:<48} {:>5}\n", t.at("id").get<std::string>(), t.at("bodies").get<int>()
    );
  }

  o << '\n';
  rule(o);
  o << "SENSITIVITY: language threshold (fraction of Itanium-mangled exported functions => C++)\n";
  rule(o);
  for (const auto &[th, v] : s.at("sensitivity").at("language_threshold").items()) {
    o << std::format(
      "  >= {}: C {:>4}   C++ {:>4}\n", th, v.at("c").get<int>(), v.at("cxx").get<int>()
    );
  }

  o << '\n';
  rule(o);
  o << "PER LIBRARY  (binary-breaking transitions strict / lenient over transitions)\n";
  rule(o);
  std::vector<Json> libs(s.at("libraries").begin(), s.at("libraries").end());
  std::ranges::sort(libs, [](const Json &a, const Json &b) {
    const auto ka = std::pair{a.at("breaks_strict").get<int>(), a.at("transitions").get<int>()};
    const auto kb = std::pair{b.at("breaks_strict").get<int>(), b.at("transitions").get<int>()};
    return ka > kb;
  });
  for (const auto &l : libs) {
    o << std::format(
      "  {:<28}{:<4}{:>3} / {:>3} / {:<3}\n", l.at("source").get<std::string>(),
      l.at("language").get<std::string>(), l.at("breaks_strict").get<int>(),
      l.at("breaks_lenient").get<int>(), l.at("transitions").get<int>()
    );
  }
  return o.str();
}

} // namespace abistudy
