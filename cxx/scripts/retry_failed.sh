#!/usr/bin/env bash
# Second chance for the memory and time outliers: pairs whose record says
# failed_memory or failed_timeout (see PairOutcome in domain/records.hpp) are
# re-run one at a time under a larger address-space cap and a longer timeout.
# The classification lives in the tool (`diff --retry-failed`); this script
# only chooses the caps and logs the pass.
#
#   scripts/retry_failed.sh [study-dir] [child-memory-mb]
set -euo pipefail
work="${1:-study}"
cap="${2:-12000}"
bin="${BIN:-build/abistudy}"
log="$work/run.log"
stamp() { echo "== $(date -u +%FT%TZ) $*" | tee -a "$log"; }

stamp "retry: rerunning failed_memory/failed_timeout pairs alone with --child-memory-mb $cap"
"$bin" diff --work "$work" --retry-failed --workers 1 --child-memory-mb "$cap" \
  --pair-timeout "${PAIR_TIMEOUT:-2400}" 2>&1 | tee -a "$log"
stamp "retry done"
