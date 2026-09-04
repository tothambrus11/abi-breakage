# Results: ABI change across consecutive releases of Debian shared libraries

Run of the corrected pipeline (`abistudy 4.0.0`, libabigail 2.4, libclang 23)
on the corpus described in `METHODOLOGY.md`. Every number below is
reproducible from `study/summary.json`; `study/report.txt` is the full
table set and `scripts/inspect_results.py study` prints the diagnostics
used in §5. Numbers are reported as *strict / lenient* (definitions in
`REVIEW.md` §1.3 and METHODOLOGY §4): strict counts every public event,
lenient excludes append-only growth of types the interface only ever
passes by pointer and removals or re-signings of symbols the old release's
headers did not declare.

**DRAFT — numbers are from the 757-pair preview taken while the last big
pairs were still running; they are replaced by the final run below.**

## 1. Realised corpus

| | planned | analysed |
|---|---|---|
| Debian source packages | 120 | 110 |
| consecutive-release transitions | 773 | 730 |
| shared objects compared | | 2031 |
| C transitions (libraries) | | 547 (77) |
| C++ transitions (libraries) | | 183 (37) |

Thirty-four planned transitions contribute nothing and are listed by cause
in §5.1. The release-level mix is 442 patch, 221 minor, 17 major, 9
snapshot, 41 unclassifiable.

## 2. How often does each kind of change happen?

Share of transitions with at least one public event of the kind (strict),
with the 95 % cluster-bootstrap interval over libraries, and the lenient
count. Layout, enum and vtable rows are over the 716 transitions with DWARF
on both sides.

| change kind | strict | 95 % CI | lenient | libraries |
|---|---|---|---|---|
| symbol_added | 28.9 % | [23.5, 34.4] | 211 | 68/110 |
| macro_value_changed | 10.8 % | [6.1, 15.8] | 79 | 30/110 |
| function_signature_changed | 10.5 % | [7.1, 14.3] | 43 | 42/110 |
| symbol_removed | 9.0 % | [5.7, 12.5] | 23 | 33/110 |
| enum_case_added | 7.8 % | [4.8, 11.0] | 56 | 25/109 |
| field_added_to_struct | 6.6 % | [3.7, 9.7] | 29 | 25/109 |
| type_size_changed | 6.0 % | [3.4, 8.8] | 24 | 25/109 |
| inline_body_changed | 5.6 % | [2.7, 9.2] | 41 | 17/110 |
| field_type_changed | 4.5 % | [2.3, 7.0] | 32 | 19/109 |
| member_offset_changed | 3.8 % | [1.7, 5.9] | 27 | 15/109 |
| base_class_changed | 2.2 % | [0.4, 5.1] | 16 | 6/109 |
| field_removed_from_struct | 1.7 % | [0.6, 2.9] | 12 | 8/109 |
| vtable_changed | 1.5 % | [0.0, 4.2] | 11 | 3/109 |
| enum_case_removed | 1.0 % | [0.3, 1.9] | 7 | 6/109 |
| any layout change (types) | 10.5 % | [6.6, 14.8] | 60 | 34/109 |

The lenient column matters for exactly two rows. `symbol_removed` drops
from 66 to 23 transitions and `function_signature_changed` from 77 to 43:
two thirds of the public symbols that disappear or change signature were
never declared in the shipped headers (§4). `field_added_to_struct` drops
from 47 to 29 and `type_size_changed` from 43 to 24: those are structs the
interface only hands out by pointer that grew at the end.

C and C++ differ where expected. Signature changes are 5.7 % of C
transitions and 25.1 % of C++ transitions (C++ overloads and member
functions change name-mangled identity when a parameter type changes);
`base_class_changed` and `vtable_changed` are C++-only in practice.
`macro_value_changed` is a C phenomenon (13.0 % vs 4.4 %).

## 3. Break rates

A transition is *binary-breaking* if it has any event a correctly compiled
consumer could not survive without recompilation (layout of an exposed
type, removed or re-signed symbol); *evolution-breaking* adds non-frozen
enum growth; *evolution-or-inline* adds inline-body and macro churn.

