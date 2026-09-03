# abi-breakage

An empirical study of the ABI changes that occur between consecutive releases of
popular C and C++ libraries in the Debian archive, and of which resilience
mechanisms (opaque layout, non-frozen enums, resilient dispatch, opt-in inlining)
would have absorbed them.

- `cxx/` — the `abistudy` pipeline: a C++26 tool linking libabigail and libclang
  with subcommands `select`, `resolve`, `diff`, `headers`, `analyze`, `report`.
  Methodology, filters, and the production-readiness review live in
  `cxx/METHODOLOGY.md`; the investigation of higher-fidelity designs
  (libclang vs libabigail, tiered evidence, probe-TU DWARF) is
  `cxx/FIDELITY.md`.
- `cxx/study/` — the study workspace: plan, per-transition results, summary,
  and the rendered report. Downloaded packages and header indexes are not
  committed; the diff stage rebuilds them from `plan.json`.
- `scripts/`, `v2/`, `results/` — the earlier Python pipelines, kept for
  cross-checking.

## Building and running the gate

Everything runs inside one Docker image; nothing is installed on the host.

```sh
docker build -t abistudy:dev cxx
docker run --rm -v "$PWD/cxx:/work" -w /work abistudy:dev scripts/check.sh
```

The gate enforces clang-format, builds with clang-tidy (bugprone, analyzer,
concurrency and performance checks as errors), and runs the unit tests plus the
thirty-case calibration suite. CI runs the same command.
