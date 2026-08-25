# Motif Chess Optimization Journey

This log records hypotheses before implementation, measured outcomes after
implementation, and the decisions that followed. It is source material for a
future engineering article or presentation, not a polished narrative.

## Method

- Optimize from profiles and end-to-end measurements, not attractive designs.
- Keep heavy performance runs serialized and use the same Release preset and
  1M-game corpus.
- Separate canonical storage, derived-index construction, publication, and
  query costs.
- Preserve correctness with DuckDB or the previous implementation as an oracle.
- Record failed assumptions and deferred ideas as carefully as successful work.

## Iteration 1: Do Not Share Replay Yet

**Hypothesis:** Replaying each chess game once into a shared transition tape
would reduce total postings-plus-tree build time.

**Evidence:** The 1M baseline showed postings replay at 12.316 s out of 95.272 s
and tree replay at 23.939 s. A full transition tape would add roughly 0.8-0.9
GiB of payload and 2.4-2.7 GiB of sequential I/O. Builders were already
sequential, so replay sharing did not solve the observed peak-RSS problem.

**Decision:** Defer replay tapes. Attack postings storage amplification and the
tree's duplicate child-frequency work first.

## Iteration 2: Compact Exact Postings

**Hypothesis:** Repeated result/Elo fields and the flat per-hash directory
dominated postings size and builder memory.

**Change:** Format v5 stores metadata once per game, groups and delta-codes game
IDs and plies, compresses directory entries into 256-hash blocks with a sparse
top-level directory, and streams posting blocks directly to staging.

**Measured 1M result:**

| Metric | v4 | v5 |
|---|---:|---:|
| End-to-end postings import | 95.272 s | 67.409-67.962 s |
| Peak RSS | 5,962 MiB | 1,254-1,474 MiB |
| Artifact | 3,196 MiB | 1,206 MiB |

**Surprise:** The compressed directory still occupies about 828 MiB because the
corpus has 68.9 million distinct hashes. The posting payload fell to about 420
MiB, so directory compression, not posting compression, remains the larger
storage opportunity.

**Decision:** Accept v5. Preserve the sorted summary stream for tree-frequency
reuse. Do not add radix partitioning until another measurement points there.

## Iteration 3: Measure the Public Explorer, Not Only Its Index

**Hypothesis:** A microsecond shallow-tree reader implies a fast opening
explorer.

**Measured result:** The raw reader reached 4.77 us p50 and 10.50 us p99 over
41,822 shallow positions. The public `opening_stats::query` measured 0.938 ms
warm p50 and 3.039 ms warm p99 for 512 non-starting positions, but the starting
position took about 3.2 seconds.

**Cause:** The public API decoded the starting position's approximately
one-million-game posting block only to compute `total_games`, then read the
microsecond tree aggregate.

**Lesson:** A fast internal index does not guarantee a fast product path. Time
the public boundary and include highly popular keys, not only representative
median keys.

## Iteration 4: Summary-Count Fast Path

**Hypothesis:** The postings directory already contains the exact distinct-game
count, so the public unfiltered explorer can avoid posting-block decode.

**Change:** `opening_stats::query` asks `database_manager::position_summary`
for `distinct_game_count`. It retains the game-ID fallback when postings are
missing or stale. A DuckDB-only canonical rebuild now invalidates old immutable
indexes even when the game count happens to remain unchanged.

**Pre-benchmark expectation:** Starting-position latency should fall from
seconds to the cost of one compressed-directory block read, one shallow-tree
read, a small SQLite context fetch, and presentation. Non-starting p50 may
improve only slightly because their posting lists were already small.

**Result:** The hypothesis was incomplete. Two repeated 1M runs still measured
the starting position at 3.115-3.128 s warm p50 and 3.201-3.206 s first call.
Non-starting warm latency remained about 0.93-0.94 ms p50 and improved to
1.43-1.71 ms p99 in these runs.