| framing | strict | 95 % CI | lenient | 95 % CI |
|---|---|---|---|---|
| binary | 22.6 % (165/730) | [17.3, 28.2] | 14.2 % (104/730) | [10.0, 18.6] |
| evolution | 26.0 % | [20.6, 31.8] | 19.5 % | [14.9, 24.3] |
| evolution or inline | 28.4 % | [22.7, 34.3] | 22.1 % | [17.2, 27.1] |

Of the 165 strict binary breaks, 7 were *declared* by a SONAME change and
158 were silent; lenient: 5 declared, 99 silent. Sixty-three of 110
libraries (57 %) had at least one strict binary break in their ten
releases; 51 (46 %) under the lenient definition.

By release level (strict / lenient binary break rate):

| level | n | strict | lenient |
|---|---|---|---|
| major | 17 | 47.1 % [24.0, 75.0] | 17.6 % [4.3, 41.2] |
| minor | 221 | 29.4 % [19.5, 39.5] | 17.6 % [10.7, 25.5] |
| patch | 442 | 17.2 % [12.0, 23.0] | 10.6 % [6.5, 14.9] |
| other | 41 | 26.8 % | 24.4 % |
| snapshot | 9 | 55.6 % | 55.6 % |

Patch releases break the binary interface in one transition out of six
under the strict definition and one in ten under the lenient one. The
gradient major > minor > patch is present but shallow: a major version
bump is far from a guarantee of a break and a patch bump is far from a
guarantee of none. The `snapshot` interval is degenerate (all nine
transitions belong to one library) and should be ignored.

## 4. What would a resilient boundary have absorbed?

Counting, per breaking transition, whether every breaking event is of a
kind a resilient ABI mechanism absorbs (opaque layout, non-frozen enums,
resilient dispatch, opt-in inlining):

| framing | breaking (strict) | fully absorbable | share |
|---|---|---|---|
| binary | 165 | 29 | 17.6 % |
| evolution | 190 | 50 | 26.3 % |
| evolution or inline | 207 | 67 | 32.4 % |

Under the corrected mechanism map (REVIEW §1.2: removing or retyping a
field is a break under any layout scheme), the rescue share is a third at
most. The dominant unabsorbable events are symbol removals and signature
changes, which no layout mechanism addresses. Non-frozen enums account for
almost all of the additional rescue in the evolution framing: enum growth
is the most common evolution-only event (56 transitions) and is entirely
absorbed by a non-frozen enum design.

