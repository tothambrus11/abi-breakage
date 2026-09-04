# Related work: ABI stability, ecosystem-scale ABI analysis and symbol versioning

A survey of the prior work this study builds on, grouped by what each strand
can tell us, with a closing section on what nobody has measured yet. It
covers natively compiled languages (C, C++, Rust, Swift, Go) and, as a
baseline for methods, the managed-language ecosystems where most empirical
work on breaking changes has been done. Every entry ends with a note on how
it relates to `METHODOLOGY.md` / `RESULTS.md`.

## 1. Mechanisms and platform policies

The mechanisms a native library has for evolving without breaking compiled
consumers, and the policies platforms built on them.

**ELF symbol versioning.** Introduced in Solaris and adopted by the GNU
toolchain, symbol versioning lets one shared object carry several versions
of one symbol under named version nodes; a consumer binds to the node it
was linked against. Drepper's *How To Write Shared Libraries* is the
canonical guide to using it, together with visibility control and SONAME
rules. glibc has used it to keep `libc.so.6` since glibc 2.0 while changing
incompatible interfaces many times; DPDK formalised the same approach into
a written ABI policy with deprecation windows. *Relation:* the study
records a symbol that moves to a new version node as
`symbol_version_renamed`, so a library using this mechanism correctly shows
neither a break nor a bump (SONAME.md §2).

**SONAME conventions.** Debian Policy §8.1, libtool's
`current:revision:age`, Fedora's and openSUSE's shared-library packaging
policies, libcurl's and Qt's project policies. Collected and compared in
`SONAME.md`; the one-line summary is that every convention is phrased in
terms of removed or re-signed symbols, leaving layout changes of exposed
types to be inferred. *Relation:* the declared/silent split in RESULTS §3
measures adherence to these conventions.

**Kernel ABI (kABI).** The Linux kernel derives a CRC per exported symbol
from the preprocessed source (`genksyms`, `CONFIG_MODVERSIONS`) and, since
6.13, optionally from DWARF (`gendwarfksyms`); a module whose CRCs differ
from the running kernel's is refused. Red Hat's kABI stablelists and
Android's kernel ABI monitor (built on libabigail) turn that into a policy:
a subset of symbols whose CRC must not change within a stable series. The
kernel's own position on in-kernel interfaces is Kroah-Hartman's *Stable
API Nonsense*: no promise inside the kernel, a strong one to userspace.
*Relation:* kABI is the one widely deployed native mechanism that detects
layout changes automatically, because the CRC covers the types reachable
from a symbol; user-space libraries have nothing equivalent, which is why
the study needs DWARF comparison to see what a SONAME cannot express.

**Android bionic.** The NDK's libc ABI is a version script with per-API-level
annotations (`libc.map.txt`); symbols appear at an API level and are never
removed, and the linker enforces that an app targeting level N sees only
level-N symbols. *Relation:* an additive-only policy enforced by tooling,
the model the lenient definition assumes for symbols.

**Apple TAPI / text-based stubs.** Apple ships `.tbd` text stubs instead of
binaries in SDKs; the stub lists exported symbols, Swift ABI version and
availability, and the linker links against it. `llvm-ifs` provides the
same for ELF. *Relation:* a stub is a declared ABI surface, the
declared-symbol join (METHODOLOGY §3.4) approximates one from headers.

**Swift library evolution.** The ABI Stability Manifesto, the *Library
Evolution* design document and SE-0260 define resilience: a library built
with `-enable-library-evolution` may add stored properties, reorder them,
add enum cases and add virtual methods without breaking clients, because
clients access layout through metadata and dispatch through witness
tables; `@frozen` opts a type out for performance, SE-0487 extends the
model to extensible enums outside the standard library. Swift 5 made the
standard library ABI-stable on Apple platforms. *Relation:* the mechanism
map (`mechanism_for`, REVIEW §1.2) is taken from this model; RESULTS §4 is
the counterfactual "how many C/C++ breaks would resilience have absorbed".

**C++.** The Itanium C++ ABI fixes name mangling, vtable layout and class
layout for the platform; libstdc++'s dual ABI (GCC 5, `_GLIBCXX_USE_CXX11_ABI`)
is the one large, deliberate C++ standard-library ABI break and shows what
one costs. In WG21, Winters's P1863 *ABI: Now or Never* and P2028 *What is
ABI, and what should WG21 do about it?* (with the P1654 comment summary)
framed the 2020 Prague decision not to commit to an ABI break: the
committee neither promises stability nor plans to break it. *Relation:* the
C++ half of the corpus lives under this ambiguity; the strict/lenient
bracket exists because the standard itself does not say which changes
count.

