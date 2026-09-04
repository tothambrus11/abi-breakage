# Results: ABI change across consecutive releases of Debian shared libraries

Final run of the corrected pipeline (`abistudy 4.0.0`, libabigail 2.4,
libclang 23; commit noted in `study/run.log`) on the corpus described in
`METHODOLOGY.md`. Every number below is reproducible from
`study/summary.json`; `study/report.txt` is the full table set and
`scripts/inspect_results.py study` prints the diagnostics used in §5.
Numbers are reported as *strict / lenient* (REVIEW.md §1.3, METHODOLOGY
§4): strict counts every public event; lenient excludes append-only growth
of types the interface only ever passes by pointer, and removals or
re-signings of exported symbols that the old release's headers did not
declare.

## 1. Realised corpus

| | planned | analysed |
|---|---|---|
| Debian source packages | 120 | 109 |
| consecutive-release transitions | 773 | 739 |
| shared objects compared | | 1037 |
| C transitions (libraries) | | 551 (77) |
| C++ transitions (libraries) | | 188 (36) |

Thirty-four planned transitions contribute nothing; §5.1 lists them by
cause. Release levels: 449 patch, 223 minor, 17 major, 9 snapshot, 41
unclassifiable. The diff stage took 1 h 56 min on 4 cores for the main pass
plus 17 min of single-worker retries; the whole run stayed inside the six-
hour budget with room to spare.

## 2. How often does each kind of change happen?

Share of transitions with at least one public event of the kind (strict),
the 95 % cluster-bootstrap interval over libraries, the lenient count and
the number of libraries ever showing the kind. Layout, enum and vtable rows
are over the 724 transitions with DWARF on both sides.

| change kind | strict | 95 % CI | lenient | libraries |
|---|---|---|---|---|
| symbol_added | 28.4 % (210) | [23.2, 33.8] | 210 | 68/109 |
| macro_value_changed | 10.8 % (80) | [6.1, 16.2] | 80 | 31/109 |
| function_signature_changed | 9.9 % (73) | [6.8, 13.2] | 45 | 42/109 |
| symbol_removed | 8.5 % (63) | [5.6, 11.7] | 23 | 33/109 |
| enum_case_added | 8.0 % (58) | [4.8, 11.2] | 58 | 26/108 |
| field_added_to_struct | 6.5 % (47) | [3.7, 9.6] | 29 | 25/108 |
| type_size_changed | 5.9 % (43) | [3.4, 8.8] | 24 | 25/108 |
| inline_body_changed | 5.7 % (42) | [2.6, 9.4] | 42 | 16/109 |
| field_type_changed | 4.4 % (32) | [2.3, 6.7] | 32 | 19/108 |
| member_offset_changed | 3.7 % (27) | [1.8, 5.8] | 27 | 15/108 |
| base_class_changed | 2.2 % (16) | [0.4, 4.8] | 16 | 6/108 |
| field_removed_from_struct | 1.7 % (12) | [0.6, 2.9] | 12 | 8/108 |
| vtable_changed | 1.5 % (11) | [0.0, 4.0] | 11 | 3/108 |
| enum_case_removed | 1.0 % (7) | [0.3, 1.8] | 7 | 6/108 |
| any layout change (types) | 10.4 % (75) | [6.5, 14.5] | 60 | 34/108 |

The lenient column matters for four rows. `symbol_removed` drops from 63
to 23 transitions and `function_signature_changed` from 73 to 45: most
public symbols that disappear or change signature were never declared in
the shipped headers (§4). `field_added_to_struct` drops from 47 to 29 and
`type_size_changed` from 43 to 24: those are structs the interface only
hands out by pointer and that grew at the end.

C and C++ differ where the language predicts it. Signature changes occur
in 6.0 % of C transitions and 21.3 % of C++ transitions (a C++ parameter
type change renames the mangled symbol); `base_class_changed` and
`vtable_changed` are C++-only in practice; `macro_value_changed` is a C
phenomenon (13.2 % vs 3.7 %). Strict `symbol_removed` is 6.5 % in C and
14.4 % in C++, but lenient it is 17 vs 6 transitions: C++ libraries export
far more undeclared internals.

## 3. Break rates

A transition is *binary-breaking* if it has any event a correctly compiled
consumer could not survive without recompilation (layout of an exposed
type, removed or re-signed symbol); *evolution-breaking* adds non-frozen
enum growth; *evolution-or-inline* adds inline-body and macro churn.

| framing | strict | 95 % CI | lenient | 95 % CI |
|---|---|---|---|---|
| binary | 21.9 % (162/739) | [16.7, 27.0] | 14.3 % (106/739) | [10.2, 18.6] |
| evolution | 25.4 % (188) | [20.1, 30.7] | 19.6 % (145) | [14.6, 24.3] |
| evolution or inline | 27.7 % (205) | [22.1, 33.2] | 22.2 % (164) | [16.6, 27.7] |

Of the 162 strict binary breaks, 8 were *declared* by a SONAME change and
154 were silent (lenient: 6 declared, 100 silent). Fifteen transitions
changed a SONAME; ten of them carried a strict break and five did not.
Sixty-three of 109 libraries (57.8 %, CI [48.6, 67.0]) had at least one
strict binary break within their ten releases; 51 (46.8 %) under the
lenient definition.

