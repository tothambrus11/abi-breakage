# Methodology, and what it would take to make this production-grade

`abistudy` measures which kinds of ABI change actually occur between
consecutive releases of widely installed C and C++ libraries, and maps each
kind onto the resilience mechanism (opaque layout, non-frozen enums, resilient
dispatch, opt-in inlining) that would make it invisible to clients. This
document describes exactly how the measurement is made, where each number
comes from, and — critically — the places where the current implementation
falls short of a production pipeline.

Everything below refers to the C++26 implementation in `cxx/`. Every
declaration in `src/` carries a contract comment (`@pre`, `@post`, `@errors`,
`@thread`, …); this file is the narrative that ties them together.

---

## 1. Research question, made precise

For each consecutive pair of upstream releases (v1, v2) of a library:

1. Which **change kinds** are present in the public ABI?
2. Would an **already-compiled client** of v1 still work against v2?
   (binary breakage)
3. Would a client still work **without recompiling** if the boundary were
   resilient? (evolution framing)
4. Which resilience mechanism is **load-bearing** — the one without which the
   release would still break?

The change taxonomy is `ChangeKind` in `src/abi/model.hpp`. Its two partitions
`is_binary_breaking` and `is_evolution_relevant` are the two framings; the
total function `mechanism_for` is the mapping to mechanisms. Nothing else in
the code base defines what a "break" is.

---

## 2. Pipeline

Five idempotent stages, each writing schema-versioned JSON artefacts into one
study directory (`Workspace` in `src/pipeline/stages.hpp`). An interrupted run
resumes where it stopped; a stage refuses an artefact whose schema id does not
match.

```
select ──▶ selection.json ──▶ resolve ──▶ plan.json ──▶ diff ──▶ pairs/<id>.json
                                                                 headers/index/<release>.json
                                              headers ──▶ headers/pairs/<id>.json
                                              analyze ──▶ summary.json, report.txt
```

### 2.1 select — corpus choice is automatic

* Input: Debian popcon `by_inst` (ranking of installed binary packages) and the
  archive `Packages.xz` index (for `Depends`).
* A candidate is a `lib*` binary package that is not a `-dev/-doc/-data/…`
  side package and not a language runtime binding (`libpython*`, …).
