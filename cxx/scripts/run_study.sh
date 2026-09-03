#!/usr/bin/env bash
# Runs the study end to end inside a wall-clock budget and leaves a log with
# per-stage timestamps next to the artefacts.
#
#   scripts/run_study.sh [study-dir] [deadline-minutes-for-diff]
#
# select/resolve are assumed to have produced selection.json and plan.json
# already (they are cheap and idempotent; rerun them by hand to change the
# corpus). Pair and header results from an earlier schema are removed first:
# every stage refuses artefacts it cannot read, and a stale result would
# otherwise be counted as "done".
set -euo pipefail
work="${1:-study}"
deadline="${2:-270}"
bin="${BIN:-build/abistudy}"
log="$work/run.log"
mkdir -p "$work"
stamp() { echo "== $(date -u +%FT%TZ) $*" | tee -a "$log"; }

stamp "cleaning results of earlier schemas"
rm -rf "$work/pairs" "$work/headers/pairs" "$work/headers/index" "$work/scratch"
rm -f "$work/summary.json" "$work/report.txt" "$work/report.html"

stamp "diff (workers=${WORKERS:-4}, deadline=${deadline} min)"
"$bin" diff --work "$work" --workers "${WORKERS:-4}" --deadline-minutes "$deadline" \
  --pair-timeout "${PAIR_TIMEOUT:-1200}" --child-memory-mb "${CHILD_MB:-6000}" \
  --big-pair-mb "${BIG_MB:-120}" --max-extracted-mb "${MAX_EXTRACTED_MB:-2500}" 2>&1 | tee -a "$log"

stamp "headers"
"$bin" headers --work "$work" 2>&1 | tee -a "$log"

stamp "analyze"
"$bin" analyze --work "$work" > "$work/report.stdout.txt" 2>>"$log"

stamp "report"
"$bin" report --work "$work" 2>&1 | tee -a "$log"
stamp "RUN DONE"
