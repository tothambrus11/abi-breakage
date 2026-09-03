// =============================================================================
// report stage: renders report.html from summary.json. The HTML template is
// embedded in the binary with C++26 `#embed`, so the tool is self-contained.
// Numbers are never typed into the page by hand; every figure is derived here.
// =============================================================================

#include <algorithm>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "abi/model.hpp"
#include "pipeline/stages.hpp"

namespace abistudy::pipeline {
namespace {

// NOLINTNEXTLINE(*-avoid-c-arrays): `#embed` initialises a C array by design
constexpr unsigned char template_bytes[] = {
#embed "template.html"
};

/// @brief The embedded template as text.
/// @post  Non-empty; stable for the life of the process.
std::string_view template_html() {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes -> chars of the same object
  return {reinterpret_cast<const char *>(template_bytes), sizeof template_bytes};
}

/// @brief Replaces every `{{key}}` in `tmpl` with values[key]; unknown keys
///        are left visible so a missing figure is noticed, not hidden.
std::string render(std::string_view tmpl, const std::map<std::string, std::string> &values) {
  std::string out;
  out.reserve(tmpl.size() + 4096);
  std::size_t i = 0;
  while (i < tmpl.size()) {
    const auto open = tmpl.find("{{", i);
    if (open == std::string_view::npos) {
      out.append(tmpl.substr(i));
      break;
    }
    out.append(tmpl.substr(i, open - i));
    const auto close = tmpl.find("}}", open);
    if (close == std::string_view::npos) {
      out.append(tmpl.substr(open));
      break;
    }
    const std::string key{tmpl.substr(open + 2, close - open - 2)};
    const auto it = values.find(key);
    out.append(it == values.end() ? "{{" + key + "}}" : it->second);
    i = close + 2;
  }
  return out;
}

std::string escape(std::string_view s) {
  std::string o;
  for (const char c : s) {
    switch (c) {
    case '&':
      o += "&amp;";
      break;
    case '<':
      o += "&lt;";
      break;
    case '>':
      o += "&gt;";
      break;
    default:
      o.push_back(c);
    }
  }
  return o;
}

std::string pct(double num, double den) {
  return den == 0 ? "n/a" : std::format("{:.1f}%", 100.0 * num / den);
}

std::string with_commas(std::uint64_t n) {
  std::string s = std::to_string(n);
  std::string out;
  int k = 0;
  for (auto it = s.rbegin(); it != s.rend(); ++it, ++k) {
    if (k && k % 3 == 0)
      out.push_back(',');
    out.push_back(*it);
  }
  return {out.rbegin(), out.rend()};
}

std::string chip_for(ChangeKind k) {
  switch (mechanism_for(k)) {
  case Mechanism::no_implicit_inlining:
    return R"html(<span class="chip hid">invisible</span>)html";
  case Mechanism::none:
    return R"html(<span class="chip no">cannot help</span>)html";
  case Mechanism::not_applicable:
    return R"html(<span class="chip na">additive</span>)html";
  default:
    return R"html(<span class="chip go">absorbs</span>)html";
  }
}

const char *group_of(ChangeKind k) {
  switch (mechanism_for(k)) {
  case Mechanism::opaque_layout:
    return "Layout";
  case Mechanism::non_frozen_enum:
    return "Enums";
  case Mechanism::resilient_dispatch:
    return "Dispatch";
  case Mechanism::no_implicit_inlining:
    return "Header bodies (libclang)";
  default:
    return "Symbols";
  }
}

/// @brief Frequency table rows for one population, in display order:
///        symbols first (as the reader's anchor), then by mechanism group.
std::string frequency_rows(const Json &freq) {
  struct Row {
    ChangeKind kind;
    std::uint64_t transitions, events, max;
    double median;
  };
  std::vector<Row> rows;
  const auto n = freq.at("n").get<std::uint64_t>();
  for (const auto &r : freq.at("rows")) {
    if (const auto k = parse_change_kind(r.at("kind").get<std::string>()))
      rows.push_back({*k, r.at("transitions"), r.at("events"), r.at("max"), r.at("median")});
  }
  const std::vector<const char *> order{
    "Symbols", "Enums", "Layout", "Dispatch", "Header bodies (libclang)"
  };
  std::uint64_t maxt = 1;
  for (const auto &r : rows)
    maxt = std::max(maxt, r.transitions);
  std::string html;
  for (const auto *g : order) {
    std::vector<Row> in;
    for (const auto &r : rows) {
      if (std::string_view{group_of(r.kind)} == g)
        in.push_back(r);
    }
    if (in.empty())
      continue;
    std::ranges::sort(in, std::greater<>{}, &Row::transitions);
    html += std::format(R"html(<tr class="group"><td colspan="8">{}</td></tr>)html", g);
    for (const auto &r : in) {
      const bool hi = is_absorbed(r.kind);
      html += std::format(
        R"html(<tr><td class="kind">{}</td><td class="barcell"><span class="bar{}" style="width:{:.1f}%"></span></td>)html"
        R"html(<td class="num big">{}</td><td class="num">{}</td><td class="num">{}</td><td class="num">{:.0f}</td><td class="num">{}</td><td>{}</td></tr>)html",
        to_string(r.kind), hi ? " hi" : "", 100.0 * double(r.transitions) / double(maxt),
        r.transitions, pct(double(r.transitions), double(n)), with_commas(r.events), r.median,
        with_commas(r.max), chip_for(r.kind)
      );
    }
  }
  return html;
}

struct Mech {
  std::uint64_t needed = 0, sole = 0;
};
std::map<std::string, Mech> mechanisms(const Json &rescue) {
  std::map<std::string, Mech> m;
  for (const auto &x : rescue.at("mechanisms")) {
    m[x.at("mechanism").get<std::string>()] = {
      .needed = x.at("needed_by"), .sole = x.at("sole_reason")
    };
  }
  return m;
}

std::string rescue_row(std::string_view name, const Json &rescue) {
  const auto affected = rescue.at("affected").get<std::uint64_t>();
  const auto fully = rescue.at("fully_rescued").get<std::uint64_t>();
  const auto m = mechanisms(rescue);
  auto cell = [&](const char *k) {
    const auto it = m.find(k);
    if (it == m.end() || it->second.needed == 0)
      return std::string{R"html(<td class="num">—</td>)html"};
    return std::format(
      R"html(<td class="num">{} <span style="color:var(--slate)">(sole {})</span></td>)html",
      it->second.needed, it->second.sole
    );
  };
  return std::format(
    R"html(<tr><td>{}</td><td class="num">{}</td><td class="num big">{} <span style="color:var(--slate)">· {}</span></td>{}{}{}{}</tr>)html",
    name, affected, fully, pct(double(fully), double(affected)), cell("opaque layout"),
    cell("non-frozen enum"), cell("resilient dispatch"), cell("no implicit cross-module inlining")
  );
}

std::string verdict_row(
  std::string_view title, std::string_view body, std::string_view cls, std::string_view verdict
) {
  return std::format(
    R"html(<div class="ledger-row"><div class="k">{}<small>{}</small></div><div class="v {}">{}</div></div>)html",
    title, body, cls, verdict
  );
}

} // namespace

Result<std::string> run_report(const Workspace &ws, const Log &log) {
  ABISTUDY_TRY(Json s, load_artifact(ws.summary(), schema_summary));
  const auto &corpus = s.at("corpus");
  const auto transitions = corpus.at("transitions").get<std::uint64_t>();
  const auto freq_all = s.at("frequency_all");
  const auto rb = s.at("rescue_binary");
  const auto re = s.at("rescue_evolution");
  const auto ri = s.at("rescue_inline");
  const auto hdr = s.at("headers");
  const auto breaks = s.at("breaks");

  // Per-kind lookups from the all-libraries table.
  std::map<ChangeKind, std::pair<std::uint64_t, std::uint64_t>> kind_stats; // transitions, events
  for (const auto &r : freq_all.at("rows")) {
    if (const auto k = parse_change_kind(r.at("kind").get<std::string>()))
      kind_stats[*k] = {r.at("transitions"), r.at("events")};
  }
  auto t_of = [&](ChangeKind k) { return kind_stats.contains(k) ? kind_stats[k].first : 0; };
  auto share = [&](ChangeKind k) { return pct(double(t_of(k)), double(transitions)); };
  const auto me = mechanisms(re);
  const auto mi = mechanisms(ri);
  const auto fully_e = re.at("fully_rescued").get<std::uint64_t>();
  const auto affected_e = re.at("affected").get<std::uint64_t>();
  const auto fully_b = rb.at("fully_rescued").get<std::uint64_t>();
  const auto affected_b = rb.at("affected").get<std::uint64_t>();

  auto sum = [&](std::initializer_list<ChangeKind> ks) {
    std::uint64_t n = 0;
    for (const auto k : ks)
      n += t_of(k);
    return n;
  };
  const auto layout_t = sum(
    {ChangeKind::field_added_to_struct, ChangeKind::field_removed_from_struct,
     ChangeKind::field_type_changed, ChangeKind::member_offset_changed,
     ChangeKind::type_size_changed, ChangeKind::base_class_changed}
  );

  auto mech = [&](const char *k) { return me.contains(k) ? me.at(k) : Mech{}; };
  auto verdict_for = [&](const Mech &m, double min_share) {
    if (fully_e == 0)
      return std::pair{"", "No data"};
    const double share_of_rescues = double(m.needed) / double(fully_e);
    if (share_of_rescues >= min_share)
      return std::pair{"go", "Worth it"};
    return std::pair{"", "Marginal"};
  };
  const auto m_layout = mech("opaque layout");
  const auto m_enum = mech("non-frozen enum");
  const auto m_disp = mech("resilient dispatch");
  const auto m_inl = mi.contains("no implicit cross-module inlining")
                       ? mi.at("no implicit cross-module inlining")
                       : Mech{};
  const auto fully_i = ri.at("fully_rescued").get<std::uint64_t>();

  std::string verdicts;
  {
    auto [c, v] = verdict_for(m_layout, 0.15);
    verdicts += verdict_row(
      "Opaque struct layout",
      std::format(
        "Field added, removed, retyped or shifted; type size changed. Present in {} transitions; "
        "load-bearing for {} of the {} releases a resilient boundary would fully rescue, the sole "
        "reason for {}.",
        layout_t, m_layout.needed, fully_e, m_layout.sole
      ),
      c, v
    );
  }
  {
    auto [c, v] = verdict_for(m_enum, 0.15);
    verdicts += verdict_row(
      "Non-frozen enums",
      std::format(
        "An added enum case moves no byte, so it is not a binary break — but it breaks any client "
        "that switched exhaustively. Enum cases changed in {} transitions ({}); load-bearing for "
        "{} "
        "of {} rescues, sole reason for {}.",
        t_of(ChangeKind::enum_case_added) + t_of(ChangeKind::enum_case_removed),
        share(ChangeKind::enum_case_added), m_enum.needed, fully_e, m_enum.sole
      ),
      c, v
    );
  }
  {
    const char *v =
      t_of(ChangeKind::inline_body_changed) * 20 >= transitions ? "Worth it" : "Marginal";
    verdicts += verdict_row(
      "No implicit cross-module inlining",
      std::format(
        "Inline, in-class and template bodies changed in <b>{}</b> of transitions ({} events); "
        "non-stamp macro values in {}. C and C++ have no defence, and no ABI tool can detect it. "
        "Counted as a hazard, it is load-bearing for {} of {} rescued releases, the sole reason "
        "for {}.",
        share(ChangeKind::inline_body_changed),
        with_commas(kind_stats[ChangeKind::inline_body_changed].second),
        share(ChangeKind::macro_value_changed), m_inl.needed, fully_i, m_inl.sole
      ),
      "hid", v
    );
  }
  {
    auto [c, v] = verdict_for(m_disp, 0.15);
    verdicts += verdict_row(
      "Resilient vtable dispatch",
      std::format(
        "Vtables changed in {} of {} transitions ({}); load-bearing for {} rescues, sole reason "
        "for "
        "{}. Real, but the thinnest return on runtime budget of the four.",
        t_of(ChangeKind::vtable_changed), transitions, share(ChangeKind::vtable_changed),
        m_disp.needed, m_disp.sole
      ),
      c, v
    );
  }
  verdicts += verdict_row(
    "Removed symbols &amp; changed signatures",
    std::format(
      "Present in {} of {} binary-breaking transitions ({}). No indirection helps: a call that no "
      "longer type-checks is an API break, not an ABI one. This is the ceiling on what any "
      "resilience "
      "mechanism can deliver.",
      affected_b - fully_b, affected_b, pct(double(affected_b - fully_b), double(affected_b))
    ),
    "no", "Cannot help"
  );

  // Least stable libraries and header churn leaders from the per-transition list.
  std::map<std::string, std::pair<int, int>> bysrc;
  std::vector<std::pair<std::string, std::uint64_t>> churn;
  for (const auto &t : s.at("transitions")) {
    if (t.value("mass_rename", false))
      continue;
    const auto counts = t.at("counts").get<ChangeCounts>();
    auto &e = bysrc[t.at("source").get<std::string>()];
    ++e.second;
    for (const auto &[k, n] : counts.items()) {
      if (is_binary_breaking(k)) {
        ++e.first;
        break;
      }
    }
    if (const auto b = counts.get(ChangeKind::inline_body_changed))
      churn.emplace_back(t.at("id").get<std::string>(), b);
  }
  std::vector<std::pair<std::string, std::pair<int, int>>> unstable(bysrc.begin(), bysrc.end());
  std::ranges::sort(unstable, [](const auto &a, const auto &b) {
    return a.second.first != b.second.first ? a.second.first > b.second.first
                                            : a.second.second > b.second.second;
  });
  std::string unstable_list;
  for (std::size_t i = 0; i < std::min<std::size_t>(8, unstable.size()); ++i) {
    if (!unstable[i].second.first)
      break;
    unstable_list += std::format(
      "{}{} {}/{}", i ? ", " : "", escape(unstable[i].first), unstable[i].second.first,
      unstable[i].second.second
    );
  }
  std::ranges::sort(churn, std::greater<>{}, [](const auto &p) { return p.second; });
  std::string churn_text;
  for (std::size_t i = 0; i < std::min<std::size_t>(3, churn.size()); ++i) {
    churn_text +=
      std::format("{}{} changed {} bodies", i ? "; " : "", escape(churn[i].first), churn[i].second);
  }

  const auto hdr_body_t = t_of(ChangeKind::inline_body_changed);
  const auto fa_t = t_of(ChangeKind::field_added_to_struct);
  const auto vt_t = t_of(ChangeKind::vtable_changed);
  std::string hdr_paragraph = std::format(
    "At {}, inline-body churn is {} as frequent as struct-field additions ({}) and {} as frequent "
    "as "
    "vtable changes ({}).{} This is the strongest argument in the data for making cross-module "
    "inlining opt-in rather than automatic.",
    share(ChangeKind::inline_body_changed),
    fa_t ? std::format("{:.0f}×", double(hdr_body_t) / double(fa_t)) : "incomparably more",
    share(ChangeKind::field_added_to_struct),
    vt_t ? std::format("{:.0f}×", double(hdr_body_t) / double(vt_t)) : "incomparably more",
    share(ChangeKind::vtable_changed),
    churn_text.empty() ? "" : " Worst single releases: " + churn_text + "."
  );

  std::string rescue_paragraph = std::format(
    "{} of {} affected releases are fully rescued ({}); opaque layout is load-bearing for {}, "
    "non-frozen enums for {}, resilient dispatch for {}. Counting changed inline bodies as a "
    "hazard too, opt-in inlining becomes load-bearing for {} of {} rescues.",
    fully_e, affected_e, pct(double(fully_e), double(affected_e)), m_layout.needed, m_enum.needed,
    m_disp.needed, m_inl.needed, fully_i
  );
  std::string ceiling_paragraph = std::format(
    "The ceiling is the real finding: even under the generous framing, <b>{}</b> of affected "
    "releases contain a removed symbol or a changed signature. Resilience buys evolution headroom, "
    "not immunity — it converts a class of breaks that is common and mechanical into a non-event, "
    "while leaving untouched the class that arises from deliberate API redesign.",
    pct(double(affected_e - fully_e), double(affected_e))
  );

  const auto breaking = breaks.at("breaking").get<std::uint64_t>();
  const auto declared = breaks.at("declared").get<std::uint64_t>();
  const auto silent = breaks.at("silent").get<std::uint64_t>();
  const auto now = utc_now_iso8601();
  auto total = [](const Json &c) {
    std::uint64_t n = 0;
    for (const auto &[k, v] : c.items())
      n += v.get<std::uint64_t>();
    return n;
  };

  std::map<std::string, std::string> v{
    {"generated_date", now.substr(0, 10)},
    {"generated_at", now},
    {"tool_line", std::string{tool_version()} + " · libabigail library API · libclang C API"},
    {"transitions", std::to_string(transitions)},
    {"libraries", std::to_string(corpus.at("libraries").get<std::uint64_t>())},
    {"objects", with_commas(corpus.at("objects").get<std::uint64_t>())},
    {"c_transitions", std::to_string(corpus.at("c_transitions").get<std::uint64_t>())},
    {"cxx_transitions", std::to_string(corpus.at("cxx_transitions").get<std::uint64_t>())},
    {"c_libraries", std::to_string(s.at("frequency_c").at("libraries").get<std::uint64_t>())},
    {"cxx_libraries", std::to_string(s.at("frequency_cxx").at("libraries").get<std::uint64_t>())},
    {"verdict_rows", verdicts},
    {"rows_all", frequency_rows(freq_all)},
    {"rows_cxx", frequency_rows(s.at("frequency_cxx"))},
    {"rows_c", frequency_rows(s.at("frequency_c"))},
    {"hdr_with_data", std::to_string(hdr.at("with_data").get<std::uint64_t>())},
    {"hdr_with_defs", std::to_string(hdr.at("with_definitions").get<std::uint64_t>())},
    {"hdr_with_defs_pct",
     pct(hdr.at("with_definitions").get<double>(), hdr.at("with_data").get<double>())},
    {"hdr_body_changed", std::to_string(hdr.at("body_changed").get<std::uint64_t>())},
    {"hdr_body_changed_pct",
     pct(hdr.at("body_changed").get<double>(), hdr.at("with_data").get<double>())},
    {"hdr_body_events", with_commas(hdr.at("body_events").get<std::uint64_t>())},
    {"hdr_macro_changed", std::to_string(hdr.at("macro_changed").get<std::uint64_t>())},
    {"hdr_macro_changed_pct",
     pct(hdr.at("macro_changed").get<double>(), hdr.at("with_data").get<double>())},
    {"hdr_macro_events", with_commas(hdr.at("macro_events").get<std::uint64_t>())},
    {"hdr_macro_nv", std::to_string(hdr.value("macro_changed_nonversion", 0ULL))},
    {"hdr_macro_nv_pct",
     pct(hdr.value("macro_changed_nonversion", 0.0), hdr.at("with_data").get<double>())},
    {"hdr_poor", std::to_string(hdr.at("poor_coverage").get<std::uint64_t>())},
    {"hdr_poor_pct", pct(hdr.at("poor_coverage").get<double>(), hdr.at("with_data").get<double>())},
    {"hdr_paragraph", hdr_paragraph},
    {"breaking", std::to_string(breaking)},
    {"breaking_pct", pct(double(breaking), double(transitions))},
    {"declared", std::to_string(declared)},
    {"silent", std::to_string(silent)},
    {"silent_pct", pct(double(silent), double(breaking))},
    {"silent_ratio", declared ? std::format("{:.0f}", double(breaking) / double(declared)) : "∞"},
    {"unstable_list", unstable_list},
    {"rescue_rows", rescue_row("Binary breakage only", rb) +
                      rescue_row("Recompilation-free evolution", re) +
                      rescue_row("…plus changed inline bodies", ri)},
    {"rescue_paragraph", rescue_paragraph},
    {"ceiling_paragraph", ceiling_paragraph},
    {"third_party_events", with_commas(total(s.at("filtered").at("third_party")))},
    {"private_events", with_commas(total(s.at("filtered").at("private_node")))},
    {"mass_rename", std::to_string(corpus.at("mass_rename_excluded").get<std::uint64_t>())},
    {"debug_missing", std::to_string(corpus.at("debug_info_missing").get<std::uint64_t>())},
  };
  std::string html = render(template_html(), v);
  ABISTUDY_TRY_VOID(fs::write_file_atomic(ws.root / "report.html", html));
  log(std::format("wrote {}", (ws.root / "report.html").string()));
  return html;
}

} // namespace abistudy::pipeline