* Language hint: `Depends: libstdc++6` ⇒ C++, else C. C and C++ are quota'd
  **separately** because popcon's top ranks are almost all C system libraries
  (an unquota'd top-N gave 743 C vs 22 C++ transitions).
* A candidate is accepted iff its source package ships, for the newest
  version, both `<binary>-dbgsym` (DWARF; without it libabigail sees only
  symbol names) and some `-dev` package (needed to separate public API from
  internals).
* One entry per source package.

### 2.2 resolve — consecutive upstream releases

* All archive versions of the source are fetched and sorted with the
  **dpkg algorithm** (`DebianVersion`, deb-version(7)), never lexically.
* Pre-releases (`~` in the upstream part) are dropped.
* One archive version per upstream release is kept (the newest Debian
  revision), so Debian-only rebuilds never count as library evolution.
* The most recent N (default 10) upstream releases that have the packages we
  need form N−1 consecutive pairs.
* Package roles are decided by a **generic rule**: a runtime package is one
  with a `-dbgsym` sibling in the same source version, anchored to the
  popcon-selected binary in a version-blind way (`libssl3` ≈ `libssl4`,
  `libicu72` ≈ `libicu73`). Anchoring matters: `gcc-16` has 166 packages with
  dbgsym siblings.

### 2.3 diff — ABI comparison through the libabigail library

For each pair, in a **child process** (libabigail keeps per-environment state
and is not thread-safe):

1. Both releases are materialised: runtime, dbgsym and dev `.deb`s are
   downloaded from snapshot.debian.org's content-addressed store, **SHA-1
   verified**, extracted in-process with libarchive (secure flags: no `..`,
   no absolute paths, no symlink escapes), and deleted. Peak disk is the
   extracted tree of the pair in flight.
2. Shared objects are paired by **SONAME stem** (`libssl` from
   `libssl.so.3`), so a SONAME bump still compares — this is exactly the case
   `abipkgdiff` skips, because it pairs by filename.
3. `abi::compare` reads each ELF with `dwarf::create_reader`, restricting types
   to those declared under the `-dev` include root via
   `gen_suppr_spec_from_headers`, computes a `corpus_diff` with harmless
   categories **kept** (enum-case additions live there), and classifies by
   walking the diff tree:
   * a `diff_node_visitor` counts each changed **type once** (dedupe by
     canonical type), emitting field added/removed/type-changed,
     member-offset-changed, type-size-changed, base-class-changed,
     enum-case added/removed, and vtable events (per virtual slot);
   * symbol events come from the corpus-level added/deleted/changed function
     and variable maps; a C++ signature change appears as delete+add of the
     same qualified name and is folded into `function_signature_changed`;
     a same-symbol declaration change (C) is a signature change iff parameter
     count differs or a parameter/return **type name modulo cv-qualifiers**
     differs (`char*`→`const char*` is not a break; `int`→`long` is);
   * compiler-generated symbols (`_ZTV/_ZTI/_ZTS/_ZTT`) are excluded — they are
     consequences of class changes, which are counted there.
4. Three attribution filters partition every event (see §3).
5. Optionally, the two `-dev` include trees are indexed with libclang while
   they are on disk (§2.4), so the header stage needs no second download.

Nothing parses `abidiff` text. Every count is traceable to a specific IR node,
and the per-object result keeps the event lists (`type_events`,
`symbol_events`) so a reviewer can audit any number.

### 2.4 headers — what no ABI tool can see

`abidiff` compares DWARF types and ELF symbols; `abi-compliance-checker`
compares declared interfaces. A change to the **body** of a header `inline`
function, an in-class method or a template — code C++ copies into every
client — produces no output from either. Four negative controls in the
calibration suite prove it.

`hdr::index` parses every header under the include root with the **libclang C
API** (`CXTranslationUnit_KeepGoing`, so a missing include does not empty the
unit) and records, for definitions declared in the package's own files:

* a fingerprint of the body's token spellings (whitespace/comments removed),
* a fingerprint of the declaration (name, result type, parameter types),
* for object-like macros with a value, a fingerprint of the value.

The parse language is chosen **per file**: C++-only extensions or C++
constructs in the text (`class`, `template <`, `namespace`, C++ standard
includes) select C++ even inside a C library — z3's `z3++.h` is the canonical
case; parsing it as C found nothing.

Keys are libclang USRs, so overloads and templates do not collide. `compare`
counts same-USR/different-body as `inline_body_changed` and same-key/
different-value as `macro_value_changed`, reporting the latter both in full and
with version/build stamps excluded (`VERSION`, `_DATE`, `BUILD`, `REVISION`,
…): those change every release by construction and a client that inlined one
is not broken by it. **Parse coverage** (files, units
created, units with a fatal diagnostic, files skipped by the cap) is stored
with every index; the analysis flags transitions where more than half the
units hit a fatal diagnostic.

### 2.5 analyze — aggregation

Per transition: public counts of all shared objects merged; header kinds
merged in as `ChangeKind` values; language = C++ if any object is C++;
`soname_changed` if any object's SONAME differs; transitions whose symbol
churn is a policy-driven mass rename are excluded. The report gives, for all /
C / C++: transitions and total events per kind with median and max; declared
vs silent binary breaks; the rescue analysis under both framings with
load-bearing and sole-reason counts per mechanism; header churn with coverage
caveats; per-library instability.

---

## 3. The filters real data forced

Each of these was discovered by inspecting tool output against ground truth.
Skipping any one silently corrupts a whole category.

| Problem | Symptom | Rule (all generic, no per-library configuration) |
|---|---|---|
| Third-party types | glibc's `_IO_FILE`, libstdc++'s `std::tuple` change when the toolchain moves and appear as "the library changed its layout" (1,213 events in the earlier run) | Each type event carries the DWARF declaring file; events outside the `-dev` include root go to `third_party_counts`. Plus header-dir suppression at read time. |
| Private ELF version nodes | dbus exports 657 `_dbus_*` internals under `LIBDBUS_PRIVATE_*` against 240 public symbols; a routine release reads as "651 functions removed" | Symbol events whose version node contains `PRIVATE`/`INTERNAL` go to `private_node_counts`. `--drop-private-types` does not cover functions. |
| Mass rename by policy | ICU suffixes every symbol with the major version: one release = ~5,600 removals + ~5,600 additions | Digits-blind matching of removed↔added linkage names; if ≥50 and ≥ all other symbol events, the transition is `mass_rename` and excluded. |
| `abidiff` default filters | Enum-case additions are "harmless" and hidden; `--leaf-changes-only` omits base-class insertions | Harmless categories are not switched off; the visitor reads base-class maps directly. |
| `abipkgdiff` | Across a SONAME rename it reports removed/added binaries, exit 0, and performs **no** comparison | Objects are paired by SONAME stem; each pair compared. |

---

## 4. Correctness gate

`tests/calibration/cases/` holds 30 synthetic libraries, each with exactly
one known change (or none), as plain files with a `case.json` (`lang`,
`truth`, `breaks`, `note`). `ctest` compiles each with the system compiler and
runs **the same** `abi::compare` and `hdr::index` the corpus uses, asserting:

* the expected `ChangeKind` is present in `public_counts`, or
* for changes invisible to DWARF/ELF by construction, the ABI stage is silent
  **and** the header stage sees the change, or
* for negative controls (no change, pimpl-only change, cv-qualifier change),
  everything is silent.

Unit tests cover the dpkg version algebra against known dpkg orderings, the
strong-type guarantees (compile-time non-interchangeability, JSON round trip,
hashing), and the taxonomy's invariants (names round-trip, framings nest,
mechanism map total).