**Rust.** The Rust reference states that the default (`"Rust"`) ABI is
unstable; stable dynamic linking is done through `extern "C"` and crates
such as `abi_stable` and `stabby` that reconstruct a versioned ABI on top
of it, and RFC 3435 (crABI) proposes a per-function stable ABI. *Relation:*
Rust libraries in Debian are statically linked or vendored, so they do not
appear in a shared-library corpus at all; the study says nothing about
Rust ABI, and §5 lists it as an open measurement.

**Go.** The Go 1 compatibility promise plus semantic import versioning
(major version in the import path) make incompatible majors distinct
packages by construction, the same effect Debian achieves with separate
source packages. *Relation:* both hide major bumps from a
consecutive-version corpus (RESULTS §5.2).

**KDE binary-compatibility policy and the d-pointer.** KDE's *Binary
Compatibility Issues With C++* is the most complete practitioner list of
what a C++ library may and may not change; its central idiom, the private
implementation pointer, is a hand-rolled form of opaque layout.
*Relation:* the `by_pointer` exposure class is exactly the type shape this
idiom produces.

**Hyrum's law.** Winters's formulation in *Software Engineering at Google*
(ch. 1, ch. 21 on dependency management, and the Abseil compatibility
guidelines): with enough users every observable behaviour is depended on,
whatever the contract says. *Relation:* the undeclared stratum is Hyrum's
law made measurable at the symbol level: exported internals that some
consumer may bind to.

## 2. Tools for detecting ABI and API change

**ABI Compliance Checker, ABI Dumper, ABI Tracker.** Ponomarenko and
Rubanov's tool family compares two versions of a C/C++ library from headers
plus binaries (or DWARF dumps) and reports removed symbols, changed
signatures, vtable and layout changes with a severity per change; their
2012 paper *Backward compatibility of software interfaces: steps towards
automatic verification* and the earlier *Automated verification of shared
libraries for backward binary compatibility* describe the model. ABI
Tracker and the Upstream Tracker produced per-library timelines for
hundreds of libraries, and openSUSE's Hack Week project explored running
ACC over the Build Service. *Relation:* the closest predecessor in intent.
The difference is that the trackers are per-library dashboards, not a
sampled corpus with per-kind rates and intervals; and ACC's severity is a
single scale, where the study keeps two definitions and the exposure and
declaration facts that separate them.

**libabigail.** Seketeli's framework builds a typed IR of a binary from
DWARF (and, since 2.2 and 2.3, CTF and BTF) and diffs two IRs;
`abidiff`, `abipkgdiff` and `abicompat` are its front ends, used by Red
Hat's release process and Android's kernel ABI monitor. *Relation:* the
study's comparer is written against the libabigail library rather than
around `abidiff` output; `FIDELITY.md` audits what it can and cannot see
and why a clang-AST evaluator is the next step.

**Debian symbols files.** `dpkg-gensymbols` records, per exported symbol,
the first package version providing it, so dependencies are generated per
symbol rather than per SONAME and a removed symbol fails the build.
*Relation:* symbols files are the distribution's declared ABI at the
symbol level and the natural ground truth for the declared/undeclared
stratum in future work; they do not see layout.

**Rust: `cargo-semver-checks` and `rust-semverver`.** Lints over rustdoc
JSON that flag API changes requiring a major bump; the 2024h2 and 2026
Rust project goals track merging the checker into cargo. *Relation:*
source-level, no binary component; the same structural checks the header
indexer performs for C/C++ minus layout.

**Java corpora and checkers.** Jezek and Dietrich's *API Evolution and
Compatibility: A Data Corpus and Tool Evaluation* (JOT 2017) built a
synthetic corpus of "all" syntactic API changes and evaluated nine
checkers against it; Roseau (2025) is a recent source-based checker
evaluated the same way. *Relation:* the 34-case calibration suite in
`tests/calibration` is the same idea for C/C++ at binary level; no
published corpus of that kind exists for native ABI checkers.

