// =============================================================================
// report stage: renders report.html from summary.json. The HTML template is
// embedded in the binary with C++26 `#embed`, so the tool is self-contained.
// Numbers are never typed into the page by hand; every figure comes from the
// summary, and the page grades nothing (REVIEW.md §4.1).
// =============================================================================

#include <algorithm>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "app/stages.hpp"
#include "core/clock.hpp"
#include "core/fs.hpp"
#include "domain/taxonomy.hpp"

namespace abistudy::app {
namespace {

// NOLINTNEXTLINE(*-avoid-c-arrays): `#embed` initialises a C array by design
constexpr unsigned char template_bytes[] = {
#embed "template.html"
};

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

std::string pct(const Json &p) {
  return std::format("{:.1f}%", 100.0 * p.at("share").get<double>());
}
std::string ci(const Json &p) {
  if (!p.contains("ci"))
    return "";
  return std::format(
    "{:.1f}–{:.1f}", 100.0 * p.at("ci")[0].get<double>(), 100.0 * p.at("ci")[1].get<double>()
  );
}
std::string libs(const Json &p) {
  if (!p.contains("libraries_share"))
    return "";
  return std::format(
    "{}/{} ({:.0f}%)", p.at("libraries_count").get<std::uint64_t>(),
    p.at("libraries").get<std::uint64_t>(), 100.0 * p.at("libraries_share").get<double>()
  );
}

std::string_view chip(const Json &row) {
  if (row.at("absorbed").get<bool>())
    return R"html(<span class="chip go">absorbs</span>)html";
  const auto m = row.at("mechanism").get<std::string>();
  if (m == "n/a")
    return R"html(<span class="chip na">additive</span>)html";
  return R"html(<span class="chip no">cannot help</span>)html";
}

std::string frequency_rows(const Json &f) {
  std::string html;
  std::uint64_t maxt = 1;
  for (const auto &r : f.at("rows"))
    maxt = std::max(maxt, r.at("strict").at("count").get<std::uint64_t>());
  for (const auto &r : f.at("rows")) {
    const auto &st = r.at("strict");
    const auto &le = r.at("lenient");
    const auto t = st.at("count").get<std::uint64_t>();
    html += std::format(
      R"html(<tr><td class="kind">{}</td><td class="barcell"><span class="bar{}" style="width:{:.1f}%"></span></td>)html"
      R"html(<td class="num big">{}</td><td class="num">{}</td><td class="num small">{}</td><td class="num">{}</td><td class="num">{}</td><td class="num">{}</td><td class="num">{:.0f}</td><td>{} <span class="mech">{}</span></td></tr>)html",
      r.at("kind").get<std::string>(), r.at("absorbed").get<bool>() ? " hi" : "",
      100.0 * static_cast<double>(t) / static_cast<double>(maxt), t, pct(st), ci(st),
      le.at("count").get<std::uint64_t>(), libs(st), r.at("events").get<std::uint64_t>(),
      r.at("median").get<double>(), chip(r), escape(r.at("mechanism").get<std::string>())
    );
  }
  const auto &lt = f.at("layout_types");
  html += std::format(
    R"html(<tr class="total"><td class="kind">types with any layout change</td><td></td><td class="num big">{}</td><td class="num">{}</td><td class="num small">{}</td><td class="num">{}</td><td class="num">{}</td><td></td><td></td><td>primary layout measure</td></tr>)html",
    lt.at("strict").at("count").get<std::uint64_t>(), pct(lt.at("strict")), ci(lt.at("strict")),
    lt.at("lenient").at("count").get<std::uint64_t>(), libs(lt.at("strict"))
  );
  return html;
}

std::string break_rows(const Json &breaks) {
  std::string html;
  for (const auto &[f, defs] : breaks.items()) {
    for (const auto &[d, b] : defs.items()) {
      html += std::format(
        R"html(<tr><td>{}</td><td>{}</td><td class="num big">{}</td><td class="num">{}</td><td class="num small">{}</td><td class="num">{}</td><td class="num small">{}</td><td class="num">{}</td><td class="num">{}</td></tr>)html",
        f, d, b.at("count").get<std::uint64_t>(), pct(b), ci(b), libs(b),
        b.contains("libraries_ci")
          ? std::format(
              "{:.0f}–{:.0f}", 100.0 * b.at("libraries_ci")[0].get<double>(),
              100.0 * b.at("libraries_ci")[1].get<double>()
            )
          : "",
        b.at("declared").get<std::uint64_t>(), b.at("silent").get<std::uint64_t>()
      );
    }
  }
  return html;
}

std::string level_rows(const Json &strict, const Json &lenient) {
  std::string html;
  for (const auto &[level, b] : strict.items()) {
    const auto &l = lenient.at(level);
    html += std::format(
      R"html(<tr><td>{}</td><td class="num">{}</td><td class="num big">{}</td><td class="num small">{}</td><td class="num big">{}</td><td class="num small">{}</td></tr>)html",
      level, b.at("n").get<std::uint64_t>(), pct(b), ci(b), pct(l), ci(l)
    );
  }
  return html;
}

std::string rescue_rows(const Json &rescue) {
  std::string html;
  for (const auto &[f, defs] : rescue.items()) {
    for (const auto &[d, r] : defs.items()) {
      std::string cells;
      for (const auto &m : r.at("mechanisms")) {
        cells += std::format(
          R"html(<td class="num">{} <span class="small">(sole {})</span></td>)html",
          m.at("needed_by").get<std::uint64_t>(), m.at("sole_reason").get<std::uint64_t>()
        );
      }
      html += std::format(
        R"html(<tr><td>{}</td><td>{}</td><td class="num">{}</td><td class="num big">{} <span class="small">· {:.1f}%</span></td>{}</tr>)html",
        f, d, r.at("affected").get<std::uint64_t>(), r.at("fully_rescued").get<std::uint64_t>(),
        100.0 * r.at("fully_share").get<double>(), cells
      );
    }
  }
  return html;
}

std::string library_rows(const Json &libraries) {
  std::vector<Json> rows(libraries.begin(), libraries.end());
  std::ranges::sort(rows, [](const Json &a, const Json &b) {
    return std::pair{a.at("breaks_strict").get<int>(), a.at("transitions").get<int>()} >
           std::pair{b.at("breaks_strict").get<int>(), b.at("transitions").get<int>()};
  });
  std::string html;
  for (const auto &l : rows) {
    html += std::format(
      R"html(<tr><td>{}</td><td>{}</td><td class="num">{}</td><td class="num">{}</td><td class="num">{}</td></tr>)html",
      escape(l.at("source").get<std::string>()), l.at("language").get<std::string>(),
      l.at("transitions").get<int>(), l.at("breaks_strict").get<int>(),
      l.at("breaks_lenient").get<int>()
    );
  }
  return html;
}

} // namespace

Result<std::string> run_report(const Workspace &ws, const Services &sv) {
  ABISTUDY_TRY(Json s, sv.store.load(ws.summary(), schema_summary));
  const auto &c = s.at("corpus");
  const auto &h = s.at("headers");
  const auto &sy = s.at("symbols");
  auto n = [](const Json &v) { return std::to_string(v.get<std::uint64_t>()); };
  const double wd = h.at("with_data").get<double>();
  auto hp = [&](const char *k) {
    return std::format(
      "{} ({:.1f}%)", h.at(k).get<std::uint64_t>(),
      wd == 0 ? 0.0 : 100.0 * h.at(k).get<double>() / wd
    );
  };
  std::string levels;
  for (const auto &[l, v] : c.at("by_level").items())
    levels += std::format("{}{} {}", levels.empty() ? "" : ", ", l, v.get<std::uint64_t>());

  const std::map<std::string, std::string> v{
    {"generated_at", utc_now_iso8601()},
    {"tool_line", std::string{ports::tool_version()} + " · " + sv.comparer.version() + " · " +
                    sv.indexer.version()},
    {"transitions", n(c.at("transitions"))},
    {"libraries", n(c.at("libraries"))},
    {"objects", n(c.at("objects"))},
    {"c_transitions", n(c.at("c_transitions"))},
    {"c_libraries", n(c.at("c_libraries"))},
    {"cxx_transitions", n(c.at("cxx_transitions"))},
    {"cxx_libraries", n(c.at("cxx_libraries"))},
    {"levels", levels},
    {"mass_rename", n(c.at("mass_rename_excluded"))},
    {"errored", n(c.at("errored"))},
    {"no_linkable_object", n(c.value("no_linkable_object", Json(0)))},
    {"not_attempted", n(c.at("not_attempted"))},
    {"debug_missing", n(c.at("debug_info_incomplete"))},
    {"rows_all", frequency_rows(s.at("frequency").at("all"))},
    {"rows_c", frequency_rows(s.at("frequency").at("c"))},
    {"rows_cxx", frequency_rows(s.at("frequency").at("cxx"))},
    {"break_rows", break_rows(s.at("breaks"))},
    {"level_rows", level_rows(
                     s.at("breaks").at("binary").at("strict").at("by_level"),
                     s.at("breaks").at("binary").at("lenient").at("by_level")
                   )},
    {"rescue_rows", rescue_rows(s.at("rescue"))},
    {"hdr_with_data", n(h.at("with_data"))},
    {"hdr_with_defs", hp("with_definitions")},
    {"hdr_body_changed", hp("body_changed")},
    {"hdr_body_events", n(h.at("body_events"))},
    {"hdr_macro_changed", hp("macro_changed")},
    {"hdr_macro_nv", hp("macro_changed_nonversion")},
    {"hdr_poor", hp("poor_coverage")},
    {"sym_removed_declared", n(sy.at("removed_declared"))},
    {"sym_removed_undeclared", n(sy.at("removed_undeclared"))},
    {"sym_removed_unknown", n(sy.at("removed_unknown"))},
    {"sym_sig_declared", n(sy.at("signature_declared"))},
    {"sym_sig_undeclared", n(sy.at("signature_undeclared"))},
    {"sym_sig_unknown", n(sy.at("signature_unknown"))},
    {"layout_excluded", n(sy.at("layout_events_excluded"))},
    {"library_rows", library_rows(s.at("libraries"))},
  };
  std::string html = render(template_html(), v);
  ABISTUDY_TRY_VOID(fs::write_file_atomic(ws.report_html(), html));
  sv.log(std::format("wrote {}", ws.report_html().string()));
  return html;
}

} // namespace abistudy::app