The gate found three real defects in the first build of the C++ tool: the
visitor received type nodes through the generic overload only; the header
indexer compared canonical roots against non-canonical cursor paths; and
libabigail files a member whose type changed under
`subtype_changed_data_members`, not `changed_data_members`. None of these
would have been visible in aggregate statistics.

The first end-to-end smoke run then found a fourth, outside the gate's reach:
the `.deb` extractor handed libarchive absolute destination paths while also
setting its refuse-absolute-paths flag, so every regular file was refused and
the study "succeeded" with zero transitions. Two consequences were adopted:
`tests/unit/extract_test.cpp` now builds a real `.deb` with libarchive and
checks both extraction and the refusal of `..`/absolute members, and the
analysis treats an empty corpus as a failure rather than a report.

---

## 5. Where this is not yet production-grade — a candid review

Ordered by how much the result would move if fixed.

### 5.1 Threats to the numbers

0. **The previous header numbers were wrong, and the new tool shows why.** An
   earlier iteration of this study measured header churn with a hand-written
   lexical scanner and reported inline-body changes in 23% of transitions.
   The libclang indexer, on the same corpus, finds ~5% overall and ~12% for
   C++ libraries. The lexical scanner counted brace-bearing macros and other
   non-definitions as "inlinable definitions" (it reported 74% of transitions
   shipping at least one; libclang says 33%). Both passed the same 30-case
   ground truth — the suite had no case that separated them. A real parser
   is not optional here, and the calibration suite needs negative cases for
   things that look like definitions but are not.

1. **Header parse coverage is heuristic.** Each header is parsed standalone
   with `-I` for the root, its first-level subdirectories and the multiarch
   dir. Packages that need `pkg-config --cflags` defines, a newer `-std=`,
   or a dependency's headers get fatal diagnostics; with `KeepGoing` the
   unit is usually still populated, but counts are a **lower bound** there.
   *Fix:* parse `Cflags` from the `.pc` files in the extracted tree (they are
   right there), materialise `Build-Depends` `-dev` packages for the include
   path, try `-std=c++23` on fatal, and report coverage prominently.
2. **Transitions are not independent samples.** Ten transitions of one
   library share its conventions; a "% of transitions" figure over-weights
   prolific libraries. *Fix:* report per-library rates alongside, and a
   bootstrap over libraries for intervals.
2b. **History depth is bounded by the dbgsym era, so the sample is bimodal.**
   Automatic `-dbgsym` packages exist only since 2016; older releases shipped
   hand-maintained `-dbg` packages the pipeline does not accept. Of the 113
   libraries with pairs, 61 reach the 9-transition cap and 52 fall short —
   six have a single transition (libXau: only 1.0.9 and 1.0.11 have dbgsym;
   bzip2 went 1.0.6 → 1.0.8). The 714 / 111 ≈ 6.4 average is that mix, not a
   uniform seven releases per library. *Fix:* accept legacy `-dbg` packages
   (their debug files are laid out by path rather than build-id) to lengthen
   the histories of slow-moving libraries; report per-library depth.
3. **What counts as a release, and the resulting mix.** A release is a
   distinct upstream version string as Debian packaged it (Debian revisions
   and binNMUs collapse; `~` pre-releases are dropped). Any change counts, so
   the 772 planned transitions are 62% patch-level, 28% minor, 2% major,
   6% date-stamped snapshots (gcc, libyuv) and 1% repack-suffix-only
   (`+dfsg1`, and xz-utils' `5.6.1 → 5.6.1+really5.4.5`, which is in fact a
   downgrade). Break rates are nearly flat across patch (28%) and minor
   (30%) transitions; the 13 major bumps break 46% of the time; layout
   changes are twice as common in minor as in patch releases (16% vs 7%).
   Debian may also skip upstream releases, so a pair can span several.
   *Fix:* carry the change level and the upstream-version distance in the
   summary and report the frequencies per stratum; treat repack-only pairs
   with suspicion.
