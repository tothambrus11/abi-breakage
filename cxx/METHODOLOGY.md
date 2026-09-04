# Methodology

`abistudy` measures which kinds of ABI change occur between consecutive
releases of widely installed C and C++ libraries, and maps each kind onto the
resilience mechanism (opaque layout, non-frozen enums, resilient dispatch,
opt-in inlining) that would make it invisible to clients. This document
describes exactly how the measurement is made, where each number comes from,
and what the numbers can and cannot support. `REVIEW.md` is the referee-style
review that produced the current definitions; `FIDELITY.md` is the design
investigation for the next generation of the tool.

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

The change taxonomy is `ChangeKind` in `src/domain/taxonomy.hpp`. Its
partitions `is_binary_breaking`, `is_evolution_relevant` and
`is_evolution_or_inline` are the three framings; the total function
`mechanism_for` is the mapping to mechanisms. Nothing else in the code base
defines what a "break" is.

### 1.1 Two break definitions bracket the answer

Whether a layout change breaks a client depends on the client: a struct that
clients only ever hold by pointer and never allocate can grow without breaking
anyone. That is a property the library's binary cannot reveal, so every
figure is reported under two definitions (`src/domain/transition.hpp`):

* **strict** — every public event counts;
* **lenient** — a layout event does not count when the type is reached only
  through pointers/references in the exported interface (or not at all) *and*
  the change is append-only; a removed or re-signed symbol does not count
  when no public header declares it.

The strict rate is an upper bound and the lenient rate a lower bound on the
population of compiled clients; the gap between them is itself a result (how
much of "ABI breakage" is opaque-by-convention growth and undeclared exports).

### 1.2 What a mechanism can absorb

`mechanism_for` follows the Swift library-evolution model the mechanisms are
taken from: opaque layout absorbs additions, moves and growth; non-frozen
enums absorb *added* cases; resilient dispatch absorbs inserted or moved
virtual slots; opt-in inlining absorbs changed header bodies. Removals and
retypings (a removed or retyped field, a removed enum case, a removed
symbol, a changed signature) are API changes no indirection helps with.

---

## 2. Architecture

The tool is organised hexagonally (ports and adapters), so the parts that
define the study are testable without libabigail, libclang or the network:

```
src/domain/     taxonomy, events, header model, version algebra, transition
                roll-up (strict/lenient), statistics, summary        -- pure
src/ports/      PackageSource, PackageExtractor, AbiComparer, HeaderIndexer,
                ArtifactStore, ProcessRunner, Log                    -- interfaces
src/adapters/   snapshot (libcurl), libarchive, abigail, libclang, posix, fs
src/app/        the stages: select, resolve, diff, headers, analyze, report
src/cli/        main(): the composition root that wires adapters into ports
```

Adapters produce `ChangeKind` events and the per-event facts the lenient
definition needs; the domain aggregates; the reports render. No adapter type
appears in the domain, and no stage sees libabigail's or libclang's API.

---

## 3. Pipeline

Six idempotent stages, each writing schema-versioned JSON artefacts into one
study directory (`Workspace` in `src/app/stages.hpp`). An interrupted run
resumes where it stopped; a stage refuses an artefact whose schema id does not
match. Every artefact envelope records the tool version and the libabigail
and libclang versions that produced it.

```
select ──▶ selection.json ──▶ resolve ──▶ plan.json ──▶ diff ──▶ pairs/<id>.json
                                                                 headers/index/<release>.json
                                              headers ──▶ headers/pairs/<id>.json
                                              analyze ──▶ summary.json, report.txt
                                              report  ──▶ report.html
```

### 3.1 select — corpus choice is automatic and recorded

* Input: Debian popcon `by_inst` (ranking of installed binary packages) and the
  archive `Packages.xz` index (for `Depends`). Their SHA-1s are stored in the
  selection's provenance so the corpus is reproducible.
* A candidate is a `lib*` binary package that is not a `-dev/-doc/-data/…`
  side package and not a language runtime binding (`libpython*`, …).
* Language hint: `Depends: libstdc++6` ⇒ C++, else C. C and C++ are quota'd
  **separately** because popcon's top ranks are almost all C system libraries.
* A candidate is accepted iff its source package ships, for the newest
  version, both `<binary>-dbgsym` (DWARF) and some `-dev` package. The number
  of candidates rejected at each filter is recorded: the dbgsym filter is a
  selection effect the reader must see (§6).
* One entry per source package.

### 3.2 resolve — consecutive upstream releases