Symbol strata (removals and re-signings of public symbols joined against
the old release's headers):

| stratum | removed | re-signed |
|---|---|---|
| declared in shipped headers | 189 | 772 |
| exported but undeclared | 2618 | 605 |
| undecidable (no or poor header data) | 164 | 25 |

Spot checks of the undeclared stratum (`sqlite3PagerCacheStat`,
`__gmpn_dcpi1_bdiv_q_n`, `ZBUFF_*`, `_XimXTransSocket*Funcs`,
`snappy::internal::*`) confirm that these are internals exported for want
of visibility annotations, not API. One known false negative: symbols
declared only behind opt-in feature macros (`ZSTD_STATIC_LINKING_ONLY`)
are counted as undeclared.

Header-level churn invisible to any ABI tool: 254 transitions (35 %) ship
at least one inlinable definition, 41 (5.6 %) change one, and 79 (10.8 %)
change a public macro value that is not a version or build stamp.

## 5. Threats to validity, blind spots and biases

### 5.1 Transitions lost, and the direction of the bias

| cause | transitions | effect |
|---|---|---|
| runtime package ships no linkable `lib*.so` (klibc: `klibc-*.so`; pam: PAM modules) | 14 | 2 libraries contribute nothing; no bias on rates |
| memory budget (libreoffice: 1.4 GB of packages per release) | 9 | the largest C++ library in the corpus is absent; C++ rates are over small/medium libraries |
| libabigail out of memory under the 6 GB cap (z3, retried at 12 GB) | see final | large C++ libraries under-represented |
| mass-rename policy (openexr 3.1→3.4, per-release symbol suffix) | 1 | excluded by design |
| SONAME stem carrying the version (hunspell 1.6→1.7, openexr 3.1→3.4) | 2 | **fixed in this run**: paired digits-blind; both are declared breaks |

### 5.2 Selection

* **Popularity and packaging.** The corpus is the popcon top of libraries
  that ship `-dbgsym` and `-dev` packages on amd64. Well-maintained,
  widely-depended-on libraries are over-represented; the results describe
  the libraries a typical system links against, not the long tail.
* **Debian, amd64, ten releases.** One distribution's packaging choices
  (symbol-version scripts, `+dfsg` repacks, `t64` renames) and one
  architecture. The ten-release window reaches back to 2015 for
  slow-moving libraries and to last year for fast-moving ones, so per-
  library "time" is not comparable across rows.
* **DWARF availability.** 14 transitions lack debug info on one side (old
  releases before automatic dbgsym); they contribute symbol events only
  and are excluded from layout denominators.
* **Language.** A library is C++ if ≥ 20 % of its exported functions are
  Itanium-mangled; z3 (a C API over a C++ core) is C under this rule.
  Sensitivity: C++ transitions are 197/183/158 at thresholds 10/20/50 %.

### 5.3 Measurement

* **Third-party filter.** Types not declared under the library's own
  headers are excluded (6469 member-offset and 3764 base-class events,
  almost all glibc and libstdc++ types). The filter matches by header
  basename with directory agreement under `/usr/include`; a library whose
  public types are declared in headers it does not ship would lose its
  own events. Calibration shows no such loss in the 33 synthetic cases,
  and freetype and apparmor were verified by hand.
* **Plugins.** Earlier runs counted dlopen'ed plugins (sane backends,
  pipewire SPA modules, libcaca output drivers) as shared objects of the
  library; their internal churn appeared as strict symbol removals. The
  final run restricts objects to linkable library directories.
* **Vague linkage and private nodes.** Weak Itanium-mangled symbols
  (template instantiations, inline functions that were not inlined) and
  symbols in `PRIVATE`/`INTERNAL` version nodes are quarantined: 2089
  weak removals and 3289 private-node removals are not breaks. A library
  that exports a template instantiation *as* its API (rare) would be
  under-counted.
* **Signature changes in C** are detected by parameter-type name modulo
  cv-qualifiers, so a typedef renamed to an identical underlying type
  counts, and a struct retyped behind an unchanged typedef name does not
  (it appears as a layout event instead).
* **Header churn is token-level** and an upper bound; it says nothing about
  semantic equivalence. 130 transitions have poor header parse coverage
  (klibc dominates) and their body/macro counts are lower bounds.
* **Undeclared stratum** depends on libclang seeing the declaration:
  feature-macro-gated declarations and headers outside `/usr/include`
  count as undeclared. The lenient definition therefore slightly
  under-counts breaks of opt-in APIs.

### 5.4 Statistics

* Confidence intervals are cluster bootstraps over libraries (1000
  resamples, seed 42); transitions of one library are not independent
  and the intervals reflect that. Strata with few libraries (`snapshot`,
  `vtable_changed`) produce degenerate or very wide intervals.
* Top-10 libraries carry 39 % of strict binary breaks; no single library
  exceeds 6 %. Removing any one library changes the headline rate by
  under one point.

## 6. Comparison with the pre-correction pipeline

On the 697 transitions common to both runs (`scripts/compare_runs.py`):

| | old pipeline | corrected strict | corrected lenient |
|---|---|---|---|
| binary-breaking transitions | 28.0 % | 21.8 % | 13.3 % |
| fully absorbable among breaking | 46/195 | 26/152 | 17/93 |
| transitions with `symbol_removed` | 121 | 63 | 20 |
| transitions with `macro_value_changed` | 332 | 75 | 75 |

The old figure of 28 % was inflated by weak C++ symbols and private
version nodes counted as removals, and its rescue share by a mechanism
map that credited field removals to opaque layout. The macro column
changed because version and build stamps are now excluded.
