#!/usr/bin/env bash
# The gate every change must pass. Runs inside the abistudy:dev image:
#   1. clang-format compliance      (style is not up for discussion in review)
#   2. build with clang-tidy on     (rules in .clang-tidy; bugprone/analyzer/
#                                    concurrency/performance are errors)
#   3. unit tests + calibration     (the correctness gate)
# Usage: docker run --rm -v "$PWD:/work" -w /work abistudy:dev scripts/check.sh [build-dir]
set -euo pipefail
build="${1:-build-check}"
# clang-tidy with the cppcoreguidelines family needs ~1.5 GB per translation unit on the
# libabigail-heavy files; JOBS=2 keeps the gate inside an 8 GB container. Override when roomier.
jobs="${JOBS:-2}"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DABISTUDY_TIDY=ON >/dev/null
cmake --build "$build" --target format-check
cmake --build "$build" -- -k 0 -j "$jobs"
(cd "$build" && ctest --output-on-failure)
echo "check.sh: all gates passed"
