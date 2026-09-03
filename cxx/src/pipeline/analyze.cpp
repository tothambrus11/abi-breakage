#include <algorithm>
#include <format>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

#include "abi/model.hpp"
#include "pipeline/stages.hpp"

namespace abistudy::pipeline {
namespace {

/// @brief One release transition after rolling its shared objects together.
struct Transition {
  std::string id;
  SourceName source;
  Language language;
  ChangeCounts counts; ///< Public events, header kinds merged in.
  ChangeCounts third_party;
  ChangeCounts private_node;
  bool soname_changed = false;
  bool mass_rename = false;
  bool debug_info_missing = false;
  std::optional<hdr::HeaderDiff> headers;
  bool header_coverage_poor = false; ///< >50% of TUs hit a fatal diagnostic on either side.
};

Transition rollup(const PairResult &p, const HeaderResult *h) {
  Transition t{
    .id = p.id,
    .source = p.source,
    .language = Language::unknown,
    .counts = {},
    .third_party = {},
    .private_node = {},
    .soname_changed = false,
    .mass_rename = false,
    .debug_info_missing = false,
    .headers = std::nullopt,
    .header_coverage_poor = false
  };
  for (const auto &o : p.objects) {
    t.counts.merge(o.public_counts);
    t.third_party.merge(o.third_party_counts);
    t.private_node.merge(o.private_node_counts);
    t.soname_changed |= o.soname_1 != o.soname_2;
    t.mass_rename |= o.mass_rename;
    t.debug_info_missing |= !o.coverage.debug_info_found_1 || !o.coverage.debug_info_found_2;
    if (o.language == Language::cxx) {
      t.language = Language::cxx;
    } else if (t.language == Language::unknown) {
      t.language = o.language;
    }
  }
  if (h && h->diff) {
    t.headers = h->diff;
    t.counts.add(ChangeKind::inline_body_changed, h->diff->inline_body_changed);
    t.counts.add(ChangeKind::macro_value_changed, h->diff->macro_value_changed_nonversion);
    auto poor = [](const hdr::ParseCoverage &c) {
      return c.parsed != 0 && std::max(c.with_errors, c.with_fatal_error) * 2 > c.parsed;
    };
    t.header_coverage_poor = poor(h->coverage_1) || poor(h->coverage_2);
  }
  return t;
}

template <class Pred>
std::vector<const Transition *> where(const std::vector<Transition> &ts, Pred p) {
  std::vector<const Transition *> out;
  for (const auto &t : ts) {
    if (p(t))
      out.push_back(&t);
  }
  return out;
}

template <class Pred>
std::vector<const Transition *> where_ptr(const std::vector<const Transition *> &ts, Pred p) {
  std::vector<const Transition *> out;
  for (const auto *t : ts) {
    if (p(*t))
      out.push_back(t);
  }
  return out;
}

bool has_kind_where(const Transition &t, bool (*pred)(ChangeKind) noexcept) {
  return std::ranges::any_of(t.counts.items(), [&](const auto &kv) { return pred(kv.first); });
}

std::set<ChangeKind> kinds_where(const Transition &t, bool (*pred)(ChangeKind) noexcept) {
  std::set<ChangeKind> s;
  for (const auto &[k, n] : t.counts.items()) {
    if (pred(k))
      s.insert(k);
  }
  return s;
}

struct KindStat {
  std::uint32_t transitions = 0;
  std::uint64_t events = 0;
  std::vector<std::uint32_t> per;
};

std::map<ChangeKind, KindStat> kind_stats(const std::vector<const Transition *> &ts) {
  std::map<ChangeKind, KindStat> m;
  for (const auto *t : ts) {
    for (const auto &[k, n] : t->counts.items()) {
      auto &s = m[k];
      ++s.transitions;
      s.events += n;
      s.per.push_back(n);
    }
  }
  return m;
}

double median(std::vector<std::uint32_t> v) {
  if (v.empty())
    return 0;
  std::ranges::sort(v);
  const auto n = v.size();
  return n % 2 ? v[n / 2] : (v[(n / 2) - 1] + v[n / 2]) / 2.0;
}

std::string pct(std::size_t num, std::size_t den) {
  return den == 0
           ? "  n/a"
           : std::format("{:5.1f}%", 100.0 * static_cast<double>(num) / static_cast<double>(den));
}

void frequency_table(
  std::ostringstream &o, Json &j, std::string_view title, const std::vector<const Transition *> &ts
) {
  std::set<SourceName> libs;
  for (const auto *t : ts)
    libs.insert(t->source);
  o << std::format(
    "\n{:=<118}\n{}\n  n = {} release transitions   ({} libraries)\n{:=<118}\n", "", title,
    ts.size(), libs.size(), ""
  );
  o << std::format(
    "{:<28}{:>12}{:>9}{:>14}{:>8}{:>7}   {:<12}{}\n", "change kind", "transitions", "% of n",
    "TOTAL events", "median", "max", "resilience", "mechanism"
  );
  o << std::format("{:-<118}\n", "");
  const auto stats = kind_stats(ts);
  Json rows = Json::array();
  for (const auto k : all_change_kinds) {
    const auto it = stats.find(k);
    if (it == stats.end())
      continue;
    const auto &s = it->second;
    const auto mx = *std::ranges::max_element(s.per);
    const auto mech = mechanism_for(k);
    const char *verdict = "n/a";
    if (is_absorbed(k)) {
      verdict = "ABSORBS";
    } else if (mech == Mechanism::none) {
      verdict = "cannot help";
    }
    o << std::format(
      "{:<28}{:>12}{:>9}{:>14}{:>8.0f}{:>7}   {:<12}{}\n", to_string(k), s.transitions,
      pct(s.transitions, ts.size()), s.events, median(s.per), mx, verdict, to_string(mech)
    );
    rows.push_back(
      {{"kind", to_string(k)},
       {"transitions", s.transitions},
       {"events", s.events},
       {"median", median(s.per)},
       {"max", mx},
       {"mechanism", to_string(mech)}}
    );
  }
  j = {{"title", title}, {"n", ts.size()}, {"libraries", libs.size()}, {"rows", rows}};
}

void rescue_table(
  std::ostringstream &o, Json &j, std::string_view title, const std::vector<const Transition *> &ts,
  bool (*relevant)(ChangeKind) noexcept
) {
  const auto affected =
    where_ptr(ts, [&](const Transition &t) { return has_kind_where(t, relevant); });
  std::vector<const Transition *> fully;
  for (const auto *t : affected) {
    const auto ks = kinds_where(*t, relevant);
    if (std::ranges::all_of(ks, [](ChangeKind k) { return is_absorbed(k); }))
      fully.push_back(t);
  }
  o << std::format("\n{:=<118}\n{}\n{:=<118}\n", "", title, "");
  o << std::format(
    "affected transitions                        : {} ({} of {})\n", affected.size(),
    pct(affected.size(), ts.size()), ts.size()
  );
  o << std::format(
    "  every relevant change absorbable          : {} ({})\n", fully.size(),
    pct(fully.size(), affected.size())
  );
  o << std::format(
    "  contained an unabsorbable change          : {} ({})\n", affected.size() - fully.size(),
    pct(affected.size() - fully.size(), affected.size())
  );
  o << "\nLOAD-BEARING MECHANISMS  (of the fully-rescued transitions, how many needed each)\n";
  o << std::format("{:-<118}\n", "");
  Json mechs = Json::array();
  for (const auto m :
       {Mechanism::opaque_layout, Mechanism::non_frozen_enum, Mechanism::resilient_dispatch,
        Mechanism::no_implicit_inlining}) {
    std::uint32_t needed = 0;
    std::uint32_t sole = 0;
    for (const auto *t : fully) {
      const auto ks = kinds_where(*t, relevant);
      const bool uses =
        std::ranges::any_of(ks, [&](ChangeKind k) { return mechanism_for(k) == m; });
      const bool only =
        uses && std::ranges::all_of(ks, [&](ChangeKind k) { return mechanism_for(k) == m; });
      needed += static_cast<std::uint32_t>(uses);
      sole += static_cast<std::uint32_t>(only);
    }
    o << std::format(
      "  {:<38} needed by {:>4} / {}   (sole reason for {})\n", to_string(m), needed, fully.size(),
      sole
    );
    mechs.push_back({{"mechanism", to_string(m)}, {"needed_by", needed}, {"sole_reason", sole}});
  }
  j = {
    {"title", title},
    {"affected", affected.size()},
    {"fully_rescued", fully.size()},
    {"mechanisms", mechs}
  };
}

} // namespace

Result<std::string> run_analyze(const Workspace &ws, const Log &log) {
  ABISTUDY_TRY_VOID(ws.ensure());
  std::map<std::string, HeaderResult> headers;
  std::error_code ec;
  for (const auto &e : std::filesystem::directory_iterator(ws.header_pairs(), ec)) {
    if (e.path().extension() != ".json")
      continue;
    if (auto h = load_artifact_as<HeaderResult>(e.path(), schema_header_pair))
      headers.emplace(h->id, std::move(*h));
  }
  std::vector<Transition> all;
  std::uint32_t errored = 0;
  std::uint32_t objects = 0;
  for (const auto &e : std::filesystem::directory_iterator(ws.pairs(), ec)) {
    if (e.path().extension() != ".json")
      continue;
    auto p = load_artifact_as<PairResult>(e.path(), schema_pair);
    if (!p) {
      log(std::format("skip {}: {}", e.path().filename().string(), p.error().message));
      continue;
    }
    if (p->error || p->objects.empty()) {
      ++errored;
      continue;
    }
    objects += static_cast<std::uint32_t>(p->objects.size());
    const auto h = headers.find(p->id);
    all.push_back(rollup(*p, h == headers.end() ? nullptr : &h->second));
  }
  std::ranges::sort(all, {}, &Transition::id);

  std::ostringstream o;
  Json summary;
  const auto mass = where(all, [](const Transition &t) { return t.mass_rename; });
  const auto good = where(all, [](const Transition &t) { return !t.mass_rename; });
  const auto c_only =
    where_ptr(good, [](const Transition &t) { return t.language == Language::c; });
  const auto cxx_only =
    where_ptr(good, [](const Transition &t) { return t.language == Language::cxx; });
  std::set<SourceName> libs;
  std::set<SourceName> libs_c;
  std::set<SourceName> libs_x;
  std::set<SourceName> mass_libs;
  for (const auto *t : good)
    libs.insert(t->source);
  for (const auto *t : c_only)
    libs_c.insert(t->source);
  for (const auto *t : cxx_only)
    libs_x.insert(t->source);
  for (const auto *t : mass)
    mass_libs.insert(t->source);

  o << std::format("{:=<118}\nCORPUS\n{:=<118}\n", "", "");
  o << std::format("release transitions analysed          : {}\n", good.size());
  o << std::format("  libraries (Debian source packages)  : {}\n", libs.size());
  o << std::format("  shared objects compared             : {}\n", objects);
  o << std::format(
    "  C transitions                       : {}  ({} libraries)\n", c_only.size(), libs_c.size()
  );
  o << std::format(
    "  C++ transitions                     : {}  ({} libraries)\n", cxx_only.size(), libs_x.size()
  );
  o << std::format(
    "  excluded: mass symbol rename policy : {}  {}\n", mass.size(),
    Json(std::vector<SourceName>(mass_libs.begin(), mass_libs.end())).dump()
  );
  o << std::format("  excluded: pair failed / no objects  : {}\n", errored);
  const auto nodbg = where_ptr(good, [](const Transition &t) { return t.debug_info_missing; });
  o << std::format(
    "  debug info missing on a side        : {}  (symbol-only comparison for those)\n", nodbg.size()
  );
  summary["corpus"] = {
    {"transitions", good.size()},
    {"libraries", libs.size()},
    {"objects", objects},
    {"c_transitions", c_only.size()},
    {"cxx_transitions", cxx_only.size()},
    {"mass_rename_excluded", mass.size()},
    {"errored", errored},
    {"debug_info_missing", nodbg.size()}
  };

  ChangeCounts tp;
  ChangeCounts pv;
  for (const auto &t : all) {
    tp.merge(t.third_party);
    pv.merge(t.private_node);
  }
  auto total = [](const ChangeCounts &c) {
    std::uint64_t n = 0;
    for (const auto &[k, v] : c.items())
      n += v;
    return n;
  };
  o << "\nEVENTS EXCLUDED BY THE TWO ATTRIBUTION FILTERS\n";
  o << std::format(
    "  types not declared in the public headers : {} events {}\n", total(tp), Json(tp).dump()
  );
  o << std::format(
    "  symbols in private ELF version nodes  : {} events {}\n", total(pv), Json(pv).dump()
  );
  summary["filtered"] = {{"third_party", tp}, {"private_node", pv}};

  frequency_table(
    o, summary["frequency_all"],
    "FREQUENCY OF CHANGE KINDS ACROSS CONSECUTIVE RELEASES  (all libraries)", good
  );
  frequency_table(o, summary["frequency_c"], "C LIBRARIES ONLY", c_only);
  frequency_table(o, summary["frequency_cxx"], "C++ LIBRARIES ONLY", cxx_only);

  const auto broke =
    where_ptr(good, [](const Transition &t) { return has_kind_where(t, is_binary_breaking); });
  const auto declared = where_ptr(broke, [](const Transition &t) { return t.soname_changed; });
  o << std::format("\n{:=<118}\nDECLARED vs SILENT BINARY BREAKS\n{:=<118}\n", "", "");
  o << std::format("transitions analysed                        : {}\n", good.size());
  o << std::format(
    "transitions with a binary-breaking change   : {} ({})\n", broke.size(),
    pct(broke.size(), good.size())
  );
  o << std::format("  SONAME bumped   (break declared)          : {}\n", declared.size());
  o << std::format(
    "  SONAME unchanged (SILENT break)           : {} ({} of breaks)\n",
    broke.size() - declared.size(), pct(broke.size() - declared.size(), broke.size())
  );
  summary["breaks"] = {
    {"breaking", broke.size()},
    {"declared", declared.size()},
    {"silent", broke.size() - declared.size()}
  };

  rescue_table(
    o, summary["rescue_binary"],
    "WOULD A RESILIENT BOUNDARY HAVE SAVED THE RELEASE?  (binary breakage only)", good,
    is_binary_breaking
  );
  rescue_table(
    o, summary["rescue_evolution"],
    "SECOND FRAMING: RECOMPILATION-FREE EVOLUTION  (adds enum cases)", good, is_evolution_relevant
  );
  rescue_table(
    o, summary["rescue_inline"],
    "THIRD FRAMING: ...PLUS CHANGED INLINE/TEMPLATE BODIES  (stale copies in clients)", good,
    is_evolution_or_inline
  );

  // Header churn.
  const auto withh = where_ptr(good, [](const Transition &t) { return t.headers.has_value(); });
  const auto withdefs =
    where_ptr(withh, [](const Transition &t) { return t.headers.value().definitions_common > 0; });
  const auto bodies =
    where_ptr(withh, [](const Transition &t) { return t.headers.value().inline_body_changed > 0; });
  const auto macros =
    where_ptr(withh, [](const Transition &t) { return t.headers.value().macro_value_changed > 0; });
  const auto macros_nv = where_ptr(withh, [](const Transition &t) {
    return t.headers.value().macro_value_changed_nonversion > 0;
  });
  const auto poor = where_ptr(withh, [](const Transition &t) { return t.header_coverage_poor; });
  std::uint64_t tb = 0;
  std::uint64_t tm = 0;
  std::vector<std::uint32_t> perb;
  for (const auto *t : bodies) {
    if (const auto &h = t->headers; h) {
      tb += h->inline_body_changed;
      perb.push_back(h->inline_body_changed);
    }
  }
  for (const auto *t : macros) {
    if (const auto &h = t->headers; h) {
      tm += h->macro_value_changed;
    }
  }
  o << std::format(
    "\n{:=<118}\nINLINE / TEMPLATE BODY AND MACRO CHURN  (from -dev headers via libclang; "
    "invisible to ABI tools)\n{:=<118}\n",
    "", ""
  );
  o << std::format(
    "{:<58}{:>12}{:>9}{:>14}\n{:-<118}\n", "measure", "transitions", "% of n", "TOTAL events", ""
  );
  o << std::format("{:<58}{:>12}\n", "transitions with header data", withh.size());
  o << std::format(
    "{:<58}{:>12}{:>9}\n", "  ...shipping >=1 inlinable definition in public headers",
    withdefs.size(), pct(withdefs.size(), withh.size())
  );
  o << std::format(
    "{:<58}{:>12}{:>9}{:>14}\n", "transitions changing >=1 inline/template body", bodies.size(),
    pct(bodies.size(), withh.size()), tb
  );
  o << std::format(
    "{:<58}{:>12}{:>9}{:>14}\n", "transitions changing >=1 public macro value", macros.size(),
    pct(macros.size(), withh.size()), tm
  );
  o << std::format(
    "{:<58}{:>12}{:>9}\n", "  ...excluding version/build stamp macros", macros_nv.size(),
    pct(macros_nv.size(), withh.size())
  );
  o << std::format(
    "{:<58}{:>12.0f}{:>9}\n", "  bodies changed per affected transition (median / max)",
    median(perb), perb.empty() ? 0U : *std::ranges::max_element(perb)
  );
  o << std::format(
    "{:<58}{:>12}{:>9}   (>50% of TUs hit a fatal diagnostic; counts there are lower bounds)\n",
    "transitions with POOR header parse coverage", poor.size(), pct(poor.size(), withh.size())
  );
  summary["headers"] = {
    {"with_data", withh.size()},
    {"with_definitions", withdefs.size()},
    {"body_changed", bodies.size()},
    {"body_events", tb},
    {"macro_changed", macros.size()},
    {"macro_events", tm},
    {"macro_changed_nonversion", macros_nv.size()},
    {"poor_coverage", poor.size()}
  };
  auto top = bodies;
  std::ranges::sort(top, std::greater<>{}, [](const Transition *t) {
    return t->headers.value_or(hdr::HeaderDiff{}).inline_body_changed;
  });
  o << "\nmost changed client-visible bodies in one release:\n";
  for (std::size_t i = 0; i < std::min<std::size_t>(10, top.size()); ++i) {
    const auto h = top[i]->headers.value_or(hdr::HeaderDiff{});
    o << std::format(
      "  {:<44} {:>5} bodies (of {} shared)\n", top[i]->id, h.inline_body_changed,
      h.definitions_common
    );
  }

  // Least stable libraries.
  std::map<SourceName, std::pair<std::uint32_t, std::uint32_t>> bysrc;
  for (const auto *t : good) {
    auto &e = bysrc[t->source];
    ++e.second;
    e.first += static_cast<unsigned int>(has_kind_where(*t, is_binary_breaking));
  }
  std::vector<std::pair<SourceName, std::pair<std::uint32_t, std::uint32_t>>> rows(
    bysrc.begin(), bysrc.end()
  );
  std::ranges::sort(rows, [](const auto &a, const auto &b) {
    return a.second.first != b.second.first ? a.second.first > b.second.first
                                            : a.second.second > b.second.second;
  });
  o << std::format(
    "\n{:=<118}\nLEAST ABI-STABLE LIBRARIES  (binary-breaking transitions / "
    "transitions)\n{:=<118}\n",
    "", ""
  );
  for (std::size_t i = 0;
       i < std::min<std::size_t>(20, rows.size()) && (rows[i].second.first != 0U); ++i) {
    o << std::format(
      "  {:<26}{:>4} / {}\n", rows[i].first, rows[i].second.first, rows[i].second.second
    );
  }

  Json per = Json::array();
  for (const auto &t : all) {
    per.push_back(
      {{"id", t.id},
       {"source", t.source},
       {"language", to_string(t.language)},
       {"counts", t.counts},
       {"soname_changed", t.soname_changed},
       {"mass_rename", t.mass_rename},
       {"debug_info_missing", t.debug_info_missing},
       {"header_coverage_poor", t.header_coverage_poor},
       {"header_defs_common", t.headers ? t.headers.value().definitions_common : 0}}
    );
  }
  summary["transitions"] = per;

  std::string text = o.str();
  ABISTUDY_TRY_VOID(save_artifact(ws.summary(), schema_summary, summary));
  ABISTUDY_TRY_VOID(fs::write_file_atomic(ws.report(), text));
  log(std::format("wrote {} and {}", ws.summary().string(), ws.report().string()));
  return text;
}

} // namespace abistudy::pipeline