* All archive versions of the source are fetched and sorted with the
  **dpkg algorithm** (`DebianVersion`, deb-version(7)), never lexically.
* Pre-releases (`~` in the upstream part) are dropped.
* One archive version per upstream release is kept (the newest Debian
  revision), so Debian-only rebuilds never count as library evolution.
* The most recent N (default 10) upstream releases that have the packages we
  need form N−1 consecutive pairs.
* Package roles are decided by a **generic rule**: a runtime package is one
  with a `-dbgsym` sibling in the same source version, anchored to the
  popcon-selected binary in a version-blind way (`libssl3` ≈ `libssl4`).
* The download size of every release is recorded (snapshot's file-info API),
  so the diff stage can schedule largest-first and enforce budgets without
  a network round trip.

### 3.3 diff — ABI comparison through the libabigail library

For each pair, in a **child process** (libabigail keeps per-environment state
and is not thread-safe), largest pairs first, at most one "big" pair at a
time, under an address-space cap (`prlimit`), a per-pair timeout and an
optional wall-clock deadline after which remaining pairs are recorded as not
attempted. Every record carries an outcome (`compared`,
`no_linkable_object`, `skipped_budget`, `not_attempted`, `failed_memory`,
`failed_timeout`, `failed`) derived in one place; `diff --retry-failed`
discards the `failed_memory` / `failed_timeout` records and runs them again
under the caps of that invocation, which is how the two z3 pairs that
exceed 6 GB were recovered at 12 GB. A stage that materialises packages
holds an exclusive lock on the workspace's scratch tree, so a diff run
cannot wipe the tree under a concurrent headers run:

1. Both releases are materialised: runtime, dbgsym and dev `.deb`s are
   streamed from snapshot.debian.org's content-addressed store to disk with
   an incremental SHA-1, extracted in-process with libarchive (no `..`, no
   absolute paths, no symlink escapes), and deleted. Peak disk is the
   extracted tree of the pair in flight.
2. The shared objects of the runtime package are the `lib*.so*` ELF files in
   a **linkable library directory**: `lib`, `usr/lib`, `lib64` and their
   multiarch subdirectory (the link editor's defaults), plus every directory
   in which the release's own `-dev` package installs a `lib*.so`
   development link (`hdf5/serial/`, `blas/`: what the package itself
   declares as a link-search path). Anything else (`sane/`, `spa-0.2/`,
   `gstreamer-1.0/`, `caca/`, `security/`) is a dlopen'ed plugin whose
   exported symbols are internal to the loading library; counting them
   would report plugin churn as ABI breaks (before this rule, sane-backends
   showed 8 strict breaks in 9 transitions, all removals of backend-internal
   symbols). Every excluded file is listed in the pair record
   (`excluded_objects`), and a pair with nothing left to compare is reported
   as `no_linkable_object`, not as a failure.
3. Shared objects are paired by **SONAME stem** (`libssl` from
   `libssl.so.3`), so a SONAME bump still compares. A stem that carries the
   version itself (`libhunspell-1.6` → `libhunspell-1.7`, `libOpenEXR-3_1` →
   `libOpenEXR-3_4`) is paired digits-blind when the match is unambiguous on
   both sides; those transitions are exactly the declared breaks, and
   dropping them as "unpaired" would bias the declared/silent split.
4. `AbigailComparer` reads each ELF, restricting types to those declared
   under the `-dev` include root, computes a `corpus_diff` with harmless
   categories **kept** (enum-case additions live there), and classifies by
   walking the diff tree:
   * each changed **type** is counted once; events: field added/removed/
     type-changed, member-offset-changed, type-size-changed,
     base-class-changed, enum-case added/removed, vtable slot inserted/moved
     (a *removed* virtual is a symbol removal, not a slot event, whatever
     its ELF binding: a virtual member function of one of the library's
     own classes is reached through its vtable slot, so an inline virtual
     with a weak symbol is never vague linkage; virtuals of third-party
     class instantiations such as libstdc++'s shared_ptr control blocks
     remain vague linkage);
   * every type event carries its **exposure** in the exported interface —
     `by_value`, `by_pointer`, or `not_in_interface` — computed by walking
     the parameter and return types of every exported function and the types
     of exported variables through typedefs, qualifiers, arrays and
     pointers/references; and whether the change is **append-only** (every
     inserted member at or beyond the old size, nothing removed, retyped or
     moved, no base change);
   * symbol events come from the corpus-level added/deleted/changed function
     and variable maps; a C++ signature change appears as delete+add of the
     same qualified name and is folded into `function_signature_changed`;
     a same-symbol declaration change (C) is a signature change iff parameter
     count differs or a parameter/return **type name modulo cv-qualifiers**
     differs; compiler-generated symbols (`_ZTV/_ZTI/_ZTS/_ZTT`) are excluded;
     every symbol event records its ELF binding.
5. Four tallies partition the events (§4): public, third-party (declared
   outside the library's headers), private version node, vague linkage.
   The per-event list behind the tallies is capped (20 000 symbol events
   per object); an object that hits the cap is flagged, its lenient counts
   stay equal to its strict counts and its symbols are `undecidable`,
   because a sample of the list cannot stand in for the tally.
6. The two `-dev` include trees are indexed with libclang while they are on
   disk (§3.4).

Nothing parses `abidiff` text. Every count is traceable to a specific IR node,
and the per-object result keeps the full event lists so a reviewer — and the
`headers` stage — can audit any number.

### 3.4 headers — what no ABI tool can see, and what the headers declare

`LibclangIndexer` parses every header under the include root with the
libclang C API (`CXTranslationUnit_KeepGoing`, so a missing include does not
empty the unit) and records, for entities declared in the package's own
files:

* a fingerprint of each inline/in-class/template body's token spellings,
* a fingerprint of the declaration (name, result type, parameter types),
* for object-like macros with a value, a fingerprint of the value,
* the **mangled names** of every externally linked function and variable
  declared (`clang_Cursor_getMangling` / `clang_Cursor_getCXXManglings`).

The parse language is chosen per file (C++-only extensions or C++ constructs
in the text select C++ even inside a C library). Parse coverage (files,
units, units with an error or fatal diagnostic, files skipped by the cap) is
stored with every index; where more than half the units hit an error the
index is *poor* and its counts are lower bounds.

The stage compares the two indexes (`inline_body_changed`,
`macro_value_changed` with and without version/build stamps; a stamp is
recognised by whole `_`-separated tokens such as `VERSION`, `MAJOR`,
`GIT`, `BUILD_DATE`, so `MAX_DIGITS` or `MINORBITS` are ordinary macros)
and performs the
**declared-symbol join**: each removed or re-signed symbol of the pair is
looked up in the old release's declared set and classified `declared`,
`undeclared`, or `unknown` (index empty or poor).

### 3.5 analyze — aggregation

`rollup` turns each pair into a `Transition` with strict and lenient counts,
the number of layout-changed types, the symbol strata, the release level of
the pair (`major`/`minor`/`patch`/`snapshot`/`other` from the upstream version
strings: a run of eight or more digits anywhere, `16-20260217`,
`6.5+20250125`, `1.11.0+git20250114`, is a snapshot; equal leading numerics
with a different suffix, `2.1.27+dfsg` → `2.1.27+dfsg2`, are `other`) and
coverage flags. `summarize` then computes, for all / C / C++:

* per kind: transitions affected (strict and lenient), the share with a
  **95 % cluster-bootstrap interval resampling libraries**, the share of
  libraries with at least one event, events, median and max — layout, enum
  and vtable rows over the DWARF-complete subset only;
* the number of transitions with any layout-changed type (the primary layout
  measure, since one edit yields several correlated kinds);
* break rates per framing × definition, per transition and per library with
  intervals, declared (SONAME bumped) vs silent, and per release level;
* the rescue analysis per framing × definition: affected, fully absorbable,
  and per mechanism needed-by / sole-reason counts;
* header churn with coverage caveats; symbol strata; a language-threshold
  sensitivity check; per-library rates.

The summary is one JSON document; the text and HTML reports render it and
compute nothing themselves.

---

## 4. The filters real data forced

| Problem | Symptom | Rule (all generic, no per-library configuration) |
|---|---|---|
| Third-party types | glibc's `_IO_FILE`, libstdc++'s `std::tuple` change when the toolchain moves and appear as "the library changed its layout" | Each type event carries the DWARF declaring file; it is the library's own iff the path matches a shipped header by include-relative **suffix**, and a path under `/usr/include` or `/usr/lib/gcc` additionally needs an exact relative match or a multi-component suffix (so `bits/types.h` is never claimed by a library shipping `types.h`). |
| Private ELF version nodes | dbus exports 657 `_dbus_*` internals under `LIBDBUS_PRIVATE_*` | Symbol events whose version node contains `PRIVATE`/`INTERNAL` go to `private_node_counts`. |
| Vague linkage | C++ template instantiations and inline functions emitted as weak symbols come and go with the compiler | Weak *and* Itanium-mangled symbol events go to `vague_linkage_counts`; every client compiled its own copy. C weak symbols are kept, and so are **virtual member functions of the library's own classes** whatever their binding: they are reached through a vtable slot, so an inline virtual that disappears is a public removal. Virtuals of third-party instantiations (libstdc++ control blocks) stay vague. |
| Undeclared exports | default-visibility builds export internals no header declares | The declared-symbol join (§3.4); undeclared removals are excluded under the lenient definition and reported as a stratum. |
| Mass rename by policy | ICU suffixes every symbol with the major version | Digits-blind matching of removed↔added linkage names; if ≥50 and ≥ all other symbol events, the transition is `mass_rename` and excluded. |
| `abidiff` default filters | Enum-case additions are "harmless" and hidden; `--leaf-changes-only` omits base-class insertions | Harmless categories are not switched off; the visitor reads base-class maps directly. |
| `abipkgdiff` | Across a SONAME rename it performs no comparison | Objects are paired by SONAME stem. |
| Missing DWARF | 15 transitions in the first run were compared on symbols only | Layout, enum and vtable rates are computed over the DWARF-complete subset; the two denominators are stated. |

---

## 5. Correctness gate

`tests/calibration/cases/` holds 33 synthetic libraries, each with exactly
one known change (or none). `ctest` compiles each with the system compiler
and runs **the same** comparer and indexer adapters the corpus uses,
asserting the expected `ChangeKind` and — new in this iteration — the facts
the lenient definition relies on: a field appended to a pointer-only struct
is `by_pointer` + append-only; appended to a by-value struct it is
`by_value`; an exported function absent from the header is `undeclared`; an
explicit template instantiation that disappears is vague linkage; a removed
virtual is a removed symbol, not a slot event.

`tests/unit/domain_test.cpp` covers the taxonomy's invariants, the corrected
mechanism map, the lenient rule, the release-level classifier, the cluster
bootstrap (determinism, bounds), the third-party attribution rule, the
transition roll-up (strict vs lenient counts, strata, header merge) and the
summary's shape — all without libabigail, libclang or the network. The other
unit tests cover the dpkg version algebra, strong types and `.deb` extraction.

The gate (`scripts/check.sh`) enforces clang-format, builds with clang-tidy
(bugprone, analyzer, concurrency and performance findings as errors) and
runs all tests.

---

## 6. Threats to validity (what the numbers can support)

* **Population.** The corpus is Debian's most-installed C/C++ libraries whose
  maintainers ship `-dbgsym` and `-dev` packages, over the releases Debian
  packaged since automatic debug packages exist (2016). The dbgsym filter is
  invisible to the reader unless stated: libraries with debug packages
  disabled, and everything before 2016, are absent. Numbers describe popular,
  well-maintained Linux libraries in the dbgsym era.
* **Unit of analysis.** Transitions are nested within libraries and share
  their conventions; all intervals resample libraries, and library-level
  shares are reported alongside transition-level ones.
* **Release granularity.** A transition is one pair of consecutive upstream
  versions *as Debian packaged them*; Debian may skip releases. Rates are
  reported per release level.
* **Layout dependence is unknowable from the library alone**; the
  strict/lenient bracket is the honest answer.
* **Header churn is token-level**: an upper bound on semantic change, a
  correct count of stale copies. Parse coverage is reported and counts under
  poor coverage are lower bounds; undeclared-symbol classification is only
  attempted where coverage is not poor.
* **Language** is a 20 % mangled-symbol threshold; the sensitivity to 10 %
  and 50 % is reported.
* **Tool versions** are recorded in every artefact; libabigail's reader has
  known limits on unusual DWARF, and a pair it cannot read is recorded as an
  error, never as "no change".

---

## 7. Reproducing

Natively (Ubuntu 24.04; clang 23 from apt.llvm.org, libabigail 2.4):

```sh
cd cxx
CXX=clang++-23 cmake -S . -B build -G Ninja && cmake --build build && (cd build && ctest --output-on-failure)
build/abistudy all --work study --c-limit 70 --cxx-limit 50 --releases 10 --workers 4 --deadline-minutes 270
```

or in the Docker image (`cxx/Dockerfile`, Debian sid) as before. `study/report.txt`
is the human report, `study/summary.json` the machine-readable one,
`study/pairs/*.json` and `study/headers/pairs/*.json` carry the per-object
event lists and the declared-symbol join that justify every count.