**Compiler-level ABI conformance.** *Mix Testing* (OOPSLA 2024) specifies
and tests ABI compatibility of C/C++ atomics across compilers, a reminder
that the compiler is part of the ABI; *ABI compatibility through a
customizable language* (GPCE 2010) is an earlier attempt to make ABI
constraints explicit in the language. *Relation:* the study holds the
compiler fixed by construction (Debian's build of each release) and so
measures library evolution only.

**Dependency-level detection.** DepOwl (ICSE 2021) collects backward- and
forward-incompatible changes between successive library versions and checks
applications' declared dependencies against them; on Ubuntu 19.10 it found
77 dependency bugs that could lead to compatibility failures. *Relation:*
the consumer side of the same question; the study measures the producer
side and lists client impact as future work.

## 3. Empirical studies of breaking changes and versioning

**Java, the reference results.** Raemaekers, van Deursen and Visser (SCAM
2014, JSS 2017) analysed over 100 000 jars from Maven Central and found
that about one third of releases introduce at least one breaking change,
at the same rate for minor and major releases. Ochoa, Degueule, Falleri and
Vinju's replication (EMSE 2022) confirmed the frequency with a refined
detector and added that most breaking changes do not affect any client
that actually uses the library. Dietrich, Jezek and Brada's *Broken
promises* (CSMR-WCRE 2014) studied evolution problems from partial library
upgrades, and Lam, Dietrich and Pearce (Onward! 2020) argued for
behavioural rather than syntactic notions of compatibility. *Relation:*
the design of this study (consecutive releases, per-kind rates, release
level as a covariate, a rescue analysis) mirrors these; the headline
finding is comparable in shape (RESULTS §3: 16 % of patch releases break
binary compatibility under the strict definition).

**Go.** Li, Wu, Fu and Zhou (ASE 2023) built a dependency graph of 124 K
libraries and 532 K clients; 86.3 % of upgrades were SemVer-compliant,
28.6 % of non-major upgrades introduced breaking changes, and 33.3 % of
clients could be affected. *Relation:* same shape of result for a compiled
language with static linking; no ABI dimension.

**Rust.** Li et al.'s study of yanked releases (arXiv 2201.11821) found
SemVer breaks the most common stated reason for yanking; Gruevski's
measurements with `cargo-semver-checks` (FOSDEM 2024) put accidental SemVer
violations at a lower bound of about 3 % of releases and one in six of the
top 1000 crates. *Relation:* API-level only; Rust has no ABI to break in
the shared-library sense.

**Linux distributions.** The C/C++ evidence is thin. Ponomarenko's trackers
provide per-library timelines without aggregate statistics; DepOwl
measures dependency bugs on one Ubuntu release; distribution bug trackers
hold the case law (SONAME.md §3). Chen, Bi, Wang and Thongtanunam's
systematic review of 97 breaking-change studies (arXiv 2605.24397, 2026)
covers Maven, npm, Python, web APIs and Linux distributions and names
behavioural break detection at scale, SemVer's failure as a trust
mechanism and transitive propagation as open problems; binary
compatibility and ABI do not appear in its scope. *Relation:* the study
fills the ecosystem-scale, binary-level gap the review does not cover.

## 4. What this study adds

Read against §1–§3, the contributions are:

1. **Per-kind rates with uncertainty for a distribution-scale corpus.** 739
   consecutive-release transitions of 109 libraries, each change kind with
   a cluster-bootstrap interval over libraries (RESULTS §2). Prior native
   work reports per-library timelines or single-release dependency bugs.
2. **Two break definitions with the facts that separate them.** Exposure
   (by value, by pointer, not in interface), append-only growth, and the
   declared/undeclared/undecidable symbol strata (METHODOLOGY §4). ACC and
   abidiff report a single severity; the Java literature has no
   binary-layout analogue.
3. **Declared versus silent breaks.** The share of breaks accompanied by a
   SONAME change (8 of 162 strict, 6 of 106 lenient), and the reasons
   (SONAME.md).
4. **Header-level churn no ABI tool sees.** Inline-function, template and
   macro changes indexed from the shipped `-dev` headers
   (`SUPPLEMENT_INLINE.md`).
5. **A counterfactual resilience analysis.** Which breaks a Swift-style
   boundary would have absorbed (RESULTS §4).
6. **A calibration corpus for native ABI checkers**, 34 synthetic libraries
   with known ground truth, in the spirit of Jezek and Dietrich.

## 5. Research gaps

Gaps the literature and this study leave open, ordered by how directly the
existing pipeline could address them.

* **Client impact.** Every native study so far, this one included, measures
  the producer side. Which consumers in the archive actually bind to the
  changed symbol or allocate the changed type by value is measurable from
  the reverse dependencies' binaries (the L0–L4 linkability lattice of
  `FIDELITY.md` §8; Ochoa et al. did it for Java) and would turn the strict/
  lenient bracket into a point estimate.
* **Ground truth for the strata.** Debian symbols files and `-dev` headers
  can be joined to decide *declared* mechanically for every package that
  ships them; the study joins headers only. A cross-check of the two would
  bound the false-negative rate of the header join (feature-macro-gated
  declarations).
