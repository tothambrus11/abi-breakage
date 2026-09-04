# Methodology review: the study as an academic artefact

This is a referee-style review of the `abistudy` methodology (as described in
`METHODOLOGY.md` and implemented in `src/`) against the standard of an
empirical software-engineering paper, followed by the corrections adopted.
Each finding names the threat category (construct / internal / external /
conclusion validity), its likely direction and size, and what was changed.
Findings marked **adopted** are implemented in the tool; **reported** ones
are surfaced as caveats or sensitivity analyses in the output; **deferred**
ones are recorded for later work.

---

## 1. Construct validity — does "break" measure what the question asks?

### 1.1 Every layout change of a public type is counted as a binary break. Overstates. **Adopted (bracketed estimate).**

A type declared in a `-dev` header is not necessarily part of the client's
compiled layout. Many C libraries expose structs that clients only ever hold
by pointer and never allocate or index — *opaque by convention*. For such a
type, appending a field or growing the size breaks no compiled client, yet the
tool counts it as a break under every framing.

The correct fix cannot be a point estimate, because whether a client touches
the layout is a property of the client, not the library. The tool now
classifies every type-level event by **how the type is exposed in the
library's own exported interface** (from DWARF): `by_value` (the type, or an
array of it, appears by value as a parameter or return of an exported
function), `by_pointer` (reached only through pointers/references), or
`not_in_interface` (declared in a public header, changed, but reachable from
no exported function — header-only helpers, config structs, macros' targets).
Layout events additionally carry `append_only` (every inserted member starts
at or beyond the old size, no member removed or retyped, no offset moved).

Two break definitions are reported side by side:

* **strict** — the previous definition: any layout event in a public type;
* **lenient** — a layout event is a break unless the type is `by_pointer` or
  `not_in_interface` *and* the change is `append_only`.

The true rate lies between them; the gap is itself a finding (how much of
"ABI breakage" is opaque-by-convention growth).

### 1.2 The mechanism map credits opaque layout and non-frozen enums with absorbing removals. Overstates rescues. **Adopted.**

`field_removed_from_struct`, `field_type_changed` and `enum_case_removed`
were mapped to *absorbable*. A resilient layout resolves offsets at run time;
it cannot conjure a removed field or retype one — a compiled client's accessor
for that field no longer exists. Likewise a non-frozen enum lets clients
tolerate *unknown* cases; a *removed* case is an API removal. Swift's library
evolution model — the reference for these mechanisms — forbids all three.
They now map to `Mechanism::none`. Effect on the previous run: 11 + 31 + 2
transitions leave the "fully rescued" set (C only: 10 + 28 + 2).

### 1.3 `vtable_changed` lumps removed virtuals with slot insertions and moves. **Adopted.**

Resilient dispatch absorbs inserted and moved slots. A *removed* virtual
method is an API removal (and is already counted as `symbol_removed` for its
mangled name), so counting it under `vtable_changed` double-counts and
mis-attributes it to a mechanism that cannot help. `vtable_changed` now counts
insertions, moves and virtuality changes only.

### 1.4 `symbol_removed` counts exported symbols regardless of whether any client could name them. Overstates the "cannot help" ceiling. **Adopted (stratified).**

Two populations of exported symbols are removed without breaking any
source-level client:

* **vague-linkage symbols** — C++ template instantiations and inline
  functions emitted as `STB_WEAK`: every client that used them compiled its
  own copy. Their appearance and disappearance is compiler bookkeeping.
  Such events (weak binding *and* Itanium-mangled) now go to a separate
  `vague_linkage_counts` tally and never count as breaks. C weak symbols are
  kept: a C client links against them.
* **undeclared exports** — symbols exported (typically by default visibility)
  that no public header declares. The header indexer now records the
  **mangled names of every function and variable declared in the public
  headers** (`clang_Cursor_getMangling` / `clang_Cursor_getCXXManglings`), and
  the header stage joins the pair's symbol events against that set. Symbol
  events are reported as `declared` / `undeclared` / `unknown` (header parse
  coverage too poor to decide). The lenient break definition counts only
  declared removals and signature changes; the strict one counts all.

This is the L0 stratum of `FIDELITY.md` §8.2 implemented with the C API; the
used-surface (L1) join over reverse dependencies remains deferred.

### 1.5 One structural edit yields several correlated kinds. Distorts the frequency table. **Reported.**

Inserting a field in the middle of a struct produces `field_added`,
`member_offset_changed` and `type_size_changed` at once. The per-kind rows are
therefore not independent; the rescue analysis (which works on kind *sets*)
is unaffected, but a reader comparing rows is misled. The frequency table
now also reports **types with any layout change** per transition as the
primary layout measure, with the sub-kinds as diagnostics.

### 1.6 Third-party attribution matches header basenames. Both directions. **Adopted.**

A type declared in glibc's `types.h` was attributed to any library that also
ships a `types.h`. Attribution now matches the DWARF declaration path by
**include-relative suffix** (`foo/bar.h`, not `bar.h`) and treats a
declaration under `/usr/include/` or `/usr/lib/gcc/` as third-party unless the
same relative path is shipped by the `-dev` package (Debian builds compile
against the source tree, not installed headers).

### 1.7 Header-body churn is a token-level fingerprint. Upper bound. **Reported.**

Renaming a local variable changes the fingerprint. The measure is a correct
count of *bodies that changed*, which is exactly the hazard for a client
holding a stale copy, but it is an upper bound on *semantically* different
bodies. All framings involving it are labelled as such; the third framing is
reported as a ceiling.

---

## 2. Internal validity — are the counts right?

### 2.1 Fifteen transitions were compared without debug info on one side but pooled with the rest. **Adopted.**

Symbol-only comparisons can see no layout event, so pooling them deflates
layout rates. Layout and enum frequencies are now computed over the
DWARF-complete subset; symbol frequencies over all. The two denominators are
stated.

### 2.2 `symbol_events` are written but never read back, so nothing downstream could audit or stratify them. **Adopted.**

The pair artefact now round-trips its event lists (cap raised from 2 000 to
20 000 symbol events; ICU-style mass renames exceed 5 000), and the header
stage consumes them for the declared/undeclared join.

### 2.3 The ordering of `type_events` deduplication depends on libabigail's traversal. **Reported.**

Dedupe is by canonical type, so the count is order-independent; only the
`declared_in` path of the *first* visit is kept. Harmless for counts.

### 2.4 Mass-rename detection is a fixed threshold of 50. **Reported.**

Now also expressed as a fraction of the exported surface in the artefact, and
the summary states both. The exclusion rule is unchanged (≥ 50 and ≥ all
other symbol events), since only ICU triggered it.

---

## 3. External validity — what population do the numbers describe?

### 3.1 The corpus is Debian's most-installed libraries whose maintainers ship dbgsym. **Reported (prominently).**

This is two selection filters, one of them (dbgsym) invisible to the reader:
libraries whose maintainers do not enable automatic debug packages, and all
releases before 2016, are absent. Numbers describe *popular, well-maintained
Linux libraries in the dbgsym era*, not "C/C++ libraries". The report now
carries the corpus description, the number of candidates rejected at each
filter, and the popcon/`Packages` snapshot hashes so the selection is
reproducible.

### 3.2 Transitions are nested within libraries. Conclusion validity. **Adopted.**

Ten transitions of one library share its conventions. All headline
proportions are now reported (a) per transition, as before, (b) **per
library** (the share of libraries with at least one event of the kind, and
the median per-library rate), and (c) with **95 % cluster-bootstrap
intervals** resampling libraries, not transitions (1 000 resamples, seeded).
Inference is stated at the library level.

### 3.3 "Consecutive release" mixes patch, minor, major and date-stamped versions. **Adopted.**

Each transition is classified from its two upstream version strings as
`major`, `minor`, `patch`, `snapshot` (a component of eight or more digits),
or `other`, and break rates are reported per stratum. Debian may skip upstream
releases, so a pair can span several; the version distance is recorded when
the strings are numeric.

### 3.4 Language is a 20 % mangled-symbol threshold. **Reported.**

The fraction is stored per object; the summary reports how many transitions
would change class at 10 % and 50 %.

---

## 4. Conclusion validity and presentation

### 4.1 The HTML report editorialised ("Worth it" / "Marginal") on arbitrary 15 % thresholds. **Adopted (removed).**

A research artefact reports estimates and intervals; it does not grade
mechanisms on hand-picked cut-offs. The verdict rows are replaced by the
per-mechanism load-bearing counts with intervals.

### 4.2 No provenance of tool versions. **Adopted.**

Every artefact envelope now records the libabigail and libclang versions and
the study's input hashes (popcon, `Packages`), so a number can be traced to
the exact readers that produced it.

---

## 5. Simplifications

* The `Transition` roll-up, break predicates, statistics and summary are pure
  functions in `domain/`, tested without libabigail, libclang or the network.
* The three framings collapse into one table with strict/lenient columns
  rather than three near-identical sections.
* `report.cpp` renders numbers only; prose paragraphs with computed
  comparisons ("N× as frequent") are gone — they belong in the paper, where a
  human decides which comparisons are meaningful.

---

## 6. Deferred

* Reverse-dependency import scan (L1) and the `L1 ⊆ L0` parse validator.
* clang C++ AST layout evaluator and probe-TU DWARF (`FIDELITY.md` §2).
* Claude-assisted per-package build profiles (`FIDELITY.md` §7).
* Legacy `-dbg` packages and debuginfod for pre-2016 history.
