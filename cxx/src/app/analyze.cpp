#include <format>
#include <map>

#include "app/stages.hpp"
#include "core/fs.hpp"
#include "domain/summary.hpp"
#include "domain/transition.hpp"

namespace abistudy::app {

Result<std::string> run_analyze(const Workspace &ws, const Services &sv) {
  ABISTUDY_TRY_VOID(ensure_workspace(ws, sv));
  std::map<std::string, HeaderResult> headers;
  for (const auto &p : sv.store.list(ws.header_pairs())) {
    if (auto h = sv.store.load_as<HeaderResult>(p, schema_header_pair))
      headers.emplace(h->id, std::move(*h));
  }
  SummaryInputs in;
  for (const auto &p : sv.store.list(ws.pairs())) {
    auto pr = sv.store.load_as<PairResult>(p, schema_pair);
    if (!pr) {
      sv.log(std::format("skip {}: {}", p.filename().string(), pr.error().message));
      continue;
    }
    if (pr->error || pr->objects.empty()) {
      const bool skipped =
        pr->error && (pr->error->starts_with("skipped") || pr->error->starts_with("not attempted"));
      (skipped ? in.not_attempted : in.errored)++;
      continue;
    }
    in.objects += static_cast<std::uint32_t>(pr->objects.size());
    const auto h = headers.find(pr->id);
    in.transitions.push_back(rollup(*pr, h == headers.end() ? nullptr : &h->second));
  }
  if (in.transitions.empty())
    return fail(ErrorCode::not_found, "no comparable pair in {}", ws.pairs().string());

  const Json summary = summarize(in);
  std::string text = render_text(summary);
  ABISTUDY_TRY_VOID(sv.store.save(ws.summary(), schema_summary, summary));
  ABISTUDY_TRY_VOID(fs::write_file_atomic(ws.report_text(), text));
  sv.log(std::format("wrote {} and {}", ws.summary().string(), ws.report_text().string()));
  return text;
}

} // namespace abistudy::app