By release level (strict / lenient binary break rate):

| level | n | strict | lenient |
|---|---|---|---|
| major | 17 | 47.1 % [24.0, 75.0] | 17.6 % [4.3, 41.2] |
| minor | 223 | 28.7 % [19.5, 38.7] | 18.4 % [11.3, 25.8] |
| patch | 449 | 16.5 % [11.5, 22.4] | 10.5 % [6.7, 14.9] |
| other | 41 | 26.8 % [10.0, 51.9] | 24.4 % [8.8, 51.7] |
| snapshot | 9 | 55.6 % | 55.6 % |

Patch releases break the binary interface in one transition in six under
the strict definition and one in ten under the lenient one. The gradient
major > minor > patch exists but is shallow: a major bump is far from a
guarantee of a break and a patch bump is far from a guarantee of none. The
`snapshot` interval is degenerate (all nine transitions belong to one
library) and should be ignored.

## 4. What would a resilient boundary have absorbed?

Per breaking transition, whether every breaking event is of a kind that a
resilient ABI mechanism absorbs (opaque layout, non-frozen enums,
resilient dispatch, opt-in inlining):

| framing | breaking (strict) | fully absorbable | breaking (lenient) | fully absorbable |
|---|---|---|---|---|
| binary | 162 | 29 (17.9 %) | 106 | 19 (17.9 %) |
| evolution | 188 | 51 (27.1 %) | 145 | 52 (35.9 %) |
| evolution or inline | 205 | 68 (33.2 %) | 164 | 71 (43.3 %) |

Under the corrected mechanism map (REVIEW §1.2: removing or retyping a
field breaks under any layout scheme) opaque layout alone rescues 29
transitions, 23 of them where it is the sole mechanism needed. The
unabsorbable remainder is dominated by symbol removals and signature
changes, which no layout mechanism addresses. Non-frozen enums account for
almost all of the additional rescue in the evolution framing: enum growth
is the most common evolution-only event (58 transitions) and a non-frozen
enum design absorbs all of it.

Symbol strata (removals and re-signings of public symbols joined against
the old release's headers):

| stratum | removed | re-signed |
|---|---|---|
| declared in shipped headers | 189 | 791 |
| exported but undeclared | 2056 | 434 |
| undecidable (no or poor header data) | 164 | 27 |

Spot checks of the undeclared stratum (`sqlite3PagerCacheStat`,
`__gmpn_dcpi1_bdiv_q_n`, `ZBUFF_*` and `ZSTDv07_*`,
`_XimXTransSocket*Funcs`, `snappy::internal::*`, `_json_c_strerror`)
confirm that these are internals exported for want of visibility
annotations, not API. Five libraries (libde265, srt, libzstd, sane-
backends, rubberband) account for 1960 of the 2056 undeclared removals.
One known false negative: symbols declared only behind opt-in feature
macros (`ZSTD_STATIC_LINKING_ONLY`) count as undeclared.

Header-level churn invisible to any ABI tool: 250 transitions (33.8 %)
ship at least one inlinable definition, 42 (5.7 %) change one, and 80
(10.8 %) change a public macro value that is not a version or build stamp
(347, or 47 %, if stamps are counted).

## 5. Threats to validity, blind spots and biases

### 5.1 Transitions lost, and the direction of the bias

| cause | transitions | effect on the estimates |
|---|---|---|
| runtime package ships no linkable `lib*.so`: klibc (`klibc-*.so`), pam (PAM modules), pipewire (popcon picked `libspa-0.2-modules`, a plugin package) | 23 | three libraries contribute nothing; no direction, but pipewire's real library (`libpipewire-0.3`) is absent from the corpus |
| memory budget: libreoffice, 1.4 GB of packages per release | 9 | the largest C++ library in the corpus is absent; C++ rates describe small and medium libraries |
| mass-rename policy: openexr 3.1→3.4 renames every symbol's version node | 2 | excluded by design; this is a declared break that the declared/silent count does not see |
| libabigail out of memory under the 6 GB cap: z3 4.8.10→4.8.12 and 4.8.12→4.13.3 | 0 | recovered by the 12 GB single-worker retry (peak 6.7 GB) |
| SONAME stem carrying the version: hunspell 1.6→1.7, openexr 3.1→3.4 | 0 | recovered by digits-blind pairing; hunspell 1.7.0 is a declared break with 21 signature changes |

### 5.2 Selection

* **Popularity and packaging.** The corpus is the popcon top of libraries
  that ship `-dbgsym` and `-dev` packages on amd64. Well-maintained,
  widely-depended-on libraries are over-represented; the results describe
  what a typical system links against, not the long tail.
* **Debian, amd64, ten releases.** One distribution's packaging choices
  (symbol-version scripts, `+dfsg` repacks, `t64` renames, snapshot
  uploads) and one architecture. The ten-release window reaches back to
  2015 for slow-moving libraries and to last year for fast-moving ones,
  so per-library calendar time is not comparable across rows, and
  fast-releasing libraries contribute recent history only.
* **Pre-releases.** The prerelease filter recognises Debian's `~` form;
  upstream-style `26.8.0.0.alpha1` slipped through (libreoffice, which was
  then skipped for size). Release-level classification puts such versions
  in `snapshot` or `other`.
* **DWARF availability.** 15 transitions lack debug info on one side (old
  releases before automatic dbgsym); they contribute symbol events only
  and are excluded from layout denominators.
* **Language.** A library is C++ if ≥ 20 % of its exported functions are
  Itanium-mangled; z3 (a C API over a C++ core) is C under this rule.
  Sensitivity: the C++ transition count is 203 / 188 / 163 at thresholds
  10 / 20 / 50 %.

### 5.3 Measurement

* **Third-party filter.** Types not declared under the library's own
  headers are excluded (6118 member-offset and 5148 base-class events,
  almost all glibc and libstdc++ types). The filter matches by header
  basename with directory agreement under `/usr/include`; a library whose
  public types live in headers it does not ship would lose its own
  events. The 33 synthetic calibration cases show no such loss, and
  freetype (installed under `freetype2/`) and apparmor were checked by
  hand.
* **Plugins.** The preview run counted dlopen'ed plugins (over 90 sane
  backends, 23 pipewire SPA modules, libcaca output drivers) as shared objects of the
  library; their internal churn produced 8 strict breaks in sane-backends'
  9 transitions. The final run restricts objects to linkable library
  directories; sane-backends now shows 3 strict / 0 lenient, all from
  `sanei_*` helpers exported by `libsane.so.1` itself.
