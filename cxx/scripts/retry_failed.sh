#!/usr/bin/env bash
# Second chance for the memory outliers: pairs whose child was killed or
# timed out under the parallel pass are re-run one at a time with a larger
# address-space cap. Records of pairs skipped by budget or deadline are kept.
#
#   scripts/retry_failed.sh [study-dir] [child-memory-mb]
set -euo pipefail
work="${1:-study}"
cap="${2:-12000}"
bin="${BIN:-build/abistudy}"
log="$work/run.log"
stamp() { echo "== $(date -u +%FT%TZ) $*" | tee -a "$log"; }

removed=0
for f in "$work"/pairs/*.json; do
  if python3 - "$f" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))["data"]
e = d.get("error") or ""
sys.exit(0 if (("killed" in e or e.startswith("timeout") or e.startswith("exit ")) and not d["objects"]) else 1)
EOF
  then rm -f "$f"; removed=$((removed + 1)); fi
done
stamp "retry: removed $removed failed pair records; rerunning alone with --child-memory-mb $cap"
"$bin" diff --work "$work" --workers 1 --child-memory-mb "$cap" \
  --pair-timeout "${PAIR_TIMEOUT:-2400}" 2>&1 | tee -a "$log"
stamp "retry done"
