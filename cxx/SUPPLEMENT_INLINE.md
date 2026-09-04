# Supplement: inline function and template body changes

Recomputed per definition from the header indexes of every transition (`scripts/inline_bodies.py`). This is a description of header-level churn that no ABI tool observes; it is **not** folded into the break rates of RESULTS.md. Token-level fingerprints: every count is an upper bound on semantic change.

## 1. Coverage

| | all | C | C++ |
|---|---|---|---|
| transitions with both header indexes | 741 | 551 | 190 |
| with >= 1 inline function body | 253 (34.1 %) | 145 | 108 |
| with >= 1 template body | 59 (8.0 %) | 28 | 31 |
| poor header parse coverage (lower bounds) | 141 (19.0 %) | | |

Definitions in the OLD release's headers across all transitions: 58068 inline functions, 134156 template definitions.

"C" is the language of the shared object, decided from its exported symbols (RESULTS.md §5.2); the `-dev` package of a C library can still ship C++ wrapper headers, which the indexer parses as C++. The C rows with template bodies come entirely from such wrappers:

* gmp: `gmpxx.h`
* ncurses: `ncursesw/cursesf.h`, `ncursesw/cursesm.h`, `ncursesw/cursesp.h`
* z3: `z3++.h`
* zeromq3: `zmq.hpp`, `zmq_addon.hpp`

## 2. How often do bodies change?

Share of transitions (over those shipping at least one definition of that kind) in which at least one body changed.

| | inline function bodies | template bodies | either | both |
|---|---|---|---|---|
| all | 37/253 (14.6 %) | 15/59 (25.4 %) | 43/253 (17.0 %) | 9 |
| C | 19/145 (13.1 %) | 3/28 (10.7 %) | 19/145 (13.1 %) | 3 |
| C++ | 18/108 (16.7 %) | 12/31 (38.7 %) | 24/108 (22.2 %) | 6 |

By release level (transitions with >= 1 changed body of the kind / transitions shipping the kind):

| level | inline | template |
|---|---|---|
| major | 0/2 (0.0 %) | 0/0 (n/a) |
| minor | 12/63 (19.0 %) | 4/12 (33.3 %) |
| patch | 18/157 (11.5 %) | 5/29 (17.2 %) |
| snapshot | 3/26 (11.5 %) | 6/17 (35.3 %) |
| other | 4/5 (80.0 %) | 0/1 (0.0 %) |

## 3. What changes, and how much?

| | inline | template |
|---|---|---|
| changed definitions | 857 | 369 |
| body only (declaration fingerprint unchanged) | 808 | 369 |
| body and declaration | 49 | 0 |
| size delta token count unchanged | 163 | 28 |
| size delta 1-4 tokens | 97 | 51 |
| size delta 5-19 tokens | 239 | 26 |
| size delta 20+ tokens | 358 | 264 |
| median body size of a changed definition (tokens, old) | 236 | 1099 |

Changed definitions by clang cursor kind: CXXMethod 519, FunctionDecl 330, FunctionTemplate 279, CXXConstructor 84, CXXConversion 7, CXXDestructor 7.

## 4. Where it concentrates

| library | transitions | with inline body change | with template body change | changed definitions |
|---|---|---|---|---|
| gmp | 6 | 2 | 1 | 564 |
| xxhash | 9 | 5 | 0 | 159 |
| libsigc++-2.0 | 8 | 3 | 3 | 144 |
| z3 | 8 | 7 | 1 | 116 |
| e2fsprogs | 9 | 1 | 0 | 93 |
| gcc-16 | 9 | 2 | 7 | 57 |
| zeromq3 | 6 | 1 | 1 | 26 |
| hunspell | 9 | 6 | 0 | 21 |
| openexr | 5 | 1 | 2 | 14 |
| intel-gmmlib | 9 | 3 | 0 | 11 |
| ncurses | 9 | 1 | 0 | 10 |
| libheif | 9 | 1 | 0 | 4 |
| soundtouch | 8 | 1 | 0 | 3 |
| jansson | 8 | 1 | 0 | 2 |
| apt | 9 | 1 | 0 | 1 |
| libmodplug | 1 | 1 | 0 | 1 |
| acl | 4 | 0 | 0 | 0 |
| apparmor | 9 | 0 | 0 | 0 |
| aspell | 3 | 0 | 0 | 0 |
| attr | 4 | 0 | 0 | 0 |

16 of 109 libraries change at least one body in ten releases; the top five libraries account for 87.8 % of changed definitions.

## 5. Does body churn coincide with binary breaks?

Strict binary break rate (RESULTS.md definition) among transitions with and without body changes, over transitions shipping at least one inlinable definition.

| | transitions | strict binary break |
|---|---|---|
| body changed | 43 | 26 (60.5 %) |
| no body change | 210 | 55 (26.2 %) |

Definitions added / removed from headers (not body changes): 1742 added, 987 removed across all transitions; 32 transitions remove at least one inlinable definition.

The two groups are not comparable populations: a release that rewrites header bodies is usually a large release that also changes exported symbols and layouts, so the association is confounded by release size and says nothing causal.

## 6. Examples

The largest body change of each transition, twelve transitions with the largest changes:

* `gmp@6.1.2+dfsg..6.2.0+dfsg`: template `abs` in `gmpxx.h`, 1930 -> 2838 tokens
* `zeromq3@4.3.1..4.3.2`: inline `monitor` in `zmq.hpp`, 543 -> 23 tokens
* `xxhash@0.8.2..0.8.3`: inline `XXH3_accumulate_scalar` in `xxhash.h`, 4521 -> 4988 tokens
* `xxhash@0.8.0..0.8.1`: inline `XXH3_update` in `xxhash.h`, 343 -> 675 tokens, declaration also changed
* `xxhash@0.8.1..0.8.2`: inline `XXH3_update` in `xxhash.h`, 675 -> 431 tokens, declaration also changed
* `z3@4.4.1..4.8.4`: inline `operator+` in `z3++.h`, 116 -> 227 tokens
* `xxhash@0.7.3..0.7.4`: inline `XXH_INLINE_XXH3_64bits_withSecret` in `xxh3.h`, 120 -> 18 tokens
* `gmp@6.2.1+dfsg1..6.3.0+dfsg`: inline `__gmp_expr` in `gmpxx.h`, 3204 -> 3258 tokens
* `gcc-16@16-20260226..16-20260307`: template `_M_invalidate_all` in `c++/16/debug/safe_unordered_container.h`, 44 -> 93 tokens
* `gcc-16@16-20260315..16-20260322`: template `allocate` in `c++/16/bits/new_allocator.h`, 146 -> 189 tokens
* `openexr@3.4.6+ds..3.4.14`: template `Array2D<T>` in `OpenEXR/ImfArray.h`, 3 -> 41 tokens
* `z3@4.8.8..4.8.9`: inline `add` in `z3++.h`, 47 -> 10 tokens

## 7. Caveats

* Token-level comparison: renaming a local variable or reformatting a macro-generated body counts as a change; nothing here says whether behaviour changed.
* Templates are counted by definition, not by instantiation; a change to one template reaches every client instantiation of it.
* Class-template member functions are classified as templates by their USR; a non-template inline member of a template class is still a template here because clients instantiate it.
* Indexes cover the headers libclang could parse with the default flags; 141 transitions have poor parse coverage and their counts are lower bounds.
* C libraries contribute `static inline` helpers and macro-heavy headers; C++ libraries contribute member functions defined in class bodies, which is why the inline column is dominated by C++ member functions.