* **Vague linkage and private nodes.** Weak Itanium-mangled symbols and
  symbols in `PRIVATE`/`INTERNAL` version nodes are quarantined: 2488 weak
  removals and 3289 private-node removals (dbus alone removes 330 per
  release) are not breaks. A library that exports a template
  instantiation *as* its API would be under-counted.
* **Signature changes in C** compare parameter-type names modulo
  cv-qualifiers, so a typedef rename to an identical underlying type counts
  and a struct retyped behind an unchanged typedef name does not (it
  surfaces as a layout event instead).
* **Header churn is token-level** and an upper bound; it says nothing
  about semantic equivalence. 139 transitions (18.8 %) have poor header
  parse coverage, mostly libraries whose headers need a configuration
  header or compiler flags the indexer does not supply; their body and
  macro counts are lower bounds and their symbol strata are `undecidable`.
* **Undeclared stratum** depends on libclang seeing the declaration:
  feature-macro-gated declarations and headers outside `/usr/include`
  count as undeclared, so the lenient definition slightly under-counts
  breaks of opt-in APIs. The direction is known; the magnitude is bounded
  by the 434 undeclared re-signings, of which the zstd case is the only
  one found in spot checks.

### 5.4 Statistics

* Confidence intervals are cluster bootstraps over libraries (1000
  resamples, seed 42); a library's transitions are not independent and
  the intervals reflect that. Strata with few libraries (`snapshot`,
  `vtable_changed`, `base_class_changed`) give degenerate or very wide
  intervals.
* The ten most break-prone libraries carry 38.9 % of strict binary breaks;
  none exceeds 5.6 %. Leaving any one library out moves the headline
  strict rate by at most 0.96 points (intel-gmmlib, which breaks in all
  nine of its transitions).

## 6. Comparison with the pre-correction pipeline

On the 702 transitions common to both runs (`scripts/compare_runs.py`):

| | old pipeline | corrected strict | corrected lenient |
|---|---|---|---|
| binary-breaking transitions | 29.3 % | 20.9 % | 13.2 % |
| fully absorbable among breaking | 46/206 | 26/147 | 17/93 |
| transitions with `symbol_removed` | 132 | 60 | 20 |
| transitions with `function_signature_changed` | 78 | 65 | 38 |
| transitions with `macro_value_changed` | 325 | 76 | 76 |

The old figure of 29 % was inflated by weak C++ symbols and private
version nodes counted as removals, and its rescue share by a mechanism map
that credited field removals and retypings to opaque layout. The macro
column changed because version and build stamps are now excluded. Eleven
`vtable_changed` transitions against five before come from counting a
removed virtual as a symbol removal and only inserted or moved slots as
vtable events, plus the attribution fix that restored events on types
declared in installed-prefix headers.

## 7. Summary

Across 739 consecutive-release transitions of 109 widely-installed Debian
libraries, about one transition in five (21.9 %, CI [16.7, 27.0]) changes
the binary interface in a way a compiled consumer cannot survive, and one
in seven (14.3 %, CI [10.2, 18.6]) once exported-but-undeclared symbols and
pointer-only struct growth are discounted. Fewer than one break in twenty
is announced by a SONAME change. Patch releases break at half the rate of
minor releases but are far from safe. A resilient boundary of the kind
Swift's library-evolution mode provides would have absorbed 18 % of the binary breaks and, with non-frozen enums, 27 %
of the evolution breaks; the rest are removals and re-signings that only
policy, not layout, can prevent.