**Why it did not fix the starting position:** The summary removed the first
game-ID decode, but `database_manager::query_unfiltered_opening_stats` checks
the postings summary's `max_ply`. Games can repeat the starting position after
ply 20, while the shallow tree aggregates root occurrences only through ply 20.
The manager therefore rejects the incomplete shallow aggregate and performs
the full postings-backed replay fallback. That fallback, not `total_games`, is
the remaining multi-second cost.

**Decision:** Retain the summary path because it removes redundant work for
covered positions and halves posting-list decoding on deep fallback paths. Do
not weaken the `max_ply` correctness check. Investigate complete aggregation
for a very small set of high-value roots, beginning with the starting position,
without expanding all roots to full depth.

## Iteration 5: Complete Aggregation for the Starting Position Only

**Hypothesis:** `database_manager::query_unfiltered_opening_stats` falls back
to full postings replay for the starting position because
`position_postings::summary(hash).max_ply` exceeds `opening_tree_index`'s
`max_root_ply` whenever a game returns to the starting position after ply 20
and keeps playing. Special-casing full-depth edge aggregation for exactly the
canonical starting-position hash during the tree build, and recording that
completeness in the index itself, should let dispatch trust the shallow tree
for that one root without the summary cross-check, while every other root
keeps the existing max_root_ply cap.

**Change:** `opening_tree_index` format bumped to v5. `accumulate_game`
captures edges from the canonical starting-position hash (`board{}.hash()`)
at every occurrence in a game, not only within `max_root_ply`; every other
root is unaffected. Each node header gained a `complete` flag so a v5 file
proves which nodes were aggregated past `max_root_ply` rather than the reader
inferring it from the hash value alone. `opening_tree_index::is_complete()`
exposes that flag. `database_manager::query_unfiltered_opening_stats` checks
it before the `summary->max_ply` cross-check and, when true, returns the tree
result directly. `game_count()` for the starting position was already exact
before this change (the initial position is inserted into `roots_this_game`
unconditionally, before the ply loop), so only the continuation edges needed
the fix.

**Verification:** New test
`opening_tree_index marks the starting position complete and aggregates
edges taken after max_root_ply` builds a game that shuffles back to the
starting position at ply 24 and plays a new move there, then checks
`is_complete()` is true only for the starting hash and that the tree's
continuations match an unbounded DuckDB oracle (game-ID-filtered, not the
existing `opening_continuation` rollup, which has the identical
`opening_stats_max_root_ply` cap and would otherwise agree with a broken
answer). Full `ctest` suite (476 non-perf tests) passes.

**Not yet measured:** No 1M-game benchmark run for this iteration (per
instructions); expected effect is the starting-position path skipping full
postings replay the same way non-starting positions already do, but this is
unverified at scale.

**Decision:** Keep the fix scoped to the one canonical root. Do not expand
full-depth aggregation to other roots without a measurement showing a
comparable latency problem elsewhere; the per-node `complete` flag already
generalizes to more roots later if that measurement appears.

## Next Candidates

- Feed postings' sorted distinct-game counts into opening-tree construction to
  remove full-depth child visits, 84 child spill runs, and an approximately
  12-second child-frequency merge.
- Buffer opening-tree codecs and reuse per-game scratch.
- Reduce the 828 MiB compressed postings directory if storage remains the
  priority.
- Reconsider replay sharing only after the above work establishes a new
  baseline.

## Iteration 5: Make One High-Value Root Complete

**Hypothesis:** The starting position is uniquely valuable and can be made
complete without expanding every opening-tree root beyond ply 20.

**Change:** Opening-tree format v5 carries a persisted per-node completeness
flag. The builder aggregates every occurrence of the canonical starting
position, at any ply, while all other roots retain the ply-20 cap. The reader
accepts the flag only for the starting hash, requires its game count to equal
the source game count, and validates every edge frequency against that count.

**Measured 1M result, two serialized Release runs:**

