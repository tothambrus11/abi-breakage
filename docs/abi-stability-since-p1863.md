# WG21 and ABI stability since "ABI - Now or Never" (P1863R1)

Research note, September 2026. Written as background for this repository's
empirical study of ABI breakage in Debian C/C++ libraries. Sources are linked
inline; where a claim rests on community commentary rather than a primary
record, that is said explicitly.

## 1. What the paper asked

[P1863R1 "ABI - Now or Never"](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p1863r1.pdf)
(Titus Winters, 2020-01-09, audience Direction Group and WG21) is a two-page
position paper, paired with the longer tutorial
[P2028R0 "What is ABI, and What Should WG21 Do About It?"](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p2028r0.pdf)
(2020-01-07). Both were discussed in a joint EWG/LEWG session in Prague,
February 2020, the last meeting of C++20.

The argument:

- WG21 says "performance" but votes "ABI". For years implementers have held an
  effective veto on any proposal that changes layout, mangling, or calling
  convention, so the committee has been prioritising ABI stability without
  admitting it.
- The cost is real but diffuse. Winters estimates a 5-10% aggregate
  performance penalty locked in by ABI: `std::unordered_map`/`std::hash`
  (200-300% possible on the container), `std::string` SSO tuning (about 1% of
  fleet performance), `unique_ptr` passed in memory rather than registers, and
  P2028's longer list (regex, `lock_guard`, `push_back` return type,
  `int128_t`, trivially destructible `bitset`, error-code layouts, vtables of
  `pmr::memory_resource` and iostreams).
- The cost of breaking is larger. Even Google, which builds from source, spent
  "5-10 engineer-years" on a recent ABI-breaking library change; the ecosystem
  cost is estimated in "engineer-millennia", and Hyrum's Law means it grows
  every year.
- Winters says he "cannot argue in good faith" that the list alone is worth a
  break. He offers three options: (1) name a release, C++23 or C++26, as a
  coordinated, total, well-tooled break; (2) formally commit to ABI stability
  and redirect the standard library toward stability and flexibility rather
  than peak performance; (3) keep the status quo, "the worst case scenario".
  He calls option 2 "the boring, responsible, proper choice".
- P2028 adds a concrete mechanism for option 1: change the mangling introducer
  for C++23 (`_Z` to `_Y` on Itanium) so that mismatches fail at link time,
  MSVC-style, rather than at run time as with GCC 5's `std::string` change. It
  also predicts that if stability is promised, Google's interest in the
  standard library "will be limited to primitives that are demonstrably
  efficient", roughly `vector` and `string`.

Context before Prague: the discussion had been running since 2019. Roger Orr's
[P1654 "ABI breakage - summary of initial comments"](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p1654r1.html)
(R0 June 2019, R1 April 2020) collected email traffic following a Michael Wong
note that framed four options: never break, break case by case, break at
declared boundary releases, break every release. An "ABI group" chaired by
Daveed Vandevoorde already existed by Prague (it appears in the meeting minutes
with "No report"), and the Direction Group's
[P2000R1](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p2000r1.pdf)
(January 2020) urged "the creation of an ABI impact review board".

## 2. What happened in Prague (February 2020)

There are no public per-poll numbers; the joint-session polls live on the
members' wiki. The public record consists of the plenary minutes and trip
reports.

From the [Prague minutes, N4870](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/n4870.pdf),
JF Bastien's EWG report:

> EWG had a joint session with LEWG regarding ABI break policy. Some polls are
> difficult to interpret. We decided not to promise ABI stability. Most people
> are saying we should consider incremental ABI for every C++ release. This is
> not saying we will do it, this is saying we will consider it. We did not have
> a consensus for a big ABI break for C++23, but it's important to notice a lot
> of people are in favour of it. There were far more people in favour of a big
> ABI break at some point in time, but there were still 14 strongly against
> votes. [...] We also took a poll that says when we are unable to resolve a
> conflict between performance and ABI compatibility, we should prioritize
> performance. There were more positive votes than negative, but a lot of
> strongly against votes.

The same minutes record SG16 turning down a `std::regex` enhancement
(P1844R1) "due to severe ABI considerations", with volunteers to bring a
deprecation paper; Peter Bindels: "we are deprecating std::regex because it's
unfixable and unusable." (No such paper has appeared as of mid-2026; the
[SG16 tracking issue](https://github.com/sg16-unicode/sg16/issues/57) is still
open.)