4. **Language is a threshold** (≥20 % Itanium-mangled exported functions ⇒
   C++). Mixed libraries land in C. *Fix:* keep the fraction in the
   artefact (it is) and report sensitivity to the threshold.
5. **Header language is inherited from the binary.** A C library with C++
   convenience headers is parsed as C. *Fix:* choose per file from extension
   and content (`extern "C"`, `class`), or parse twice.
6. **Symbol pairing for C++ signature changes uses the qualified name.** Two
   overloads removed and two different ones added are paired greedily.
   *Fix:* pair by demangled name + parameter count first.
7. **The mass-rename heuristic is a threshold** (≥50 digits-blind matches).
   A library renaming 40 symbols per release slips through as churn. *Fix:*
   make it a ratio of the exported surface and report both.
8. **Popcon is one distribution's users.** The corpus is what Debian users
   install; embedded, Windows and macOS ecosystems are absent. That is a scope
   statement, not a bug, but it belongs in every headline.

### 5.2 Robustness of the pipeline

0. **Memory is the real ceiling.** In the first full run, 32 pairs (z3,
   libstdc++, LibreOffice, libheif, openal-soft, libetonyek, libmwaw) were
   SIGKILLed: four concurrent libabigail readers on large DWARF exceeded the
   7.5 GiB the container had, and a `-j4` clang-tidy build running alongside
   made it worse. Three controls now exist, all in `run_diff`/`diff_one`:
   * every child runs under `prlimit --as` (`--child-memory-mb`, default
     5000), so an oversized reader fails in seconds with `bad_alloc` and a
     recorded reason instead of stalling the machine for minutes until the
     OOM killer acts -- a z3 pair went from a 7-minute kill to a 77-second
     recorded failure;
   * a pair is budgeted from HTTP `HEAD` sizes before a byte is fetched
     (`--max-extracted-mb`, assuming ~3x compression), which turned the
     LibreOffice pairs from ten minutes of download each into a two-second
     skip with the reason on record;
   * a child killed by a signal is retried alone after the pool drains, once.
   *Still to do:* size workers to memory rather than cores, and run the
   largest pairs first and serially so the tail of the run is short.
1. **Whole-body buffering.** `Client::get` holds the response in memory and
   `download` re-reads the file to hash it. A 400 MB dbgsym costs 800 MB of
   RAM per worker. *Fix:* stream to a temp file with an incremental EVP
   digest; rename on match.
2. **Rate limiting is per process.** Four workers are four throttles.
   *Fix:* a token bucket in the parent, or `--workers` sized to the politeness
   budget; better, a shared-memory limiter.
3. **Scratch cleanup on SIGKILL.** `TempDir` cleans up on destruction, not
   on a killed child. *Fix:* sweep `scratch/` at stage start; tag dirs with
   the owning PID.
4. **Provenance is thin.** Artefacts record tool version and time, not the
   libabigail/libclang versions, the popcon/Packages fetch time, or the
   snapshot API responses. *Fix:* a `provenance` block in every envelope;
   pin popcon and `Packages` to a snapshot.debian.org timestamp so the corpus
   is reproducible.
5. **Failure taxonomy.** A pair that fails is recorded with an error string;
   there is no classification (network vs. no-DWARF vs. libabigail crash) and
   no retry policy per class. *Fix:* carry `ErrorCode` into `PairResult`,
   retry transient classes with backoff at the orchestration layer.
6. **No integration tests for I/O layers.** The snapshot API parser, the
   `.deb` extractor and the popcon/Packages parsers have no fixtures. *Fix:*
   record a handful of real API responses and a hand-built `.deb` (libarchive
   can write one) as test data.
7. **`std::filesystem` traversal errors are swallowed** in `find_shared_objects`
   / `header_files` (an unreadable subtree becomes "no files"). *Fix:* count
   and report them in coverage.
8. **Env-gated debug output** (`ABISTUDY_DEBUG_DIFF`) prints mangled
   `typeid` names. Fine for a diagnostic, but a `--trace` option with
   structured output belongs in the CLI.

### 5.3 Engineering hygiene

* One Docker image for build and run; ship a multi-stage build with a
  runtime image pinned by digest.
* `-Werror` in CI, clang-tidy, and the Debug configuration's ASan/UBSan run
  of the calibration suite on every change.
* C++26 contracts: the code uses `ABISTUDY_EXPECTS/ENSURES` macros with the
  standard vocabulary; move to `pre()`/`post()` when GCC ships them
  non-experimentally.