| Metric | Before | Complete starting root |
|---|---:|---:|
| Starting position, first call | 3,201-3,206 ms | 1.066-1.084 ms |
| Starting position, warm p50 | 3,115-3,128 ms | 0.898-0.926 ms |
| Non-starting warm p50 | 0.932-0.939 ms | 0.881-0.888 ms |
| Non-starting warm p99 | 1.429-1.712 ms | 1.430-1.439 ms |
| Postings + tree import | 127.568-127.743 s | 128.276-128.421 s |
| Tree artifact | 242 MiB | 246 MiB |

The starting-position path improved by roughly 3,400x. Tree construction added
about 0.7-0.9 seconds end to end in these adjacent runs, while builder-local
tree time rose from about 60.6 seconds to 61.7-61.8 seconds. Only one additional
distinct edge appeared on this corpus; the extra artifact bytes primarily come
from the v5 completeness field added to every node.

**Lesson:** A narrow completeness exception can outperform a broad cache or
format redesign when one extremely popular key dominates product latency. The
exception remains safe because completeness is persisted and validated, not
inferred by query code.

**Decision:** Keep the complete starting root. The next build optimization is
still postings-to-tree child-frequency reuse, which targets 84 spill runs and
roughly 12.9 seconds of child merge without changing query semantics.

## Iteration 6: Reuse Exact Postings Counts in the Tree Builder

**Hypothesis:** Exact postings already persist the distinct-game count for
every position hash. Feeding that sorted summary stream into manager-backed
opening-tree construction should remove the tree's second full-depth visit
counter, its child spill runs, and its k-way merge without changing
`transposition_frequency` semantics.

**Change:** `opening_tree_index` accepts an optional sorted child-count stream.
When supplied, game replay still builds shallow roots and direct edge
aggregates but does not allocate or update the per-game full-depth visited-hash
set. The postings summary stream is materialized into the existing bounded
child-count file format, so final index writing retains its referenced-child
merge and low-memory binary-search fallback. Standalone tree builds keep the
independent replay counter as an oracle. Missing, nonascending, zero, or
too-small external counts fail closed.

**Measured 1M result, two serialized Release runs:**

| Metric | Independent tree counts | Postings count reuse |
|---|---:|---:|
| Postings + tree import | 128.276-128.421 s | 109.060-109.225 s |
| Tree accounted phases | about 55.5 s corrected baseline | 37.035-37.242 s |
| Tree replay | 23.0 s | 11.590-11.747 s |
| Child spill runs | 84 | 0 |
| Child visits accumulated | full-depth stream | 0 |
| Tree artifact | 246 MiB | 246 MiB |
| Process peak RSS | 1,452 MiB baseline detailed run | 1,427-1,514 MiB |

The tree's accounted phases improved by about 33% and the complete
postings-plus-tree import improved by about 15%. These corrected phase totals
subtract `child_frequency_load_elapsed` from the old `index_write_elapsed`,
which previously included and therefore double-counted the load. The external
summary materialization still costs 6.35-6.40 seconds, so count reuse removes
both the approximately 12-second merge and about half of tree replay rather
than making child-count handling free. Query latency remained effectively
unchanged: raw tree p50 was
4.82-4.85 us and p99 was 10.58-10.61 us; public non-starting warm p50 was
0.891-0.895 ms and p99 was 1.384-1.525 ms.

**Decision:** Keep postings count reuse for manager-backed builds. Keep the
standalone replay-count path for isolation and oracle testing. Shared replay is
still not justified by the remaining profile; the next candidates are buffered
tree codecs, per-game scratch reuse, and reducing the postings directory.

## Iteration 7: Buffer Final Tree Encoding

**Hypothesis:** Final tree serialization writes every fixed-width and varint
byte through `std::ofstream::put`. Encoding into 64 KiB blocks should reduce
virtual stream calls without changing the persisted format.

