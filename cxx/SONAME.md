# When is a SONAME bump expected, and why authors skip it

Background research for the *declared vs silent* split in RESULTS.md §3. A
break is "declared" there when the SONAME of a paired shared object changed
between the two releases; everything else is "silent". This note collects
what the conventions actually require, what mechanisms exist besides a
bump, and the reasons library authors give, or reveal, for not bumping when
their change breaks compiled consumers.

## 1. What the mechanism is

Two parties are involved throughout this note. *Upstream* is the project
that writes and releases the library (the libcurl developers, the spdlog
maintainers, the Samba team for libtdb); it sets the SONAME in its own
build system (libtool `-version-info`, CMake `SOVERSION`), and the value is
baked into the file as `DT_SONAME` when a release is built. *Downstream*
is whoever repackages that release for a distribution, here the Debian or
Fedora packager who turns the tarball into `libfoo7`.

The dynamic linker records the library's `DT_SONAME` in every consumer's
`DT_NEEDED` and, at load time, does a plain string-equality lookup: it
neither checks nor understands ABI compatibility. Fedora's guideline states
this directly and draws the consequence for the downstream packager: run an
ABI comparison tool, and if upstream shipped an incompatible release under
the old SONAME, change the SONAME in the distribution's own build. A
SONAME change therefore does exactly one thing: it lets the old and the new
library coexist on one system so that binaries linked against the old one
keep loading it, while packages get rebuilt against the new one at their
own pace. That is why distributions couple the SONAME to the *package name*
(`libfoo7` → `libfoo8`): the Debian Library Packaging Guide and Policy §8.1
both make the package rename the visible half of a bump.

## 2. What the conventions require

| convention | bump required when | explicitly not required when |
|---|---|---|
| Debian Policy §8.1 | "any time an interface is removed from the shared library or the signature of an interface ... is changed" | "if new interfaces are added but none are removed or changed" |
| libtool `current:revision:age` | interfaces removed or changed: `current++`, `age = 0` (SONAME major = `current − age`, which changes) | interfaces added only: `current++`, `age++` (major unchanged) |
| Fedora | any ABI-incompatible change; in a stable release only for security or grave bugs, otherwise Rawhide; libraries without an upstream SONAME get a packager-chosen `0.x.y` | additions |
| libcurl project policy | "whenever there are changes done to the library that causes an ABI breakage"; in practice they avoid breaks altogether (last bump: 7.16.0, 2006) | additions, new options |
| Qt | major version only; minor releases promise binary compatibility, patch releases both ways | anything inside a major series, by construction |
| glibc | never (`libc.so.6` since glibc 2.0); every incompatible change is handled by symbol versioning | n/a |

Two things stand out. First, the written rules are phrased in terms of
*symbols*: removed interfaces, changed signatures. Policy §8.1 says
"normally, this means", leaving layout changes of exposed types, which are
the most frequent binary break in RESULTS.md §2, to be inferred. Second,
the mainstream alternative to bumping is not "do nothing" but **symbol
versioning**: keeping the old symbol under its old version node and adding
the new behaviour under a new one, so that one SONAME serves both old and
new consumers (Drepper's *How to Write Shared Libraries*; glibc; DPDK's
ABI policy). The study counts a symbol that moves to a new version node as
`symbol_version_renamed`, not as a removal, so a library using this
mechanism correctly shows no break and no bump: that is the intended
outcome, not a silent break.

## 3. Reasons authors do not bump after an ABI break

The cases below are documented breaks that shipped under an unchanged
SONAME. They fall into recognisable categories, and each category maps
onto one of the strata the study measures.

**They did not consider the interface public.** libtdb removed six
exported functions without a bump; the maintainer's answer was that
"except for `tdb_logging_function` none of these symbols were ever intended
to be exported by upstream, and I'm not aware of any users". The symbols
existed because the build exported everything. This is the study's
*undeclared* stratum (2 056 of the removed public symbols were never
declared in the shipped headers), and it is exactly what the lenient
definition discounts. The same reasoning covers struct fields: svt-av1
removed a field from the middle of `EbSvtAv1EncConfiguration` in 1.6.0
and shipped it as `libSvtAv1Enc.so.1`.

**They did not know it was a break.** Layout changes are invisible unless
someone runs an ABI checker; the conventions talk about removed functions
and changed signatures, and a maintainer who appends a field to a struct
that consumers allocate by value has not done either of those things in
the wording of Policy §8.1. libtool's `current:revision:age` arithmetic is
a well-known source of wrong SONAMEs on its own (the on-disk major is
`current − age`, not `current`). Semantic versioning adds a second
confusion: a minor version bump promises API compatibility, and authors
read that as ABI compatibility. spdlog broke its ABI between 1.9 and 1.10
by adding a constructor parameter; the project had started header-only,
where ABI is not a concept, and only after the Debian bug did upstream
decide to use `major.minor` as the SOVERSION from 1.11 on. Debian bumped
the SONAME on its own for 1.10 in the meantime.

**Their stability promise is narrower than the SONAME implies.** Some
projects promise compatibility only across major versions and expect
consumers to be rebuilt on every minor release. srt kept `libsrt.so.1`
across ABI-incompatible 1.4.x releases (the reporter's example is fields
added to `CBytePerfMon`), and the request to bump per minor release was
closed with a pull request rather than a policy change. This is what
intel-gmmlib looks like in RESULTS.md §5.4: nine transitions, nine strict
breaks, one SONAME. For such libraries the SONAME is a product name, not a
compatibility statement.

