# Raising fidelity: libclang vs libabigail, and the path to a general ABI diff tool

This is the investigation the study's next iteration should be built on. The
question posed: *instead of libabigail (which reads DWARF), could we use
libclang and evaluate the public ABI ourselves? What are the tradeoffs? Can we
lift code from libabigail? The goal is the best ABI diff tool that works on any
published Linux library package with a C- or C++-linkable interface.*

Short answer: **do not replace libabigail with libclang — layer them.** The two
tools read different projections of the ABI, and neither projection contains
the other. The highest-fidelity design uses three evidence sources with
graceful degradation — ELF dynamic symbols (universal), a clang-built header
model (any package with a `-dev`), and DWARF via libabigail (when debug info
exists) — and treats *disagreement between sources* as a measured quantity
rather than an error. Where we do "evaluate the ABI ourselves", the right way
is not to reimplement the Itanium rules over libclang's C API but to link
clang's own C++ AST libraries (`ASTRecordLayout`, `VTableContext`,
`ItaniumMangleContext`): the compiler's production implementation of the ABI
is the best oracle that exists, and reimplementing it is where the fidelity
bugs would come from.

---

## 1. What each evidence source can and cannot see

An ABI diff is only as good as its input, and there are exactly three inputs
available for a published binary package:

1. **ELF dynamic symbol table** (`.dynsym` + version nodes) — present in every
   shared object ever published. Names, versions, sizes, weak/ifunc/TLS flags.
   No types, no layouts.
2. **DWARF** — present only when debug info was shipped (`-dbgsym` since
   ~2016, hand-made `-dbg` before, or a debuginfod server). Describes what the
   compiler *actually emitted*: real struct layouts after all `#ifdef`s and
   build flags, real member offsets, the types of exported functions.
3. **Public headers** (`-dev` package) — the *contract* the client compiles
   against. The only place where inline bodies, templates, macros, default
   arguments and `constexpr` values exist at all.

| ABI fact | dynsym | DWARF (libabigail) | headers (clang) |
|---|---|---|---|
| symbol added/removed/version-moved | ✓ | ✓ | partial¹ |
| function signature change (C) | — | ✓ | ✓ |
| C++ signature change (mangled delete+add) | ✓² | ✓ | ✓ |
| struct field added/removed/retyped | — | ✓ | ✓ |
| member offset / size change | — | ✓ (as emitted) | ✓ (as predicted)³ |
| vtable slot change | — | ✓ | needs `VTableContext`⁴ |
| enum case added/removed | — | ✓ | ✓ |
| inline/template body change | — | — | ✓ |
| uninstantiated template, default arg, macro | — | — | ✓ |
| public/private boundary | version nodes only | reconstructed by header-path suppression | native (it *is* the headers) |
| library implemented in Rust/Go with C API | ✓ | misleading⁵ | ✓ (the C header is the contract) |
| works without debug info | ✓ | ✗ | ✓ |
| ground truth for what shipped | ✓ | ✓ | prediction³ |

¹ Headers under-approximate the export set (symbols exported but not declared
publicly; version scripts) and over-approximate it (declared but not compiled
in, visibility attributes, `#ifdef`ed-out configurations).

² A mangled name encodes the signature, so delete+add of the same qualified
name is visible from dynsym alone — this is what the existing
`function_signature_changed` folding already exploits, and it needs no DWARF.

³ This is the crux of §2: a header parse *predicts* layout under one
preprocessor configuration; the binary *is* the layout. They diverge whenever
the package was built with `-D` flags the headers don't default to
(`_FILE_OFFSET_BITS=64`, `config.h` products, arch `#ifdef`s).