**Change:** The final index writer now encodes headers and continuation records
through a small file-local buffered writer. Spill writers and all readers are
unchanged. The benchmark reports child-frequency loading separately and derives
serialization by subtracting that nested phase from the inclusive index-write
timer; the stored metric stays comparable with the low-memory lookup fallback.

**Measured 1M result:** Three serialized Release runs produced final-phase
totals of 10.981-11.081 seconds including child-frequency loading. After the
metric correction, the detailed run split that into 6.282 seconds loading and
4.799 seconds serialization. The preceding unbuffered runs were 12.088-12.097
seconds including 6.352-6.396 seconds loading, implying 5.692-5.745 seconds of
serialization. Buffering therefore reduced its targeted phase by about 16%.
The corrected complete tree phase total was 36.390 seconds versus
37.035-37.242 seconds before, about a 2% improvement. End-to-end
postings-plus-tree import measured 108.591-108.860 seconds, only a modest
improvement over 109.060-109.225 seconds and close to run variance.

**Decision:** Keep the localized writer because it removes roughly one second
from a stable, isolated phase without changing the format or query path. Do not
generalize it into a shared codec abstraction yet. The next larger opportunity
is per-game replay scratch reuse or the postings directory.

## Iteration 8: Reuse Per-Game Tree Scratch

**Hypothesis:** Tree replay still constructs fresh root and edge hash containers
for every game. Retaining their capacity in builder state should remove several
small allocations and rehashes per game without changing aggregation semantics.

**Change:** The builder owns reusable root, edge, and optional full-depth visit
containers. `accumulate_game` clears them at entry, which both retains capacity
and prevents a failed game from contaminating the next attempt. Manager-backed
builds still omit the full-depth visited set because postings supply those
counts. Review also exposed a pre-existing failure-state issue: a failed
`accumulate` could leave partial spill/counter effects and still permit reuse.
The builder now latches its first error and rejects further accumulation or
finalization.

**Measured 1M result:** Across the first two isolated runs, tree replay fell
from the buffered-writer baseline of 11.836-11.942 seconds to 10.929-11.074
seconds, a 6.5-8.5% improvement. Subsequent runs while instrumenting and
changing the postings directory measured 11.003-11.218 seconds, still below
the baseline despite adjacent workload changes. Peak RSS did not increase.

**Decision:** Keep scratch reuse. Its implementation is small, the replay gain
clears the 2-3% adoption gate, and failure latching improves correctness beyond
the performance change.

## Iteration 9: Compact the Directory Again

**Hypothesis:** The 828 MiB v5 directory is large enough that estimates are not
adequate. Exact field accounting should identify a small exact v6 layout that
reduces it by at least 20% without changing point lookup or summary semantics.

**Measured v5 field costs on 68,907,007 hashes:**

| Field | Bytes |
|---|---:|
| Hash deltas | 403,486,860 |
| Redundant posting-offset deltas | 68,697,012 |
| Posting lengths | 68,966,408 |
| Occurrence counts | 68,919,729 |
| Distinct-game counts | 68,919,693 |
| Minimum plies | 72,266,086 |
| Maximum plies | 72,338,956 |

Only 1,127,882 hash deltas exceed 40 bits and none exceed 48 bits on this
corpus. Also, 68,306,247 entries have equal occurrence and distinct-game
counts. These distributions support a narrow exact encoding rather than a
general entropy codec.

**Change:** Directory format v6 removes posting-offset deltas and derives each
offset from the preceding posting length. Each block stores 40-bit hash-delta
lows plus a bitmap and ULEB128 high parts for sparse exceptions. A second
bitmap omits occurrence counts when they equal distinct-game counts. Every
summary remains exact; blocks remain independently readable; sorted summary
streaming and one-block point lookup are unchanged. The decoder rejects
non-canonical bitmap padding, zero high exceptions, overflow, malformed counts,
and old versions.

**Measured 1M result, three serialized Release runs:**