* **Major releases and sibling source packages.** Debian and Go both hide
  incompatible majors behind new package names; a corpus that follows
  `libfoo2` → `libfoo3` would recover exactly the releases authors declare
  incompatible, which are 14 of 739 transitions here.
* **Other ecosystems.** Fedora, Homebrew, conda-forge and vcpkg ship the
  same upstreams under different build conventions; a cross-distribution
  replication would separate library evolution from packaging policy.
* **Symbol versioning in practice.** No measurement exists of how many
  user-space libraries use version scripts, how many use them to preserve
  old symbols, and whether those libraries break less. The corpus already
  records version nodes per symbol.
* **Compiler-induced ABI change.** Mix Testing and the dual-ABI episode show
  the compiler is part of the ABI; a study of the same library rebuilt
  across compiler releases (Debian's binNMUs are natural experiments)
  would separate it from library evolution.
* **Behavioural compatibility.** Every detector, native or managed, stops
  at syntax and layout; Lam et al. and the 2026 review both name this the
  largest open problem, and it is where the Claude-assisted profiling stage
  proposed in `FIDELITY.md` §7 could add evidence.
* **Rust and Swift ecosystems.** Rust has no stable ABI and Swift's
  resilience is measured by nobody outside Apple; a study of Swift
  packages on Linux, or of `abi_stable`/`stabby` users, would test whether
  resilience mechanisms change the rates observed here.
* **Author intent.** SONAME.md reconstructs why authors do not bump from
  bug trackers; a survey or interview study of maintainers of the libraries
  that break most (RESULTS §5.4) would replace inference with evidence.
* **Tool accuracy at scale.** The 34 calibration cases are synthetic; an
  annotated sample of real transitions, judged by hand, would give the
  precision and recall of libabigail-based classification that the
  intervals in RESULTS assume to be 100 %.

## Sources

Mechanisms and policies
* Ulrich Drepper, [How To Write Shared Libraries](https://cs.dartmouth.edu/~sergey/cs258/ABI/UlrichDrepper-How-To-Write-Shared-Libraries.pdf)
* DPDK, [ABI Versioning](https://doc.dpdk.org/guides/contributing/abi_versioning.html)
* Linux kernel, [DWARF module versioning (gendwarfksyms)](https://docs.kernel.org/kbuild/gendwarfksyms.html); Red Hat, [What is kABI?](https://access.redhat.com/solutions/444773); Android, [Kernel ABI monitoring](https://source.android.com/docs/core/architecture/kernel/abi-monitor)
* Android NDK, [Platform APIs](https://android.googlesource.com/platform/ndk/+/refs/heads/ndk-release-r21/docs/PlatformApis.md)
* Apple, [TAPI](https://github.com/apple-oss-distributions/tapi) and the [TBD format](https://github.com/apple-opensource/tapi/blob/master/docs/TBD.rst); LLVM, [llvm-ifs](https://llvm.org/docs/CommandGuide/llvm-ifs.html)
* Swift, [ABI Stability Manifesto](https://github.com/apple/swift/blob/main/docs/ABIStabilityManifesto.md), [Library Evolution](https://github.com/swiftlang/swift/blob/main/docs/LibraryEvolution.rst), [SE-0260](https://github.com/swiftlang/swift-evolution/blob/main/proposals/0260-library-evolution.md), [SE-0487](https://github.com/swiftlang/swift-evolution/blob/main/proposals/0487-extensible-enums.md), [ABI Stability and More](https://www.swift.org/blog/abi-stability-and-more/)
* WG21, [P1863R1 ABI: Now or Never](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p1863r1.pdf), [P2028](https://github.com/cplusplus/papers/issues/759), [P1654R0 ABI breakage: summary of initial comments](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p1654r0.html), [Prague 2020 record of discussion](https://open-std.org/JTC1/SC22/WG21/docs/papers/2020/p2130r0.pdf)
* Rust, [Application binary interface (reference)](https://doc.rust-lang.org/reference/abi.html), [abi_stable](https://crates.io/crates/abi_stable), [stabby](https://github.com/ZettaScaleLabs/stabby)
* Winters, [Software Engineering at Google, ch. 21 Dependency Management](https://abseil.io/resources/swe-book/html/ch21.html); [Hyrum's Law](https://www.hyrumslaw.com/)
* Debian Policy, [§8 Shared libraries](https://www.debian.org/doc/debian-policy/ch-sharedlibs.html); GNU libtool, [Updating version info](https://www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html); Fedora, [Packaging tricks](https://fedoraproject.org/wiki/Packaging_tricks)

Tools
* Ponomarenko & Rubanov, [Backward compatibility of software interfaces: steps towards automatic verification](https://link.springer.com/article/10.1134/S0361768812050052) (Programming and Computer Software, 2012); [Automated verification of shared libraries for backward binary compatibility](https://www.researchgate.net/publication/232646193_Automated_Verification_of_Shared_Libraries_for_Backward_Binary_Compatibility)
* [ABI Compliance Checker](https://lvc.github.io/abi-compliance-checker/), [ABI Tracker](https://github.com/lvc/abi-tracker), [kernel-abi-tracker](https://github.com/lvc/kernel-abi-tracker); SUSE Hack Week, [Automation of ABI compatibility checks](https://hackweek.opensuse.org/all/projects/automation-of-abi-compatibility-checks)
* Seketeli, [Libabigail: semantic analysis of C and C++ ELF binaries](https://events.opensuse.org/conferences/oSC17/program/proposal/1234) (openSUSE Conference 2017); [ABI analysis using BTF, CTF and DWARF](https://lpc.events/event/18/contributions/1923/attachments/1487/3146/abi-analysis-using-btf-ctf-dwarf-2024-lpc.pdf) (Linux Plumbers 2024); Red Hat, [How to write an ABI compliance checker using libabigail](https://developers.redhat.com/blog/2020/04/02/how-to-write-an-abi-compliance-checker-using-libabigail)
* Debian, [dpkg-gensymbols](https://manpages.debian.org/testing/dpkg-dev/dpkg-gensymbols.1.en.html), [UsingSymbolsFiles](https://wiki.debian.org/UsingSymbolsFiles)
* [cargo-semver-checks](https://crates.io/crates/cargo-semver-checks), [rust-semverver](https://github.com/rust-lang/rust-semverver), Rust project goal [merging cargo-semver-checks into cargo](https://rust-lang.github.io/rust-project-goals/2024h2/cargo-semver-checks.html)
* Jezek & Dietrich, [API Evolution and Compatibility: A Data Corpus and Tool Evaluation](https://www.jot.fm/issues/issue_2017_04/article2.pdf) (JOT 2017); [Roseau](https://arxiv.org/pdf/2507.17369) (2025)
* [Mix Testing: Specifying and Testing ABI Compatibility of C/C++ Atomics Implementations](https://dl.acm.org/doi/10.1145/3689727) (OOPSLA 2024); [ABI compatibility through a customizable language](https://dl.acm.org/doi/10.1145/1868294.1868316) (GPCE 2010)
* Jia et al., [DepOwl: Detecting Dependency Bugs to Prevent Compatibility Failures](https://arxiv.org/pdf/2102.08543) (ICSE 2021)

Empirical studies
* Raemaekers, van Deursen & Visser, [Semantic versioning versus breaking changes](https://dl.acm.org/doi/10.1109/SCAM.2014.30) (SCAM 2014) and [Semantic versioning and impact of breaking changes in the Maven repository](https://dl.acm.org/doi/10.1016/j.jss.2016.04.008) (JSS 2017)
* Ochoa, Degueule, Falleri & Vinju, [Breaking bad? Semantic versioning and impact of breaking changes in Maven Central](https://link.springer.com/article/10.1007/s10664-021-10052-y) (EMSE 2022)
* Dietrich, Jezek & Brada, [Broken promises: an empirical study into evolution problems in Java programs caused by library upgrades](https://ieeexplore.ieee.org/document/6747226/) (CSMR-WCRE 2014); Lam, Dietrich & Pearce, [Putting the semantics into semantic versioning](https://dl.acm.org/doi/10.1145/3426428.3426922) (Onward! 2020)
* Li, Wu, Fu & Zhou, [A Large-Scale Empirical Study on Semantic Versioning in Golang Ecosystem](https://arxiv.org/abs/2309.02894) (ASE 2023)
* Li et al., [An Empirical Study of Yanked Releases in the Rust Package Registry](https://arxiv.org/pdf/2201.11821); Gruevski, [SemVer in Rust: Tooling, Breakage, and Edge Cases](https://predr.ag/blog/semver-in-rust-tooling-breakage-and-edge-cases/) (FOSDEM 2024)
* Chen, Bi, Wang & Thongtanunam, [Breaking Changes in Software Ecosystems: A Systematic Literature Review](https://arxiv.org/abs/2605.24397) (2026)
* Stenberg, [Eighteen years of ABI stability](https://daniel.haxx.se/blog/2024/10/30/eighteen-years-of-abi-stability/); libcurl, [ABI policy](https://curl.se/libcurl/abi.html)