⁴ libclang's C API exposes `clang_Type_getSizeOf/getAlignOf/getOffsetOf`,
`clang_getOffsetOfBase`, `clang_Cursor_getMangling` /
`clang_Cursor_getCXXManglings` and virtual-method predicates — enough for
record layout and symbol prediction — but has **no vtable layout API**: no
slot ordering, no thunks, no key-function query. Verified against current
`clang-c/Index.h` (declarations at lines 3692–4784 of today's tree).

⁵ DWARF for a Rust `cdylib` or Go `c-shared` library describes Rust/Go
internals, not the C contract the client links against; a DWARF-only pipeline
either chokes or reports implementation churn as ABI churn. For these, the
shipped C header *is* the ABI surface — a point in favour of the header model
for the "any linkable package" goal.

The current pipeline already uses two of the three sources (DWARF + a header
*token* index for inline bodies/macros). The fidelity gap is that the header
side is a fingerprinter, not an ABI model, and that DWARF's availability gates
the whole corpus (§5.1.2b of METHODOLOGY.md: the dbgsym era bounds history to
2016+, halves usable pairs for slow-moving libraries, and — worse — is a
*selection bias*: we only study libraries whose maintainers ship dbgsym).

---

## 2. "Use libclang and evaluate the public ABI ourselves"

### What it would buy

* **No dbgsym dependency.** Any package with a `-dev` becomes analysable, the
  history extends past 2016, and the dbgsym selection bias disappears.
* **The native public/private boundary.** Today "public" is reconstructed by
  suppressing DWARF types whose declaring file falls outside the `-dev`
  include root — a heuristic with known failure modes (types declared in
  private headers but reachable from public ones, headers relocated between
  releases). In a header model, public is definitional.
* **The client-copied surface becomes first-class.** Inline bodies, templates,
  macros, default arguments and `constexpr` initializers — the material the
  `no_implicit_inlining` mechanism exists for — are only visible here. Today
  they are fingerprinted (changed/unchanged); a real model could classify
  *how* they changed.
* **Correct handling of non-C/C++ implementations** exposing a C API (see ⁵).

### What it would cost

* **Headers are a prediction, not ground truth.** The compiled binary reflects
  one preprocessor configuration chosen by the package's build system. A
  header-only evaluator must reproduce that configuration or it computes the
  layout of a library that was never shipped. Mitigations exist and are
  cheap-ish — `.pc` `Cflags` are in the extracted `-dev` tree, Debian's
  `debian/rules` and buildinfo files record flags, and the multiarch include
  dir carries the arch config — but the residual risk never reaches zero.
  **This is why DWARF must stay in the design as the calibrator: for every
  pair where both sources exist, predicted-vs-emitted layout disagreement is
  itself a metric** (and per §5.1.0 of METHODOLOGY.md, this study has already
  been burned once by an unvalidated header analysis: the lexical scanner
  over-reported inline churn 4×).
* **Parse fidelity becomes the load-bearing wall.** Every fatal-diagnostic
  translation unit silently under-counts. The existing coverage machinery
  (§2.4) helps, but the `-I`-guessing must be replaced by real flag recovery
  (`.pc` Cflags, `Build-Depends` `-dev` closure, a `-std=` ladder) before the
  header model can carry primary numbers rather than supplementary ones.
* **The Itanium rules are not weekend work.** Record layout with virtual
  bases, EBO, bit-field allocation; vtable construction with thunks, covariant
  returns, key functions; mangling with substitutions. libabigail and the
  compilers took years to converge. Reimplementing any of it over the libclang
  C API is exactly where new fidelity bugs would enter.

### The right way to "evaluate it ourselves": ask the compiler

The C API audit (⁴ above) says libclang alone reaches record layout and
mangled names but not vtables. Rather than reimplement, link clang's C++ AST
libraries and use the compiler's own ABI implementation:

* `clang::ASTContext::getASTRecordLayout` — field and base offsets, size,
  alignment, exactly as clang the compiler lays them out (this is what
  `-fdump-record-layouts` prints);
* `clang::ItaniumVTableContext` / `VTableContext` — complete vtable layouts
  including thunks and the component ordering (what `-fdump-vtable-layouts`
  prints);
* `clang::ItaniumMangleContext` — the mangler itself, so the predicted export
  set is computed by the same code that computes real ones.

Tradeoffs of the C++ API: it is unstable across LLVM majors (pin the version
in the Docker image — already the practice here), the link is heavier, and the
code must be insulated behind our own small interface (one `clang_abi.cpp`
that produces our `abi::model` types, so an LLVM bump touches one file). In
exchange, the "evaluate the ABI ourselves" plan degenerates into "extract what
the production compiler already computed", which is the highest-fidelity
oracle available on any platform.

### A cheap intermediate with outsized leverage: the probe-TU trick

Before (or instead of) building the full clang-AST evaluator, there is a
two-week-sized move that reuses the entire existing diff engine:

1. Generate a probe translation unit per release that `#include`s every
   public header (per-file language and flag recovery as above), plus
   explicit instantiations for public templates where feasible.
2. Compile it with `-g -Og -fno-eliminate-unused-debug-types -fstandalone-debug`.
3. Feed the resulting object's DWARF to the **existing** `abi::compare` —
   libabigail neither knows nor cares that the DWARF came from a probe rather
   than a dbgsym.

This is the architecture of `abi-dumper` + `abi-compliance-checker`'s DWARF
mode, inverted: they dump shipped binaries; we additionally compile the
*declared* API to DWARF. It removes the dbgsym gate for type-level analysis
at the cost of the prediction caveat (³), keeps every existing filter and the
calibration suite, and produces DWARF for declared-but-uninstantiated
entities that shipped binaries never contain. Its known limits: templates
appear only if instantiated (hence the explicit-instantiation probes), and a
header set that doesn't compile standalone yields nothing for the affected
TU — so parse-fidelity work is a prerequisite here too.

---

## 3. Can we lift code from libabigail?

**Legally: yes.** libabigail relicensed from LGPLv3 to **Apache-2.0 WITH
LLVM-exception** (completed for the 2.x series, with all contributors signed
off), the same license as LLVM/clang. Vendoring fragments into this tool is
unencumbered.

**Practically: mostly no — link it, don't vendor it.** The valuable parts are
entangled with the IR:

* `abg-ir.cc` (the type system and canonical-type machinery) is tens of
  thousands of lines with global-ish environment state — the reason `diff`
  already runs in a child process per pair. Fragments do not compile
  standalone.
* `abg-dwarf-reader.cc` is the single most expensive-to-rewrite component in
  this whole space, and linking it (as now) already captures its value.
* What **is** realistically liftable:
  * `abg-elf-helpers` / the symtab reader — though `libelf` directly is
    comparable effort; only worth it for the alias/ifunc/version-node edge
    cases it already handles.
  * **The knowledge, not the code, in `abg-comp-filter.cc`** — the accumulated
    taxonomy of which diffs are harmless (compatible typedef changes,
    anonymous-member reshuffles, CV-qualifier churn…). Port it as a checklist
    into our classifier and as negative cases into the calibration suite.
    This is the highest value-per-hour lift available, and it transfers to
    the header-model classifier where libabigail's code never could.
  * The suppression-spec *format* (already used via
    `gen_suppr_spec_from_headers`).

The same logic applies to clang with opposite sign: from clang we want the
*code* (layout, vtables, mangling — by linking, which the LLVM exception makes
frictionless), because there the code is the compiler's ground truth; from
libabigail we want the *judgment* (what constitutes a harmless diff), because
that encodes a decade of corpus experience.

Prior art worth mining for judgment rather than code:
`abi-compliance-checker` (the original header-based mode — 20 years of
header-diff edge cases, but Perl), `abi-dumper` (DWARF dumping conventions),
and the Swift `swift-api-digester` (the closest existing tool to this study's
resilience framing).

---

## 4. Tradeoffs, condensed

| | libabigail over dbgsym DWARF (today) | libclang C API, own evaluator | clang C++ AST libs evaluator | probe-TU DWARF → libabigail |
|---|---|---|---|---|
| ground truth for shipped layout | **yes** | no (prediction) | no (prediction) | no (prediction) |
| needs debug info packages | yes — gates corpus | no | no | no |
| sees inline/template/macro surface | no | yes | **yes, incl. layout of header-only types** | partial (instantiated only) |
| vtable fidelity | as emitted | ✗ (no C API) | compiler's own builder | as compiled from headers |
| public boundary | heuristic suppression | native | native | native |
| Rust/Go-implemented C libraries | poor | good | good | good |
| new code to trust | none | **most** (Itanium reimpl.) | glue only | glue + TU generator |
| API stability of dependency | good (C++ but slow-moving) | excellent (stable C API) | poor (pin LLVM) | good |
| reuses existing diff engine + calibration | — | no | partially (model-level) | **fully** |
| rough effort | — | months, high defect risk | weeks | ~2 weeks + parse-fidelity work |

---

## 5. What "any published Linux library, C/C++-linkable" demands

1. **Tiered evidence, never a gate.** Analyse every pair at the deepest tier
   available instead of rejecting candidates at `select`:
   * **Tier A — dynsym only** (universal): symbol add/remove/version events,
     C++ signature changes via mangled names, mass-rename detection. Note
     libabigail reads symbol-only corpora without DWARF, so this tier is
     nearly free through the existing dependency.
   * **Tier B — + header model** (any `-dev`): types, layout prediction,
     enums, inline/template/macro surface.
   * **Tier C — + DWARF** (dbgsym, legacy `-dbg`, or **debuginfod** — the
     `debuginfod.debian.net` service serves debug info by build-id even for
     packages/eras without dbgsym debs, and generalises to Fedora/Ubuntu
     later): emitted layout, and the calibration of Tier B.
   Every reported number carries its tier; corpus-level statements are made
   per-tier, so the dbgsym selection bias becomes visible instead of silent.
2. **Flag recovery before header parsing** (the current §5.1.1 fix, now
   promoted to a prerequisite): `.pc` `Cflags` from the extracted tree, the
   `Build-Depends` `-dev` closure materialised onto the include path, a
   `-std=` ladder on fatal diagnostics, per-file language choice. Coverage
   stays a first-class output.
3. **Cross-validation as a product feature.** For every Tier-C pair, compare
   header-predicted layout/exports against DWARF/dynsym reality and publish
   the disagreement rate. This converts the header model's biggest weakness
   into a measured, improvable quantity — and it is the corpus-scale version
   of the calibration suite, which ground-truth synthetic cases cannot
   provide (§5.1.0 showed why: two implementations passed the same 30 cases
   and differed 4× on the corpus).
4. **Symbol-table completeness**: version scripts and version-node moves,
   weak symbols, ifunc, TLS, aliases — the dynsym tier must classify these
   explicitly rather than fold them into add/remove noise.
5. **Language detection must not assume Itanium**: keep the mangled-fraction
   heuristic for C vs C++, but detect Rust/Go implementation (e.g. `.comment`
   section, `rust_eh_personality`) and route those pairs header-first.

---

## 6. Recommendation and phasing

Keep libabigail as the DWARF engine and the existing classifier as the single
taxonomy. Build outward:

1. **Tier A fallback + debuginfod** (days): stop rejecting candidates without
   dbgsym; symbol-level analysis for everyone; try debuginfod before giving up
   on DWARF. Largest corpus-fidelity win per unit effort — it attacks the
   selection bias and the 2016 horizon simultaneously.
2. **Flag recovery for the header indexer** (days): `.pc` Cflags,
   Build-Depends closure, `-std` ladder. Prerequisite for everything
   header-shaped; immediately improves the numbers already being reported.
3. **Probe-TU DWARF** (≈2 weeks): type-level diffs without dbgsym, through
   the existing, calibrated engine. Ship the Tier B/Tier C disagreement
   metric with it.
4. **clang C++ AST evaluator** (weeks): replace the token-fingerprint header
   index with a real model — `ASTRecordLayout`, `VTableContext`,
   `ItaniumMangleContext` — behind one insulation file, producing the same
   `abi::model` events. This is the step that makes the tool *better than*
   the existing ecosystem (per-entity classification of the client-copied
   surface, which no DWARF tool can do), not just equal to it.
5. **Port libabigail's harmless-diff judgment** into the classifier and the
   calibration suite as negative cases (ongoing, alongside 4).

At the end of this, "the best ABI diff tool for any published Linux library"
is concretely: *dynsym truth for symbols, compiler-computed header model for
the contract, DWARF for what shipped, one taxonomy over all three, and the
disagreements between them reported as data.* No single-source tool —
libabigail, abi-compliance-checker, or a from-scratch libclang evaluator —
can make that claim.