Corentin Jabot's summary of the polls, in
["The Day The Standard Library Died"](https://cor3ntin.github.io/posts/abi/)
(2020-02-24), is the one most people quote:

- WG21 is not in favour of an ABI break in C++23.
- WG21 is in favour of an ABI break in a future version of C++ (the poll
  literally said "C++SOMETHING").
- WG21 will take time to consider proposals requiring an ABI break.
- WG21 will not promise stability forever.
- WG21 wants to keep prioritising performance over stability.

"In all these polls, there is a clear majority but no consensus."

[Herb Sutter's trip report](https://herbsutter.com/2020/02/15/trip-report-winter-iso-c-standards-meeting-prague/)
gives the official framing: the committee is "definitely not willing to
guarantee pure ABI stability forever", is "ready to consider proposals
(especially ones that enable performance improvements) even if they may require
an ABI break or migration on some platforms for affected types and functions",
but "isn't ready to take a broad ABI break across the entire standard library".
He called it "an engraved invitation for proposal authors to bring proposals
(and to bring back previously rejected ones)" for June 2020.

[Botond Ballo's report](https://botondballo.wordpress.com/2020/03/12/trip-report-c-standards-meeting-in-prague-february-2020/)
adds two nuances: the room preferred a partial break (some facilities change,
runtime misbehaviour if you use them across an ABI boundary) over a complete
one (link-time failure everywhere), and there was interest in new language
facilities to manage library evolution, such as coexisting versions of a class
with different mangled names.

Net result: of Winters' three options, the committee chose the third. It
declined to promise stability, declined to schedule a break, and agreed to keep
considering ABI-breaking proposals case by case.

## 3. Reactions

**Inside the committee and its orbit.**

- Jabot (2020): "not breaking ABI in 23 is the worst mistake the committee
  ever made." Predicts the standard library loses relevance to Abseil, Folly
  and EASTL; that "new names" workarounds (`scoped_lock` next to
  `lock_guard`) will multiply; that a partial break later will be worse than
  a total one now; and that "you can pick two" of performance, ABI stability
  and the ability to change.
- Google's [P2137R0 "Goals and priorities for C++"](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p2137r0.html)
  (Chandler Carruth and sixteen co-authors including Winters, 2020-03-24) was
  the follow-up. It ranks performance first and "both software and language
  evolution" second, and states that "providing broad ABI-level stability for
  high-level constructs is a significant and permanent burden on their
  design", proposing curated low-level stable ABIs instead. It was an
  informational paper; the committee never adopted it as direction.
- The Direction Group answered in [P2000](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2000r4.pdf)
  (section 7.2.2, present from R1 in January 2020 through R4 in October 2022):
  ABI breaks "should be considered on the merits of each individual proposal";
  "labeling any specific C++ standard as 'your chance to break ABI'" is not
  "healthy for the future of C++". The ABI Review Group (ARG) "has been set up"
  as an advisory board with at most two representatives per company.
- Hal Finkel and Tom Scogland's [P2123R0 "interfaces: A Facility to Manage ABI/API Evolution"](https://wg21.link/p2123r0)
  (March 2020, slides May 2021) was the language-level answer Botond had
  anticipated. LEWG reviewed it in 2021; it has not progressed.

**Wider community.**

- JeanHeyd Meneide, ["Binary Banshees and Digital Demons"](https://thephd.dev/binary-banshees-digital-demons-abi-c-c++-help-me-god-please)
  (September 2021): C has the same disease (`intmax_t`, GCC nested functions,
  `initializer_list`, `polymorphic_allocator`), and implementers use "ABI" as
  a veto over changes they could fix themselves; recounts the pressure needed
  to stop a locale-dependent `std::format` being shipped as ABI-frozen.
- Hacker News threads on Jabot's post ([2020](https://news.ycombinator.com/item?id=22451749),
  [2022 repost](https://news.ycombinator.com/item?id=31791790)) split
  between "just add new names" and "the real problem is that C++ can't build
  from source".
- The "Google left over the ABI vote" story. The Carbon project's own
  [difficulties_improving_cpp.md](https://raw.githubusercontent.com/carbon-language/carbon-lang/trunk/docs/project/difficulties_improving_cpp.md)
  cites P1863, P2028 and P2137 as evidence that when "pushed to address the
  technical debt caused by not breaking the ABI, C++'s process did not reach
  any definitive conclusion". Carbon was announced at CppNorth in July 2022.
  ACCU's Overload noted Google's absence from CppCon 2022, and 2024's
  ["The two factions of C++"](https://herecomesthemoon.net/2024/11/two-factions-of-cpp/)
  treats the "infamous" vote as the moment Google "supposedly significantly
  lowered its participation". None of this is an official Google statement;
  the causal link is community inference. Winters himself had moved to Adobe
  by 2024.
- Luis Caro Campos, CppCon 2025, ["Could C++ Developers Handle an ABI Break Today?"](https://isocpp.org/blog/2026/05/cppcon-2025-could-cpp-developers-handle-an-abi-break-today-luis-caro-campos):
  the abstract notes that EWG "recently reaffirmed its commitment to ABI
  stability", then argues that library authors are already not ABI-careful in
  practice and that Conan and vcpkg can tag binaries by ABI, so the pain may
  be overestimated.
- Victor Ciura's "ABI Resilience" talks (2023 to 2025) hold up Swift's
  resilience model as the design C++ never got.
- May 2026, ["The C++ Standard Library Has Been Walking Itself Back for Fifteen Years"](https://hftuniversity.com/post/the-c-standard-library-has-been-walking-itself-back-for-fifteen-years-and-the-receipts-are-public):
  reads Prague as a vote "in effect, for permanent ABI stability" and blames
  it for `regex`, `unordered_map`, `list`, `map` and `deque` being frozen. The
  [HN discussion](https://news.ycombinator.com/item?id=48254401) mostly
  argued about whether the piece was machine-written.

## 4. What WG21 actually decided afterwards

**No revision, no schedule.** P1863 and P2028 were never revised. P2028's
tracking issue was closed as "needs-revision". No paper has since proposed a
scheduled break for C++26 or C++29. C++23 shipped ABI-stable; C++26 was
finalised in Croydon in March 2026 with no ABI break
([Sutter](https://herbsutter.com/2026/03/29/c26-is-done-trip-report-march-2026-iso-c-standards-meeting-london-croydon-uk/));
the first C++29 meeting in Brno (June 2026) was about undefined behaviour,
profiles and contracts, with nothing on ABI
([Sutter](https://herbsutter.com/2026/06/13/brno-trip-report/)). The
Direction Group's [P5000R0 "Directions for ISO C++29"](https://www.open-std.org/JTC1/SC22/WG21/docs/papers/2026/p5000r0.pdf)
(February 2026) does not mention ABI at all; its subject is safety.

**The one formal statement went the other way.** In Wrocław (November 2024) EWG
voted 29:22:2 to turn Herb Sutter's
[P3466R1 "(Re)affirm design principles for future C++ evolution"](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p3466r1.pdf)
into a standing document. Its first principle, "Retain link compatibility with
C [and previous C++]", reads:

> 100% seamless friction-free link compatibility with older C++ should be a
> default requirement. We can decide to take an ABI breaking change on a case
> by case basis (or, potentially, even wholesale) but should do that with
> explicit discussion and document the reasons why. [...] We should not make a
> change we know requires an ABI break without explicitly approving it as an
> exception.

This is Winters' option 2 in everything but name: stability is the default,
breaks are documented exceptions.

**The ABI Review Group exists but is rarely invoked.** It is listed on
[isocpp.org](https://isocpp.org/std/the-committee) as an advisory group
(chair Daveed Vandevoorde, vice-chair Jason Merrill). Only two papers in the
WG21 index carry it as an audience: P3092R0 "Modules ABI requirement" (2024)
and P3566 on `char*` safety (2025).

**ABI shows up as a routine constraint, not a policy question.** Library issues
are resolved with ABI in mind (LWG3145 `file_clock`, LWG3600 "making
`istream_iterator` copy constructor trivial is an ABI break", LWG3890
integer-class types). The pattern Jabot criticised, new names next to old
ones, became the norm: `flat_map`/`flat_set` and
`move_only_function`/`copyable_function` in C++23, `inplace_vector` and
`hive` in C++26, while `unordered_map`, `function` and `regex` are untouched.
Implementers' January 2026 [P3962R0 "Implementation reality of WG21 standardization"](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p3962r0.pdf)
lists "ABI stability" among the permanent maintenance burdens that make it
hard to keep up with the standard.

**Trivial relocation, the nearest thing to an ABI-adjacent language feature,
slipped.** P2786 (Bloomberg) and P1144 (O'Dwyer) competed for years; P2786R13
was forwarded in February 2025 and was in the C++26 draft, then removed in
Kona in November 2025 over a "showstopper bug" and what
[P4197R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p4197r0.html)
(April 2026) calls "fundamental disagreements on several core aspects". It is
now a C++29 item. Note that relocation helps containers, not the
`unique_ptr` calling-convention cost; that one is still a pure ABI issue.

## 5. What the implementers did

Because WG21 declined to decide, the decision moved to the three vendors, and
they each chose stability by default with an escape hatch.

| Implementation | State in 2026 |
| --- | --- |
| libstdc++ (GCC) | No ABI break since the GCC 5 dual ABI (2015). `libstdc++.so.6` soname unchanged; each release only adds a symbol version. `_GLIBCXX_DEBUG` is the only ABI-changing mode. |
| libc++ (LLVM) | Stable ABI v1 is the default; an "unstable" v2 accumulates the breaks: `[[clang::trivial_abi]]` on `unique_ptr`/`shared_ptr` (exactly the P1863 item), alternate `string` layout, size-based `vector`, optimised `function`, bounded iterators for hardening ([ABI guarantees](https://libcxx.llvm.org/ABIGuarantees.html), [versioning policy](https://libcxx.llvm.org/DesignDocs/ABIVersioning.html)). A 2025 [issue](https://github.com/llvm/llvm-project/issues/142066) objects that v2's `trivial_abi` is non-conforming on destruction order. Small unconditional breaks are still taken when judged harmless, e.g. `bitset::operator[] const` returning `bool` ([December 2025](https://github.com/llvm/llvm-project/pull/169894), with a two-release opt-out). |
| MSVC STL | The v14 ABI has been stable since VS 2015; VS 2026 (v145, Build Tools 14.50/14.51) is still binary-compatible with 2015 ([Microsoft](https://learn.microsoft.com/en-us/cpp/overview/what-s-new-for-msvc)). A "vNext" total break is planned on the [STL wiki](https://github.com/microsoft/STL/wiki/vNext-Planning): rewrites of `regex`, `deque`, `unordered_map`, `unordered_set`, `tuple`, replacing `_Compressed_pair` with `[[no_unique_address]]`. Stephan T. Lavavej said in September 2024 it was "estimated for the next 1-2 years" and unofficial ([python.org thread](https://discuss.python.org/t/planning-for-an-msvc-abi-break/65102)); it has not shipped. MSVC still cannot implement C++20 `[[no_unique_address]]` for ABI reasons and offers `[[msvc::no_unique_address]]` instead. |

Two further observations:

- The vNext list is essentially P2028's list. Microsoft, the vendor whose
  DLL-versioning model P2028 held up as the clean way to break, is the only
  one planning a wholesale break, and six years on it is still a plan.
- The big library initiative of 2024 to 2026, standard library hardening
  (C++26, Dionne, Varlamov, Rebert, Shavrick), was explicitly designed to be
  deployable without an ABI break; the checks that need one (bounded
  iterators in `vector`/`string`) are opt-in. Even safety work is now shaped
  around the constraint P1863 asked the committee to confront.

## 6. Scorecard against P1863's list

| P1863 / P2028 item | 2026 status |
| --- | --- |
| `unique_ptr` by value in registers | Only under libc++ unstable ABI (`trivial_abi`). Itanium ABI unchanged. |
| `unordered_map` / `std::hash` layout | Unchanged everywhere. MSVC vNext plans a rewrite. `flat_map` added as a new name. |
| `std::string` SSO tuning | Only under libc++ unstable ABI. |
| `std::regex` | Unchanged; deprecation never proposed; MSVC vNext plans a rewrite. |
| `lock_guard` as alias of `scoped_lock` | Not done. |
| `push_back` returning a reference | Not done. |
| `int128_t` / `intmax_t` | Not standardised. |
| Trivially destructible `bitset` | Not done. |
| Vtable changes in `pmr::memory_resource`, iostreams | Not done. |
| Error-code layouts (P1196-P1198) | Not done. |

## 7. Bottom line

Winters asked WG21 to choose between a scheduled, total break and an honest
commitment to stability, and warned that the status quo was the worst outcome.
The committee polled a wish to break "someday", refused to pick a day, and
then, in 2024, codified link compatibility as the default. In practice the
ecosystem got option 2 without the honesty Winters wanted, and the actual ABI
policy of C++ is whatever libstdc++, libc++ and MSVC each decide. The energy
that might have gone into an ABI transition went instead into the safety
agenda that now dominates C++26 and C++29, and the "long-term challenge from
other systems languages" the paper predicted arrived in the form of Rust,
Carbon and profiles rather than of a faster `unordered_map`.