| Metric | v5 | v6 |
|---|---:|---:|
| Directory | 828,439,768 bytes | 649,481,128 bytes |
| Full postings artifact | 1,206 MiB | 1,035 MiB |
| Peak temporary estimate | 2,368 MiB | 2,198 MiB |
| Postings-only import | 68.018-69.958 s adjacent instrumented runs | 67.614-68.346 s |
| Postings + tree import | 108.564-109.892 s | 107.028-107.471 s |

The directory shrank by 178,958,640 bytes, or 21.6%, clearing the acceptance
gate. The complete artifact shrank by about 14.2%. The final v6 directory
breakdown is 4,845,024 bytes of block headers, 17,226,752 bytes of bitmaps,
344,317,077 bytes of hash encoding, 68,966,408 bytes of posting lengths,
601,132 bytes of occurrence exceptions, 68,919,693 bytes of distinct-game
counts, and 144,605,042 bytes of ply bounds.

**Decision:** Keep v6. Do not add an MPHF, larger block size, or general entropy
codec: they would add significantly more construction and validation complexity
without a measured requirement after v6 clears the storage gate.

## Iteration 10: Measure the Literal Default

**Question:** Does the production perf test measure the 67-68 second
DuckDB-free postings path, or does the default configuration still build both
derived stores?

**Observation:** `import_config {}` enables both
`rebuild_positions_after_import` and `build_position_postings_after_import`.
The manager therefore rebuilds, sorts, and rolls up DuckDB before building
exact postings.

**Measured 1M result on the current worktree:**

| Path | Wall | Peak RSS |
|---|---:|---:|
| SQLite + DuckDB | 59.479 s | 9,484 MiB |
| SQLite + exact postings | 66.618 s | 1,404 MiB |
| SQLite + postings + opening tree | 105.199 s | 1,488 MiB |
| Literal `import_config {}` | 102.734 s | not separately sampled |

The postings-only run spent 17.071 seconds in external merge, 12.270 seconds
in replay, and 6.787 seconds in spill work. Its measured builder phases left
5.289 seconds for validation, checksum, publication, cleanup, and other
orchestration.

**Decision:** Do not claim that the default import takes 66.6 seconds. The
current literal default takes 102.7 seconds because it still pays for both
derived stores. Inventory every remaining DuckDB-dependent product and recovery
path before changing the default. If DuckDB can be removed, disabling its
rebuild is the largest immediate end-to-end optimization. Otherwise, target
postings partition/radix construction or instrument publication overhead;
another directory encoding change is not the next priority.

## Iteration 11: Make Exact Postings the Production Default

**Question:** Can the import default omit a fresh DuckDB rebuild without losing
query behavior, recovery guarantees, or compatibility with existing bundles?

**Inventory:** Exact search and pagination, position-filtered game search,
filtered opening statistics, Elo distributions, and deep bounded traversal all
dispatch through exact postings when the published generation matches canonical
SQLite. Unfiltered shallow traversal may additionally use the opening tree, but
falls back to postings replay when the tree is absent or incomplete. Stale,
malformed, or count-mismatched postings fail closed when DuckDB is not known to
be synchronized. Explicit DuckDB builds and existing DuckDB-only bundles retain
their query fallback. Reopen accepts checksum-verified postings that cover every
canonical game without rebuilding DuckDB; canonical mutation invalidates the
derived generation before changing SQLite, and dirty recovery rebuilds when no
valid exact generation remains.

**Change:** `import_config {}` now leaves
`rebuild_positions_after_import` disabled while keeping
`build_position_postings_after_import` enabled. DuckDB rebuilding remains an
explicit option. Focused tests cover every inventoried query path, postings-only
reopen, stale-index failure, mutation recovery, and DuckDB-only compatibility.

**Verification:** The focused non-performance import suite and postings tests
passed during implementation. Three isolated fresh-process 1M Release runs of
the literal default measured 81.359, 81.520, and 81.728 seconds. The median is
81.520 seconds, or 12,280 attempted games/s, a 20.6% improvement over the old
102.734-second dual-build default. Each run reported zero DuckDB replay, sort,
and rollup time, confirming that it exercised the intended postings-only path.