* The JSON layer hand-writes (de)serialisers so on-disk names are a deliberate
  interface; a JSON Schema file per artefact would let other tools validate.
* `analyze` writes a text report; a machine-readable per-library CSV and the
  HTML report generator belong in the tool, not in a notebook.

---

## 6. Development process

The gate every change passes, in `scripts/check.sh` (run inside the image):

1. **clang-format** with the project `.clang-format` (LLVM base, 2-space
   indent, 100 columns, block-indented brackets, broken template
   declarations). The pinned version is the image's clang-format 21; the
   `format-check` target fails the build on any drift. Formatting on a host
   with a different clang-format is fine for editing, but the pinned version
   decides.
2. **clang-tidy** with the recommended families (`bugprone-*`, `cert-*`,
   `clang-analyzer-*`, `concurrency-*`, `cppcoreguidelines-*`, `misc-*`,
   `modernize-*`, `performance-*`, `portability-*`, `readability-*`) run as
   part of the build (`-DABISTUDY_TIDY=ON`). `bugprone`, `clang-analyzer`,
   `concurrency` and `performance` findings are errors. Every disabled check
   is listed in `.clang-tidy` with the reason; every `NOLINT` in the code
   names the check and the reason.
3. **ctest**: unit tests (version algebra, strong types, taxonomy, `.deb`
   extraction against a synthetic archive) and the 30-case calibration.

What the first tidy pass found in a code base that already compiled warning-
free under GCC's `-Wall -Wextra -Wpedantic -Wconversion` set, in order of
value:

* Two thread-safety hazards: `std::strerror` in the error paths of a
  pipeline that runs four workers, and `std::getenv` in a hot visitor.
  Replaced by `std::system_category().message` and an explicit trace option.
* `std::expected` results discarded with a C-style `(void)` cast in two
  places where a failure was genuinely acceptable (cache write) and one where
  it was not (recording a failed pair). The intent is now written down:
  `static_cast<void>` with a comment, and the check's `AllowCastToVoid` set.
* Exceptions able to escape `noexcept` entry points (`main`, the contract-
  violation handler); both now have a last-resort handler.
* ~250 mechanical findings (braces, designated initialisers, unnamed
  parameters, joined declarations, C arrays) applied with `run-clang-tidy
  -fix`. Two of those automatic fixes were wrong and the build caught them:
  `misc-const-correctness` const-qualified a libarchive out-parameter, and
  `cppcoreguidelines-pro-type-member-init` gave `Strong` members a `{}`
  initialiser that their deliberately deleted default constructor rejects.
  Auto-fixes are reviewed like any other patch.

Two families are suppressed as false positives with a scoped `NOLINTBEGIN`:
`cert-dcl58-cpp` on the `std::formatter`/`std::hash` specialisations for
`Strong` (specialising for a program-defined type is permitted; the check
does not see through the constraint), and `bugprone-macro-parentheses` on
`ABISTUDY_TRY`, whose first argument is a declaration.

---

## 7. Reproducing

```sh
docker build -t abistudy:dev -f cxx/Dockerfile cxx
docker run --rm -v "$PWD/cxx:/work" -w /work abistudy:dev \
  bash -c 'cmake -S . -B build -G Ninja && cmake --build build && (cd build && ctest --output-on-failure)'
docker run --rm -v "$PWD/cxx:/work" -w /work abistudy:dev \
  build/abistudy all --work study --c-limit 70 --cxx-limit 50 --releases 10 --workers 4
```

`study/report.txt` is the human report; `study/summary.json` is the
machine-readable one; `study/pairs/*.json` carry the per-object event lists
that justify every count.

### Development container

`.devcontainer/devcontainer.json` (at the repository root) builds from this
same `cxx/Dockerfile`, so the container you edit in is the container the gate
runs in; the Feature layer on top adds only interactive tooling (non-root
user, sudo, git, gdb) and can never change a build result. Open the repository
root in VS Code and choose *Reopen in Container*, or drive it headlessly:

```sh
npx @devcontainers/cli up   --workspace-folder .
npx @devcontainers/cli exec --workspace-folder . bash -lc 'cd cxx && scripts/check.sh'
```

The container configures CMake into `cxx/build-dev`, deliberately not into
`cxx/build` or `cxx/build-check`: those caches were written by the
`docker run -v "$PWD/cxx:/work"` invocations above with the source directory
at `/work`, and CMake rejects a cache whose paths have moved. The workspace
mounts at `/workspaces/abi-breakage`, so `notes/`, `results/` and the
superseded Python pipeline in `v2/` stay visible alongside `cxx/`.