**A bump is expensive and visible; a silent break may never bite.** A
SONAME change forces a distribution transition: the package is renamed,
every reverse dependency is rebuilt and migrated together, and the
maintainer hears about every failure. libcurl's own account of its 3 → 4
bump in 2006 is that "the obvious friction this bump caused made a huge
impact" and led the project to put ABI stability at the top of its
priorities. The asymmetry is real: a bump costs every consumer and the
author immediately; a silent break costs only the consumers that actually
use the changed interface, and only when they upgrade the library without
rebuilding. Authors who believe the changed interface is rarely used
rationally skip the bump. This is also why distributions sometimes take
the cost on themselves instead: Debian's answer to svt-av1 was a
Debian-specific package suffix with `Breaks`/`Replaces`, to tdb a symbols
file plus a compatibility shim for the one genuinely public symbol.

**The SONAME is one scalar for the whole library.** A break confined to a
rarely used interface, or to one of several shared objects in a package,
still requires the whole library, and every consumer, to transition. There
is no way to declare "broken for the 2 % of consumers that touch this
struct". Symbol versioning is the fine-grained tool, but it needs a version
script, a compatibility implementation of the old entry point, and an
understanding of the mechanism; most small libraries have none of the
three.

**Nothing in the release process checks.** A SONAME is set in a build
file (libtool `-version-info`, CMake `SOVERSION`), usually once, and by
convention set to the major version regardless of what changed. Unless an
ABI checker runs in CI, the person tagging a release has no signal that a
field moved three commits ago. On Windows and macOS the SONAME mechanism
does not exist in this form, so cross-platform projects have no
platform-independent habit that would produce one.

**They already keep it working by other means.** glibc, DPDK and others
never bump because every incompatible change carries a new symbol version
and a compatibility implementation; libcurl never bumps because it does not
break. In both cases "no bump" is the *correct* observation and the study
reports no break for them.

## 4. What this means for the declared/silent numbers

* In RESULTS.md, 8 of 162 strict binary breaks and 6 of 106 lenient ones
  coincide with a SONAME change. Under the conventions above, every
  lenient break (removal or re-signing of a *declared* symbol, layout
  change of a by-value type) is a case where Policy §8.1 or libtool would
  have required a bump; the declared share among them is the fraction of
  breaks for which the author followed the convention.
* The strict-only breaks (undeclared symbols, appended fields of
  pointer-only types) are the categories where the author's likely
  position is "that was never public"; the lenient definition exists to
  separate that argument from the rest.
* Two systematic limits: a library using symbol versioning shows no break
  and no bump by design, and a Debian-only SONAME bump (spdlog 1.10) would
  count as declared even though upstream declared nothing. The first is small
  in this corpus (symbol-version renames occur in 2 transitions); the 15
  SONAME changes were not individually checked for Debian-only bumps, so
  the declared count is an upper bound on upstream declarations.
* The pairing rule in METHODOLOGY §3.3 exists because of this split: a
  stem that carries the version (`libhunspell-1.6` → `libhunspell-1.7`) is
  the declared-break case, and losing it as "unpaired" would have removed
  declared breaks from the count.

## Sources

* Debian Policy Manual, [§8 Shared libraries](https://www.debian.org/doc/debian-policy/ch-sharedlibs.html)
* Debian Library Packaging Guide, [libpkg-guide](https://www.netfort.gr.jp/~dancer/column/libpkg-guide/libpkg-guide.html)
* Debian mentors thread, [Are soname bumps required when library upgrades break compatibility?](https://lists.debian.org/debian-mentors/2007/09/msg00255.html)
* GNU libtool manual, [Updating version info](https://www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html)
* Fedora Project Wiki, [Packaging tricks: sonames](https://fedoraproject.org/wiki/Packaging_tricks)
* Ulrich Drepper, [How To Write Shared Libraries](https://cs.dartmouth.edu/~sergey/cs258/ABI/UlrichDrepper-How-To-Write-Shared-Libraries.pdf)
* DPDK, [ABI Versioning](https://doc.dpdk.org/guides/contributing/abi_versioning.html)
* libcurl, [ABI policy](https://curl.se/libcurl/abi.html) and Daniel Stenberg, [Eighteen years of ABI stability](https://daniel.haxx.se/blog/2024/10/30/eighteen-years-of-abi-stability/)
* Qt, [Qt Version Compatibility](https://wiki.qt.io/Qt-Version-Compatibility)
* Debian bug [#1015742 libspdlog1: ABI breakage without SONAME bump](https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=1015742) and spdlog issue [#2454](https://github.com/gabime/spdlog/issues/2454)
* Debian bug [#511011 libtdb1: Breaks ABI without SONAME bump](https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=511011)
* Debian bug [#1041302 svt-av1: breaks ABI without SONAME bump](https://www.mail-archive.com/debian-bugs-dist@lists.debian.org/msg1917194.html)
* srt issue [#1592 Shared library soname should change iff ABI incompatible](https://github.com/Haivision/srt/issues/1592)
* STK list, [ABI stability, library versioning, and debian downstream](https://cm-mail.stanford.edu/pipermail/stk/2013-June/001063.html)
* Douglas Creager, [Shared library versions](https://dcreager.net/2017/10/shared-library-versions/)