A contemporaneous serialized comparison in one process measured DuckDB at
59.847 seconds, postings at 66.959 seconds with 1,110 MiB peak RSS, and postings
plus the opening tree at 105.280 seconds. The 14.561-second gap between its
postings result and the isolated median is repeatable process/order sensitivity,
not ordinary run variance. The current instrumentation does not attribute it;
the isolated median is therefore the production default benchmark, while the
comparison remains useful for same-process path and memory comparisons.

**Decision:** Adopt postings-only as the literal production default. Preserve
DuckDB as an explicit and compatibility fallback rather than removing it. Use
81.520 seconds, not the warmer comparison result, as the current default timing
until the process-context difference is explained.

## Iteration 12: Stop Provisioning DuckDB for Persistent Bundles

**Goal:** Make the default bundle and runtime genuinely DuckDB-free while
retaining one release of safe compatibility for existing DuckDB-only bundles.

**Change:** New persistent bundles publish an empty postings generation and do
not create `positions.duckdb`. Bundles whose checksum-verified postings cover
canonical SQLite open without a DuckDB connection, including dual-index bundles
that retain an old DuckDB file. Missing or corrupt postings in a postings-only
bundle are rebuilt directly from SQLite; if staging or publication fails, open
fails closed without creating DuckDB or changing the published manifest.

Legacy DuckDB-only bundles still open their existing file during this migration
release and make one failure-safe postings migration attempt. A failed attempt
removes only unpublished staging bytes and leaves the legacy fallback usable.
The old DuckDB file is never deleted. Scratch databases temporarily retain
in-memory DuckDB, and an explicit `rebuild_position_store()` may still lazily
provision DuckDB for compatibility and oracle tests.

**Recovery model:** A clean position-index state now means either synchronized
DuckDB or valid postings that cover canonical SQLite. Postings-only bundles no
longer remain perpetually dirty. Canonical mutation invalidates postings first;
queries fail closed until direct SQLite-to-postings repair succeeds.

**Verification:** Focused manager, import, workspace, and search suites pass.
The complete development suite passes 497 tests when excluding the independently
reproducible HTTP import-SSE timing race, and the sanitizer suite passes all 416
eligible tests. Static analysis and formatting are clean. No new large-corpus
benchmark was run for this lifecycle-only milestone.

**Decision:** Keep DuckDB linked for one migration release, but do not provision
it for normal persistent operation. Remove the source and packaging dependency
only after migrated bundles have shipped and the scratch/oracle fixtures have a
replacement.

## Iteration 13: Complete the Release-Boundary Removal

**Goal:** Remove the final source and packaging dependency while preserving a
safe one-release migration path for bundles that still contain a legacy
`positions.duckdb` file.

**Change:** `database_manager::open()` now rebuilds missing, stale, or corrupt
postings directly from canonical SQLite. It never opens, modifies, or deletes a
legacy DuckDB file. Publication remains staged, checksum-validated, and atomic;
failure leaves the published manifest and canonical database unchanged and is
safe to retry. Scratch databases use regular bundles inside private temporary
directories removed on close. Deterministic expected fixtures replace DuckDB
oracle queries. DuckDB source, CMake links, Nix inputs, and vcpkg dependencies
are removed.

**Verification:** The complete development suite passed all 483 registered
tests, with corpus and performance tests skipped when fixtures were absent. The
ASan/UBSan preset passed all 402 eligible tests while also running clang-tidy and
cppcheck with zero warnings. Claude Code Sonnet 5 independently reviewed the
complete diff and found no implementation defect.

**Decision:** Treat SQLite as the only canonical store and exact postings as the
only durable full-depth position index. Preserve `positions.duckdb` only as an
ignored legacy filename during the migration window; no runtime code reads it.
