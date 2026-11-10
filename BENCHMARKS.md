# Benchmarks

Performance results recorded on each meaningful change. All timings taken on the release build (`cmake --preset=release`) unless noted otherwise.

## Benchmark suite: how to run, compare, and when to refresh the baseline

The suite (`test/source/motif_bench/`, executable `motif_bench_test`) tracks performance across the incremental-index PR sequence. It uses only public APIs (`database_manager`, `game_store`/`game_writer`, `opening_stats::query`, `opening_tree`, `import_pipeline`), so the same scenarios stay comparable when mutations become incremental. Cases are tagged `[performance][bench]`, so the standard filter `~[objectives]~[performance]` excludes them, and the executable is not registered with `ctest`.

**Build (Release only; Debug and sanitizer builds skip every scenario):**

```sh
cmake -S . -B /tmp/mc-rel -DCMAKE_BUILD_TYPE=Release -Dmotif_DEVELOPER_MODE=ON \
      -Dmotif_ENABLE_SLINT_APP=OFF -Dmotif_ENABLE_X86_64_V3=ON
cmake --build /tmp/mc-rel -j --target motif_bench_test
```

**Run** (`scripts/bench_compare.py`, Python 3 stdlib only; each scenario runs in its own process, repeated `--repeat` times, medians stored):

```sh
# quick tier: 100k corpus, a few minutes
scripts/bench_compare.py run --build-dir /tmp/mc-rel --tier quick --repeat 3 --out /tmp/new-quick.json
# full tier: 1M corpus; insert/remove kinds still rebuild indexes
scripts/bench_compare.py run --build-dir /tmp/mc-rel --tier full --repeat 3 --out /tmp/new-full.json
# run only chain growth and compaction (seeds its own reference bundle if needed)
scripts/bench_compare.py run --build-dir /tmp/mc-rel --tier full --repeat 3 --scenarios patch_chain --out /tmp/chain-full.json
# or through the existing driver
scripts/run_perf_benchmarks.sh --mode bench-quick --build-dir /tmp/mc-rel [--compare docs/bench-baselines/quick-twic-100k.json]
```

The first scenario (`import`) builds the reference bundle (corpus plus postings plus opening tree plus one user-added game) into a cache directory (`MOTIF_BENCH_CACHE`, a fresh temporary directory per `run` unless `--cache-dir` is given); every other scenario copies or opens it instead of re-importing. Never reuse a cache across builds whose on-disk format differs. One scenario alone: `MOTIF_BENCH_PGN=bench/data/twic-100k.pgn MOTIF_BENCH_CACHE=/tmp/c /tmp/mc-rel/bin/motif_bench_test "bench: open"`.

**Compare:**

```sh
scripts/bench_compare.py compare docs/bench-baselines/quick-twic-100k.json /tmp/new-quick.json [--threshold 10]
```

The table lists every metric with its delta; the exit status is 1 if any metric regressed, 2 if the comparison is refused (build type not Release/RelWithDebInfo, or different corpora). Tolerances: 10% for `_ms`, `_us`, `_per_s` and `_ratio` (`--threshold`), 5% for `_bytes` (`--bytes-threshold`), 15% for RSS figures (`--rss-threshold`). Higher is better for `_per_s`; lower for everything else; `incremental` may never drop from 1 to 0. A change smaller than a tiny absolute floor (0.2 ms, 2 µs, 1 MiB, 0.02 ratio) is ignored, and a change is reported as `noisy` instead of a regression when the baseline's own spread, (max−min)/median, already exceeds the tolerance. Metrics present in only one file are shown as `added`/`removed` and never fail the run. Metrics without a unit suffix (counts) are informational.

**PR 6 observation (1M tier, jaghut, Release, one repetition per version):** the manual Elo/result edit's `ready_ms` fell from 58,284 ms to 309 ms, with `incremental` changing from 0 to 1 and rebuild time from 57,980 ms to zero. The timed mutation rose from 0.893 ms to 5.068 ms as publication became durable within the call; that older measurement included a game lookup. The earlier imported-edit measurement also included target sampling and is not a valid edit-call latency. The comparison flagged import ingest, multi-open navigation p99 and RSS delta, nonindexed metadata mutation and navigation p99, and first/warm start-query latency. These single samples do not establish whether those other changes persist; the full baseline has not been refreshed.

After moving game lookup and target sampling outside the timers, three 1M repetitions measured median edit-call latency of 5.10 ms for the manual correction and 7.10 ms for the imported correction. Readiness medians were 316 ms and 319 ms respectively; neither rebuilt an index.

In a three-repetition 1M recheck against the single-sample baseline, start-query p50 was 84.4 µs versus 83.6 µs and first-query latency was 232 µs versus 245 µs, clearing those initial flags. Ingest was 11.70 s versus 10.61 s; multi-open navigation p99 was 1.48 ms versus 1.31 ms and the RSS delta was 6.70 MiB versus 2.78 MiB. Nonindexed metadata mutation and navigation p99 also remained above the comparison thresholds, but varied considerably across the three runs. The baseline's single sample limits conclusions about all of these differences.

A subsequent complete three-repetition 1M run (`/tmp/pr6-final-full.json`) failed comparison against the single-sample baseline: ingest was 12.40 s versus 10.61 s, postings merge 13.66 s versus 10.78 s, and multi-open navigation p99 1.57 ms versus 1.31 ms. Several other metrics exceeded thresholds; timed mutation boundaries changed since the baseline, so those call timings are not directly comparable. The baseline binary was overwritten and the baseline source tree was dirty, preventing a controlled old-versus-new rerun. No baseline was refreshed.

An import-only five-repetition follow-up (`/tmp/pr6-final-import-recheck.json`) measured ingest at 12.57 s median (11.67–21.61 s) and postings merge at 13.26 s (12.96–19.76 s). The first repetition overlapped investigation activity. The full-run navigation p99 increase also appears in single-bundle `open` (1.55 ms versus 1.35 ms), so it cannot be attributed specifically to opening small bundles. Multi-open RSS delta was 6.70 MiB versus 2.78 MiB, but includes additional big-bundle queries and allocator effects; the saved baseline has only one sample.

On a quieter jaghut, five repetitions of `import,open,multi_open` (`/tmp/pr6-jaghut-quiet.json`) measured median ingest at 10.685 s (10.567–11.050 s), postings merge at 10.996 s (10.921–11.002 s), and single-bundle warm navigation p99 at 1.342 ms (1.297–1.410 ms). The respective single-sample baselines were 10.612 s, 10.779 s, and 1.347 ms. These results are consistent with host activity affecting the earlier timings, but do not establish a controlled before-and-after comparison. The multi-open RSS delta remained 6.70 MiB versus 2.78 MiB; its allocator and query-history confounders remain unresolved, and further investigation is deferred. This restricted rerun does not replace the full benchmark gate or justify refreshing the baseline.

On the 1M `patch_chain` scenario, three repetitions measured the 17th edit's synchronous fold at a median 5.02 s before removing the redundant staged-checkpoint validation open and 3.66 s afterward (range 3.64–3.75 s). The final-named checkpoint is still validated before commit. In the latter run, warm navigation p50 was 64.12 µs before edits and 64.21 µs after 16 runs. This cap-crossing latency is not representative of an ordinary edit.

A 1 kHz CPU-time sampler (a throwaway `SIGPROF` handler preloaded into `motif_bench_test`, because `perf record` was refused here: `perf_event_paranoid=2` and no accessible PMU) located the remaining 17th-edit cost in checkpoint decoding. In that edit's 3,353 CPU samples, `std::istream::get()` (one call, with its sentry, per byte read) was on the stack in 1,544 (46%). Of the 3,353, 1,619 (48%) were under `fold_tree_chain`, which streams the old checkpoint, and 1,367 (41%) were under the final-named checkpoint's `opening_tree_index::open`; both decode it byte by byte. `tellg()` (an `lseek` per node) took another 222, and `file_checksum` took 350 (10%). Across the whole scenario, `opening_tree_index::open` accounted for 17.5 s of the 48.6 s CPU spent inside `database_manager::open`. Checkpoint decoding now reads 64 KiB blocks (4 KiB for single-node queries) through `buffered_tree_reader` and tracks offsets with a counter. The on-disk format, every decode-time range, canonical-form and ordering check, and the final-named-checkpoint validation before commit are unchanged. Three 1M repetitions (`scripts/bench_compare.py ... --scenarios patch_chain`, same cached bundle, baseline `/tmp/pr6-chain-single-open.json`) measured the 17th edit's fold at a median 2.08 s (range 2.02–2.09 s) versus 3.66 s, `open_median_ms` at 2.79 s versus 3.72 s, and `open_baseline_median_ms` at 2.84 s versus 3.76 s. Warm navigation p50 was 62.48 µs before edits and 61.91 µs after 16 runs. In a sampled run after the change, `fold_tree_chain` took 995 of the edit's 1,777 CPU samples, with `file_checksum` at 348 (FNV-1a over the staged file, once at seal and once at publish), `read_varint` at 319 and the checkpoint writer's per-byte buffering at about 350. The writer was not changed. These are single-host figures, and the sampler attributes CPU time, not the wall time spent in `fsync`.

On the 100k tier, an earlier three-repetition recheck measured median `import.tree_root_merge_ms` at 216 ms versus the baseline's 124 ms. A later five-repetition import-only run measured 127 ms (126–143 ms), and a complete three-repetition quick run measured 126 ms; the earlier increase did not persist. The phase includes the final root spill and sort as well as the merge, and the cause of the earlier difference remains unknown. The complete quick comparison flagged post-100-insert navigation p99 at 228.8 µs versus the baseline's 206.3 µs, and a five-repetition targeted recheck measured 230.3 µs (203.3–236.3 µs). The earlier pre-change build was dirty and its binary was overwritten, so no exact before/after comparison exists.

A later investigation with a temporary out-of-tree harness (per-sample timing of 30 sweeps over the 512 sampled positions, linked against the same unmodified release libraries) found no code regression. The p99 of the 1,536 pooled samples (512 positions times 3 sweeps) is the 16th slowest sample, and the 15 slowest samples all come from the 9 of 512 sampled positions whose postings `max_ply` exceeds the tree's `max_root_ply` of 20. Those positions also occur after a transposition or piece shuffle deeper than ply 20, so `database_manager::query_unfiltered_opening_stats` answers them from exact postings instead of the tree; this fallback is unchanged from the committed code. Three of them (10,714, 4,500 and 2,847 games) take about 17, 7 and 5 ms and account for the 9 slowest samples. Six lighter ones (15–60 games) take 150–300 µs; the 16th slowest sample, the p99, lies among them. The other 503 positions are served by the tree with a per-position median p99 of 84 µs and a maximum of 94 µs. The same positions form the tail in every harness run, on a freshly opened unmutated bundle and after 100 inserts alike. The post-insert bundle is about 5% slower on those positions and about 4 µs slower at p50, as it already was in the PR 5 quick run (`insert_100` p99 202 µs versus `remove_1` 196 µs). Because the metric is a rank among a handful of 150–300 µs samples, it moves by 10–20% with host state: earlier three-repetition quick runs in the same build directory measured an `insert_100` median p99 of 202 µs, 243 µs and 209 µs, and 23 later isolated runs of the current binary on four independently imported base bundles measured a median of 206.7 µs (193.5–249.8 µs, p50 57–64 µs). Three of those 23 were at or above 228.8 µs; the two highest (240.8 and 249.8 µs) came with a p50 of 63–64 µs, the signature of the runs made with eight concurrent spinner processes, which raised p99 to 216–235 µs and p50 to 61–64 µs. The post-100-insert p99 flag is therefore treated as run-to-run noise on a stable exact-postings tail, not a regression; the baseline was not refreshed.

**Gate status for the PR 1-6 review (recorded 2026-11-10; the benchmark gate is not cleared).** The runs above support the claims below and no more. The change is reviewed with these exceptions, and no baseline was refreshed to remove any of them.

- *Baseline provenance.* Both files in `docs/bench-baselines/` were recorded on 2026-11-07 on `jaghut` from a dirty tree on `06ca306` (`git_describe` `06ca306-dirty`). That tree is the PR 5 state of this work; no commit reproduces it and its binary was overwritten. The quick baseline has three repetitions; the full baseline has one, so it has no spread of its own and no full-tier metric can be classified `noisy`. Every comparison here is PR 6 against PR 5 on one host, not against pre-PR 1 code or a committed revision.
- *The measured tree.* The earlier jaghut result files predate the final review fixes. Complete quick and full runs of the reviewed code were recorded on imass (below) and, later, on jaghut (next item); the imass runs cannot be directly compared to the jaghut baselines.
- *Lint refactors.* The jaghut and imass runs predate the subsequent clang-tidy refactors to the database manager, store, opening-tree code, and benchmark harness. The refactored staged tree passed the pre-commit hook and database, import, search, HTTP, and Slint tests, but has not been benchmarked.
- *Latest jaghut runs of the reviewed tree.* These runs on the baseline host still do not clear the gate. Quick tier: `mutate.patch_meta_1.nav_p99_us` is flagged at 215 µs against the baseline's 192 µs (+12.0%, threshold 10%); the cause is not established. Full tier: the comparison flags the deferred `multi_open.rss_delta_mib` (see *Peak RSS*) and the Elo mutation call time, whose timing boundary changed since the baseline (game lookup and target sampling moved outside the timer, and publication became durable within the call), so that flag reflects a change of definition and is not shown to be a slowdown. The target behavior holds: `patch_elo_1.ready_ms` is 309.6 ms against 58,284 ms, with `incremental=1`. Because the full baseline is a single sample from a dirty tree whose binary no longer exists (*Baseline provenance*), these results carry no controlled before-and-after attribution. No new benchmark was run for the PR description and no baseline was refreshed.
- *Quick tier.* The complete three-repetition comparison (`/tmp/pr6-quick-latest.json`) flags one metric: `mutate.insert_100.nav_p99_us`, 228.8 µs against 206.3 µs (+10.9%, threshold 10%). The five-repetition recheck median was 230.3 µs. The investigation found that an unchanged exact-postings fallback dominates the tail, but did not establish the cause of the difference. The flag remains unresolved. The targets moved as claimed: `mutate.patch_elo_1.ready_ms` 6,023 ms to 67.5 ms, `incremental` 0 to 1, rebuild time 0.
- *Full tier.* The only complete three-repetition comparison (`/tmp/pr6-final-full.json`) failed with 22 flagged metrics, and it ran while the host was busy. The quiet five-repetition rerun covered only `import`, `open` and `multi_open`: ingest 10.685 s against 10.612 s and warm navigation p99 1.342 ms against 1.347 ms, both within tolerance. `insert_single`, `mutate`, `segments` and `patch_chain` were not rerun quietly, so these flags are unresolved: `mutate.*.nav_p99_us` (+14.6% to +22.4%; the contended run's `open.nav_warm_p99_us` was also high, 1.554 ms against 1.342 ms quiet, which [INFERENCE] points to contention but was not measured for the mutate kinds); `rebuild_postings_ms` (+12.0% to +17.9%; the quiet run brought the postings merge in `import` back to within 2% of baseline); `mutate_ms` of `insert_1` (0.95 to 2.69 ms), `patch_meta_1` (0.84 to 2.70 ms) and `insert_100` (7.4 to 9.4 ms), cause not investigated and not covered by the exemption for `insert_single.single_*`; `segments.insert_{max,total}_ms` (1.0 to 2.3 ms and 1.6 to 3.1 ms); `patch_elo_1.mutate_ms` (7.7 ms, but its timing boundary changed, so it is not comparable). The plan requires a clean full-tier run before PR 6 lands; none exists.
- *Reviewed tree on imass.* Complete quick and full runs with three repetitions are in `/tmp/motif-merge-review-imass-quick.json` and `/tmp/motif-merge-review-imass-full.json` on imass. Full-tier medians: ingest 14.602 s, postings merge 14.858 s, warm navigation p99 1.704 ms, and `patch_elo_1.ready_ms` 427.14 ms with `incremental=1`. The earlier five-repetition imass run measured 14.442 s, 14.862 s and 1.676 ms for the first three metrics; it did not cover mutation. These measurements do not establish a comparison against the jaghut baseline. The first full import iteration took 162 s overall versus 91 s and 90 s for the later iterations.
- *Peak RSS.* `multi_open.rss_delta_mib` rose from 2.78 MiB (one baseline sample) to 6.70 MiB (five quiet runs, 6.70-7.14 MiB). The owner decided to defer the investigation. The cause is unknown and the rise is not shown to be benign. It is bounded by the absolute figures in the same runs: `rss_multi_mib` 423.7 to 428.8 MiB (+1.2%), `rss_big_alone_mib` 420.9 to 422.1 MiB, `open.peak_rss_mib` 425.4 to 430.7 MiB, `import.peak_rss_mib` 754.8 to 746.3 MiB. The delta is the difference of two peaks near 420 MiB, so a shift of about 4 MiB exceeds the 15% tolerance. Treat it as a known open item.
- *Evidence is not in the repository.* The `/tmp/pr6-*.json` files cited above, and the `docs/objectives-runs/` logs cited elsewhere in this file, are not part of this change; attach the former to the review if they are needed.
- *To close the gate:* on an otherwise idle host and the final tree, run the complete quick tier and the complete full tier with `--repeat 3` or more, compare against the existing baselines, then either explain each remaining flag or record it as a deliberate regression and refresh the baselines in the same change.

**Scenarios and metrics** (`BENCH <scenario>.<metric>=<number>`):

| Scenario | What it measures | Main metrics |
|---|---|---|
| `import` | fresh import with postings and tree | `wall_ms`, `games_per_s`, `ingest_ms`, `dedup_ms`, `postings_ms`, `tree_ms` (and their parts), `size_{games_db,postings,tree,total}_bytes`, `peak_rss_mib` |
| `open` | `database_manager::open` of the reference bundle (5 opens), then first queries | `open_first_ms`, `open_min_ms`, `open_median_ms`, `start_first_us`, `open_to_first_query_ms`, `start_warm_p50_us`, `nav_{cold,warm}_p{50,99}_us` over the 512 sampled positions (seed 42, same sampler as the objectives test), `peak_rss_mib` |
| `insert_single` | 20,000 standalone `insert_game` calls into an empty bundle; raw batches of 1000 (10,000 games) as import writes | `single_p50_us`, `single_p99_us`, `single_per_s` (informational), `batch_games_per_s`, `batch_commit_p50_ms`, `batch_dedup_ms` |
| `mutate` | on a fresh copy per kind: `insert_1`, `insert_100` (100 sequential `insert_game`), `remove_1`, `patch_elo_1` (Elo and result on the manual fixture game), `patch_elo_imported_1` (an Elo correction on a real imported game), `patch_meta_1` (event, site) | per kind: `mutate_ms`, `ready_ms`, `rebuild_ms`, `rebuild_postings_ms`, `rebuild_tree_ms`, `incremental`, `nav_p50_us`, `nav_p99_us`, `postings_bytes`, `tree_bytes`, `tree_patch_bytes`, `metadata_bytes`, `peak_rss_mib`; patch kinds also `patch_ms` (= `mutate_ms`, the edit call itself). Since PR 6 the two `patch_elo*` kinds are incremental: `incremental=1`, all `rebuild_*_ms` 0 |
| `patch_chain` | 1, 4, 8, 16 and 17 sequential single-game Elo corrections on a copy of the big bundle (default caps: soft 8, hard 16 runs) | `patch_<k>_ms` per edit, `n<N>.nav_{p50,p99}_us` after N edits, `n<N>.{tree,metadata}_runs`, `compact_ms` (the edit that crossed the 16-run hard cap and folded both chains: the 17th), `open_median_ms` (opening a bundle that holds the full 16-run chain) next to `open_baseline_median_ms`, `chain_{tree_patch,metadata}_bytes`, `baseline_nav_p50_us`, `peak_rss_mib` |
| `multi_open` | the big bundle plus 8 small ones (200 games each) open in one process | `rss_delta_mib` (small bundles vs. big alone), `rss_multi_mib`, `sqlite_cache_in_use_bytes` (from `resource_governor::process()`), `big_{start,nav}_p50_{alone,with_small}_us`, `small_start_p50_us` |
| `segments` | 10 sequential single-game inserts on the big copy, then queries versus the unmutated baseline | `insert_p50_ms`, `ready_ms`, `rebuild_ms`, `incremental`, `{start,nav}_degradation_ratio`, `degradation_ratio` (the larger of the two), `postings_files`, `tree_files` |

`mutate` and `segments` define `ready_ms` as the wall time from the start of the mutation until the start-position `opening_stats::query` and a full pass of `opening_tree` navigation (open with prefetch depth 0, then expand) over the 512 sampled positions are answerable again. An insert or removal still advances the bundle's mutation sequence without publishing a matching snapshot, which leaves the last published index stale, so the scenario detects that (the start query fails, or `database_manager::derived_index_status()` reports the tree as not usable -- a stale snapshot keeps its manifest entries, so the presence of an entry proves nothing) and calls `rebuild_position_postings()` / `rebuild_opening_tree_index()` inside the timed window, reporting them as `rebuild_*_ms` with `incremental=0`. It never assumes a rebuild is needed: once a mutation is incremental it finds nothing to rebuild, reports `incremental=1`, and `ready_ms` collapses with no change to the benchmark. That is the case since PR 6 for `patch_elo_1` and `patch_elo_imported_1` (a result or rating correction publishes its metadata after-image run and signed tree run in the edit's own FULL commit; `ready_ms` then holds the storage latency of that commit plus the navigation pass, not only the patch call) and has always been the case for `patch_meta_1` (event/site patches do not touch the indexes and do not advance the sequence). An edit that crosses the 16-run hard cap folds the chain into a new checkpoint inside the edit (a checkpoint-sized synchronous write under the generation lock, not a rebuild or a replay); `patch_chain.compact_ms` discloses that cost separately so it is not hidden inside `incremental=1`. The `insert_100` kind uses 100 sequential `insert_game` calls, the only multi-insert API on the manager; revisit it when a manager-level batch insert exists.

Each `mutate` kind first serves one navigation pass on its freshly opened copy and only then mutates, because a bundle edited in real use has been queried. Observed on the 100k corpus (Release, 32-core host): without that pass the first `rebuild_position_postings()` in a process took about 18 s, versus about 4.7 s for every later rebuild or for a rebuild after queries; the cause was not investigated. `MOTIF_BENCH_KINDS=insert_1,remove_1` restricts the kinds that `mutate` runs; each skipped kind saves one full index rebuild (the 1M-game build phases in `docs/objectives-runs/master-*-1.log` sum to about 39 s for postings and 29 s for the tree).

**When to refresh the baseline.** Baselines live in `docs/bench-baselines/` (`quick-twic-100k.json`, and `full-twic-1m.json` once the full tier has been run). Refresh one only on purpose, never to silence a failing comparison: (1) when a PR deliberately changes a metric's behavior (for example mutations becoming incremental flips `incremental` and collapses `ready_ms`) and the new numbers are reviewed and explained in the PR; (2) after a toolchain, compiler flag, or machine change, since absolute timings are only comparable on the same host (the file records host, compiler, `nproc`, corpus and `git describe --dirty`); (3) before starting a new PR in the incremental-index sequence, so each PR is compared against its predecessor. Record the run with `--repeat 3` or more on an otherwise idle machine, and commit the JSON together with the change that explains it.

---

## 2026-04-24 — Import Strategy Decision

**Machine:** Linux, Clang 21, perf-release build (`-O3 -DNDEBUG -march=x86-64-v3`)

### Import Pipeline — 3.41M games (`bench/data/twic-all.pgn`)

| Benchmark | Attempted | Committed | Skipped | Errors | Import elapsed | Wall | User (s) | Sys (s) | CPU% | Peak RSS |
|---|---|---|---|---|---|---|---|---|---|---|
| Default fast path (`write_positions=false`, sorted rebuild) | 3,411,924 | 3,381,047 | 30,877 | 9,134 | 303,384 ms | 5:04.62 | 448.63 | 145.80 | 195% | 17,983,084 KB |

### Decision

- Direct position writes were removed: they were dramatically slower and did not provide enough practical value over deferred rebuild.
- Partitioned rebuild was evaluated as a replacement candidate, but after making it preserve the normal final `position` table semantics it was still slower than the default path and no longer showed a convincing memory advantage.
- **Default fast path remains the only supported import strategy.**

---

## 2026-04-22 — Story 2.11 post-review (num_workers=4 fixed default, idempotent sort_by_zobrist)

**Machine:** Linux, Clang 21, release build

### Import Pipeline — 10k games

| Benchmark | Wall (s) | User (s) | Sys (s) | CPU% |
|---|---|---|---|---|
| Default fast path (4w, 64L, write_positions=false, sorted rebuild) | 0.82 | 1.07 | 0.33 | 171% |
| Serial fast path candidate (1w, 1L, write_positions=false, sorted rebuild) | 0.91 | 0.91 | 0.09 | 109% |
| Pipeline mode (4w, write_positions=true) | 9.59 | 7.41 | 0.69 | 84% |
| Serial mode (1w, write_positions=true) | 8.32 | 5.62 | 0.39 | 72% |
| SQLite-only serial | 0.40 | 0.36 | 0.03 | 98% |
| SQLite + rebuild (sorted, default) | 0.91 | 0.96 | 0.10 | 115% |
| SQLite + rebuild (unsorted, no index) | 0.85 | 0.77 | 0.07 | 99% |
| SQLite + rebuild (sorted, no index) | 0.90 | 0.91 | 0.10 | 111% |
| Rebuild-only | 0.92 | 0.98 | 0.08 | 116% |
| DuckDB no-index serial (1w, write_positions=true) | 8.37 | 5.61 | 0.42 | 72% |
| SQLite + partitioned rebuild | 0.87 | 0.82 | 0.07 | 102% |

### Import Pipeline — 1M games

| Benchmark | Wall | User (s) | Sys (s) | CPU% |
|---|---|---|---|---|
| Default fast path (4w, 64L, sorted rebuild) | 1:25 | 124.51 | 39.97 | 193% |
| Serial fast path candidate (1w, 1L, sorted rebuild) | 1:39 | 111.03 | 15.29 | 127% |
| SQLite-only serial | 45.5s | 35.80 | 8.86 | 98% |
| SQLite + rebuild (unsorted, no index) | 1:29 | 79.43 | 10.21 | 101% |
| SQLite + rebuild (sorted, no index) | 1:36 | 104.16 | 14.23 | 123% |
| Rebuild-only | 1:37 | 104.94 | 14.33 | 123% |

### Query Latency — 1M games, 82.3M positions, 192 sampled hashes

| Mode | p50 (µs) | p99 (µs) | min (µs) | max (µs) | total (ms) |
|---|---|---|---|---|---|
| Unsorted | 14,438 | 50,061 | 13,771 | 57,711 | 3,032 |
| Sorted by zobrist | 646 | 28,715 | 508 | 38,523 | 274 |

### Key Observations

- **Default fast path wins over serial**: 1:25 vs 1:39 wall time on 1M games (~14% faster)
- **Sorted-by-zobrist gives 22× query latency improvement**: 646µs p50 vs 14,438µs p50
- **SQLite-only is the floor**: 45.5s for 1M games; total pipeline cost is rebuild + SQLite overhead
- **write_positions=false fast path is the production default**: avoids per-move DuckDB overhead during import

---

## 2026-04-22 — Story 3.1 position_search API

**Machine:** Linux, Clang 21, release build

### Position Search API — 1M games, sorted-by-zobrist store, 199 sampled hashes

| Benchmark | Wall | User (s) | Sys (s) | CPU% | p50 (µs) | p99 (µs) | min (µs) | max (µs) | total (ms) |
|---|---|---|---|---|---|---|---|---|---|
| `position_search::find` on sorted position store | 1:24 | 118.66 | 41.51 | 189% | 656 | 13,077 | 522 | 16,211 | 201 |

### Notes

- This benchmark exercises the public `motif_search` API, not the lower-level `motif_db::position_store` query directly.
- Corpus used for the recorded run (via `MOTIF_IMPORT_PERF_PGN`): `/home/bogdb/scid/twic/1m_games.pgn`
- To build a repo-local benchmark corpus, run `scripts/download_twic_pgns.sh --1m` or `scripts/download_twic_pgns.sh --all` and let the perf tests discover the repo-local output automatically.
- Result is comfortably within the Story 3.1 directional guardrail (`p99 < 100ms`) on the available 1M corpus.
- **Remaining AC gap:** the formal acceptance target is `<100ms P99` on a **10M-game** corpus. That has not been fully validated yet because no local 10M corpus benchmark run was performed in this story.

---

## 2026-04-23 — Story 3.2 opening_stats API

**Machine:** Linux, Clang 21, release build (`-O3 -DNDEBUG`)

### Quick Guardrail — 100k games (`bench/data/twic-100k.pgn`)

| Benchmark | Elapsed / total | p50 (µs) | p99 (µs) | min (µs) | max (µs) |
|---|---|---|---|---|---|
| `import_pipeline: default fast path perf` | 8,319 ms | - | - | - | - |
| `opening_stats::query` on sorted position store | 203.996 ms | 545 | 81,185 | 457 | 81,185 |
| `position_search::find` on sorted position store | 103.585 ms | 485 | 1,548 | 366 | 3,218 |

### Full Guardrail — 1,001,092 games (`bench/data/twic-1m.pgn`)

| Benchmark | Elapsed / total | p50 (µs) | p99 (µs) | min (µs) | max (µs) |
|---|---|---|---|---|---|
| `import_pipeline: default fast path perf` | 89,992 ms | - | - | - | - |
| `opening_stats::query` on sorted position store | 312.013 ms | 797 | 116,669 | 633 | 116,669 |
| `position_search::find` on sorted position store | 222.802 ms | 681 | 20,023 | 564 | 41,963 |

### Post-review optimization — 1,001,092 games (`bench/data/twic-1m.pgn`)

Elo now sourced from DuckDB directly (no SQLite player JOINs for Elo). Batch context query drops two player JOINs. Correlated subquery for Opening tag retained (safer than LEFT JOIN). Sampling now uses `REPEATABLE (seed)` for deterministic results.

| Benchmark | p50 (µs) | p99 (µs) | min (µs) | max (µs) |
|---|---|---|---|---|
| `opening_stats::query` on sorted position store (run 1) | 730 | 321,000 | 582 | 321,000 |
| `opening_stats::query` on sorted position store (run 2) | 760 | 706,000 | 590 | 706,000 |

### Disk Size — 818,000 games (deduplicated from 1M PGN)

| Component | Size | Bytes/game |
|---|---|---|
| PGN source (`1m_games.pgn`) | 824 MB | ~1,031 |
| SQLite (`games.db`) | 371 MB | ~465 |
| DuckDB positions (estimated) | ~590 MB | ~739 |
| **Total bundle (measured)** | **371 MB** | **~465** |

The DuckDB position table was empty in the measured test db (data not checkpointed after `sort_by_zobrist`). The estimated ~590 MB is derived from ~40 position rows/game × 19 bytes/row, before DuckDB columnar compression. For comparison, scidb's TWIC database with 3.4M games occupies 1.3 GB (~400 bytes/game, positions in-memory only).

### Notes

- The public perf guard is in `test/source/motif_search/opening_stats_test.cpp` and follows the same discovery path as Story 3.1.
- Functional coverage for continuation aggregation, null-Elo averaging, ECO/opening-name lookup, and no-match behavior is in place and passes under both `dev` and `dev-sanitize`.
- `sample_zobrist_hashes` now accepts a `seed` parameter (default 0) and uses DuckDB's `REPEATABLE (seed)` clause, eliminating run-to-run P99 variation caused by different sampled positions. Tests use seed 42.
- P99 latency previously varied between runs (321–706ms) because unseeded reservoir sampling could select extremely popular positions (e.g., after 1.e4) with thousands of games. With a fixed seed, results are reproducible.
- p50 is consistently ~730–760µs, which is fast. The p99 tail is dominated by a handful of high-fanout positions.
- **Remaining NFR02 gap:** The formal acceptance target is `<500ms P99` on a 10M-game corpus. On the current 1M corpus, p99 can exceed 500ms for the most popular positions. The remaining bottleneck is per-game SQLite context fetch (moves blob + eco + opening_name). A future optimization could extract just the continuation byte at offset `2*ply` from each blob rather than deserializing the entire moves vector.

---

## 2026-08-16 — Materialized Opening Continuation Rollup

**Machine:** Linux, Clang 21, isolated Release build without clang-tidy/cppcheck.

### Opening Explorer — 989,967 games (`bench/data/twic-1m.pgn`)

| Benchmark | p50 (µs) | p99 (µs) | min (µs) | max (µs) | total (ms) |
|---|---:|---:|---:|---:|---:|
| `opening_stats::query` with rollup ordered by root hash | 2,949 | 6,744 | 2,289 | 6,744 | 323 |

| Benchmark | Total games | Continuations | Elapsed (ms) |
|---|---:|---:|---:|
| Starting-position explorer | 989,967 | 20 | 19.5 |

### Notes

- The rollup materializes direct edge statistics by `(root_hash, encoded_move, child_hash)` during position-store rebuild.
- The existing HTTP `frequency` field remains the count of games reaching the child hash through any move order. The new `direct_frequency` field reports games that selected the displayed continuation.
- The previous 1M-game runs recorded p99 values of 321-706ms for the dynamic aggregate. This run reduces the sampled p99 to 6.7ms, but its p50 is higher than the former ~0.7ms because every query now reads the compact materialized relation and reconstructs response context.
- Results are on the available 1M corpus. They do not establish the 10M-game performance target.
- The P99 `CHECK` is only enforced on release builds; dev builds emit `WARN` instead.

---

## 2026-08-17 — Import Pipeline Throughput (parser migration, worker scaling, SQL fold)

**Machine:** Linux, 32-core, Clang 21, `build/perf-release` (Release, matches `skip_perf_unless_release_build()` gating). Not yet re-measured on lower-core-count hardware. Full narrative and diagnosis in `docs/handoffs/2026-08-17-opening-explorer-storage.md`, "Session 8".

### Single-threaded ingest, isolating the parser migration (`pgn::parse_string` → `pgn::import_stream`)

| corpus | before | after | change |
|---|---:|---:|---:|
| 100k games | 7,831 ms | 5,790 ms | −26.1% |
| 1M games | 102,734 ms | 83,245 ms | −19.0% |

Per-game component breakdown, 100k corpus, before this session's changes:

| component | µs/game |
|---|---:|
| PGN read + SAN parse | 35.0 |
| + SQLite write | +12.5 |
| + Zobrist hash + position rows + DuckDB append | +31.8 |
| Scidb, whole import (reference) | 35.5 |

### Full pipeline, 3.41M games (`bench/data/twic-all.pgn`), 16 workers (`hw/2` default)

| | attempted | committed | ingest | sort | rollup | total |
|---|---:|---:|---:|---:|---:|---:|
| Before this session | 3,411,924 | 3,363,425 | 347,560 ms | 7,665 ms | 28,621 ms | 386,571 ms |
| After this session | 3,411,919 | 3,366,774 | 298,124 ms | 5,036 ms | 29,855 ms | 336,995 ms |

**−12.8% wall-clock, +3,349 games committed.** The extra commits are verified as a correctness improvement, not leniency: `pgn::parse_string`'s strict-UTF-8 lexy grammar was rejecting entire well-formed games over a single non-UTF-8 byte in an unrelated tag (e.g. `WhiteTeam`); `import_stream` doesn't validate encoding and accepts them. See the handoff doc for the verification method (lockstep `game_stream` vs `import_stream` diagnostic).

### DuckDB appender throughput, isolated

`position_store::insert_batch` alone (no PGN, no SQLite, no file I/O), synthetic 100k games × 87 rows/game (matching real median game length):

| | value |
|---|---:|
| ns/row | 225.9 |
| µs/game (87 rows) | 19.65 |

At `hw/2` workers, 1M games: SQLite-write-only ingest is 24.2 µs/game; full ingest (+ position build + DuckDB append) is 76.0 µs/game. The isolated appender number accounts for ~38% of that ~51 µs/game gap — the rest (vector copy, any non-hidden zobrist-hash cost) is not yet isolated further.

### DuckDB appender: scalar → vectorized chunk-append

`position_store::insert_batch` rewritten from row-by-row `duckdb_appender_begin_row`/`append_*`/`end_row` (6 API calls/row) to `duckdb_create_data_chunk` + direct pointer writes into each vector + `duckdb_append_data_chunk` (~2048 rows/chunk). `position` has no nullable columns, so no validity-mask handling needed.

| benchmark | before | after | change |
|---|---:|---:|---:|
| isolated appender (synthetic, 100k games × 87 rows) | 225.9 ns/row | 178.2 ns/row | **−21.1%** |
| 1M, serial ingest | 83,245 ms | 79,124 ms | −5.0% |
| 1M, 16 workers, ingest | 75,382 ms | 71,578 ms | −5.0% |
| 3.4M, 16 workers, total elapsed | 336,995 ms | 332,794 ms | −1.25% |

Unlike the parser migration, this still helps at full 16-way parallelism (~5% at 1M) since it targets stage2's actual serial bottleneck — but smaller than the isolated 21% suggests, and smaller again at 3.4M scale where sort/rollup are a larger share of total time.

**Cumulative, this whole investigation, 3.4M-game corpus:** 386,571 ms → 332,794 ms total elapsed — **13.9% faster wall-clock**, plus 3,349 more games committed (verified correctness fix, see handoff doc).

### Notes

- Parser migration win is largest single-threaded (−19% to −26%) and nearly disappears at 16-way parallelism (75,382 ms vs 75,651 ms at 1M) — once parsing is cheap, the serial SQLite-write + DuckDB-append stage becomes the pipeline's ceiling regardless of parser speed.
- `num_workers` default changed from a hardcoded `4` to `hardware_concurrency() / 2` (floor 1, no cap) — only validated on this 32-core box.
- SQL-fold (single-statement duplicate-check-and-insert in `game_writer::insert`) and SQLite pragma tuning (`synchronous=NORMAL`, `cache_size`, `mmap_size`, `wal_autocheckpoint`) were tried first; both real but small (~1.3% and ~1.7% respectively) since neither was the dominant cost.
- `import_stream` holds the whole PGN file in memory (vs. the old line-by-line reader); isolated ingest-only peak RSS delta is ~1.1 GB on the 896 MB 1M-game corpus (so ~3 GB on the full 3 GB `twic-all.pgn`) — proportional to file size, not the dominant contributor to the ~12.7 GB peak RSS measured for the full default path (sort/rollup phase, not isolated further, is the more likely cause).
- **Open question, not a settled one:** the parser migration (`import_stream`) reportedly wasn't adopted earlier specifically because of RAM cost and negligible speed impact at production parallelism. This session's numbers corroborate the speed point at `hw/2`+ workers (nearly flat) and confirm real RAM cost proportional to file size. It does still deliver a real single-threaded/low-core win and fixes a strict-UTF-8 game-rejection bug (see handoff doc) — whether that's worth the RAM cost is a product call, not decided here.

---

## 2026-08-20 — SQLite Game Store and Compact Opening Index

**Machine:** Linux, 32-core, Clang 21, `build/perf-release`. Corpus:
`bench/data/twic-1m.pgn` (896 MiB). Runs were serialized.

| Path | Attempted | Committed | Ingest | Replay | Sort | Rollup / index build | Total | Bundle |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Normal import: SQLite + DuckDB positions | 1,001,090 | 991,278 | 10,753 ms | 14,182 ms | 7,157 ms | 7,289 ms rollup | 48,555 ms | not retained |
| Candidate: SQLite + `opening_tree.idx` | 1,001,090 | 991,278 | 18,181 ms | - | - | 28,525 ms index build | 46,706 ms | 520 MiB |

The candidate stores `games.db` in 278 MiB and `opening_tree.idx` in 242 MiB.
It was 1,849 ms (3.8%) faster than the same-run normal import while producing
a 520 MiB retained bundle. The normal path's separately measured retained
bundle is about 2.28 GiB (`games.db` plus `positions.duckdb`), so the candidate
is about 4.4x smaller.

### Compact Index Query Latency

`opening_tree_index::query_opening_stats` was sampled at every opening ply
through ply 20 for 2,000 imported games.

| Queries | Rows | Mean | p50 | p99 | Max |
|---:|---:|---:|---:|---:|
| 41,822 | 368,729 | 0.53 us | 0.55 us | 1.24 us | 8.85 us |

### Scope

The compact index is still a prototype replacement only for unfiltered,
shallow opening-explorer queries. DuckDB remains required for position search,
filtered explorer queries, Elo distributions, and full-depth traversal. The
index build is intentionally not wired into the normal import path.

---

## 2026-08-22 — Compact Position Postings and Opening Explorer

**Machine:** Linux, 32-core, Clang 21, `build/perf-release`. Corpus:
`bench/data/twic-1m.pgn`. The DuckDB, postings, and postings-plus-tree imports
ran serially in one process. Results are one detailed run after three earlier
postings-format validation runs established a 67.409-67.962 s range.

### Total Import

| Path | Attempted | Committed | Total | Peak RSS | Derived artifacts |
|---|---:|---:|---:|---:|---:|
| SQLite + DuckDB | 1,001,090 | 991,278 | 60,116 ms | 9,475 MiB | 2,039 MiB DuckDB |
| SQLite + exact postings | 1,001,090 | 991,278 | 68,323 ms | 1,238 MiB | 1,206 MiB postings |
| SQLite + postings + shallow tree | 1,001,090 | 991,278 | 126,282 ms | 1,452 MiB | 1,206 MiB postings + 242 MiB tree |

The exact-postings path is the current DuckDB-free position-query import. The
tree remains opt-in, so 68.323 s is the current default derived-index total when
DuckDB rebuilding is disabled. Building both indexes adds 57.959 s.

### Import Components

| Path | Ingest | Dedup | Derived wall | Accounted builder phases | Validation/publication and other remainder |
|---|---:|---:|---:|---:|---:|
| DuckDB | 14,650 ms | 8,953 ms | 35,261 ms | replay 18,000 + sort 8,886 + rollup 8,375 ms | 1,250 ms |
| Exact postings | 14,590 ms | 8,976 ms | 44,755 ms | 38,369 ms | 6,386 ms |
| Postings + tree | 14,478 ms | 8,882 ms | 102,921 ms | postings 38,369 + tree 60,356 ms | 4,196 ms |

Builder-local postings phases were 12,070 ms replay, 6,796 ms spill, 16,980 ms
merge, 76 ms metadata write, 345 ms directory write, and 2,102 ms final write.
The build indexed 88,221,427 occurrences across 68,907,007 hashes in 85 spill
runs. The output comprised 420,327,977 posting bytes, 9,912,780 metadata bytes,
828,439,768 compressed-directory bytes, and 5,921,696 sparse-directory bytes.

Builder-local tree phases were 22,947 ms replay, 916 ms root merge, 6,150 ms
edge merge, 11,978 ms child-frequency merge, about 6,264 ms child-frequency
loading, and 12,101 ms final index write.

### Opening Explorer

The raw immutable tree reader sampled 41,822 positions through ply 20:

| API | Queries | Mean | p50 | p99 | Max |
|---|---:|---:|---:|---:|---:|
| `opening_tree_index::query_opening_stats` | 41,822 | 5.36 us | 4.77 us | 10.50 us | 37.78 us |

The public `opening_stats::query` API sampled 512 unique non-starting shallow
positions twice:

| Pass | Mean | p50 | p99 | Max |
|---|---:|---:|---:|---:|
| First | 1,209.25 us | 963.95 us | 3,021.06 us | 85,460.71 us |
| Warm | 1,193.23 us | 937.89 us | 3,039.12 us | 84,501.05 us |

The starting-position public query took 3,233.676 ms on the first call and
3,191.127 ms warm p50. This is not tree-reader latency. The public API first
calls `position_game_ids()` to determine `total_games`, which decodes the
starting position's approximately one-million-game posting block before using
the shallow aggregate. Repeated calls remain equally slow because this path has
no result cache. The next explorer optimization should use the postings
directory's `distinct_game_count` summary for unfiltered `total_games`, avoiding
posting-block decode entirely.

### Complete Starting-Position Root Follow-up

The summary-count change removed one redundant posting decode but did not fix
the starting position: repetitions can return to that position after ply 20,
so the manager correctly rejected the shallow aggregate and used full replay.
Opening-tree format v5 now marks the starting node complete and aggregates its
edges at every ply. Two serialized 1M Release runs measured:

| Metric | Run 1 | Run 2 |
|---|---:|---:|
| Starting position first | 1.066 ms | 1.084 ms |
| Starting position warm p50 | 0.898 ms | 0.926 ms |
| Non-starting warm p50 | 0.881 ms | 0.888 ms |
| Non-starting warm p99 | 1.439 ms | 1.430 ms |
| Postings + tree total import | 128.421 s | 128.276 s |
| Tree builder-local time | 61.847 s | 61.716 s |
| Tree artifact | 246 MiB | 246 MiB |

Compared with the preceding 3.1-3.2 second starting-position measurements, the
complete root improves warm latency by roughly 3,400x. The tree costs about 4
MiB more storage and approximately one additional second of builder-local work.

### Follow-up — Unfiltered `total_games` via postings summary

`opening_stats::query`'s unfiltered path now calls `database_manager::position_summary()`
first and uses its `distinct_game_count` for `total_games` whenever a valid,
non-stale postings generation covers the queried hash; it decodes the hash's
posting block only through the pre-existing fallback (`position_game_ids()`),
which still runs when postings are absent, stale, or do not index the hash.
This removes the full posting-block decode from the previously measured
3.2 s starting-position path in `source/motif/search/opening_stats.cpp`. The
1M-corpus benchmark above was not re-run to produce a new number for this
entry; correctness and dispatch are covered by
`test/source/motif_search/opening_stats_test.cpp` (`[postings]` tag) and the
existing `database_manager::position_summary` valid/stale/absent coverage in
`test/source/motif_db/position_postings_test.cpp`.

### Follow-up — Reuse postings counts during tree construction

The manager-backed tree build now consumes the postings index's sorted
`distinct_game_count` summary stream for child transposition frequencies. It
does not construct the tree's independent full-depth visit counter; standalone
tree builds retain that path as an oracle. Two serialized 1M Release runs
measured:

| Metric | Previous | Run 1 | Run 2 |
|---|---:|---:|---:|
| Postings + tree import | 128.276-128.421 s | 109.225 s | 109.060 s |
| Tree accounted phases | about 55.5 s corrected baseline | 37.035 s | 37.242 s |
| Tree replay | about 23.0 s | 11.590 s | 11.747 s |
| Child count materialize/load | about 18.2 s merge + load | 6.396 s | 6.352 s |
| Child spill runs | 84 | 0 | 0 |
| Tree artifact | 246 MiB | 246 MiB | 246 MiB |
| Process peak RSS | 1,452 MiB detailed baseline | 1,427 MiB | 1,514 MiB |

The tree's accounted phases improve by about 33%; full postings-plus-tree
import is about 15% faster. The corrected phase totals subtract child-frequency
loading from the old `index_write_elapsed`, which previously included and
therefore double-counted that work. Raw and public query latency remained
within the preceding range. The counts are semantically identical: postings
group occurrences by game and hash, while the standalone tree counter
deduplicates each hash once per game. Build-time validation rejects missing
counts and counts below a direct edge's distinct-game frequency.

### Follow-up — Buffered final tree encoding

The final tree encoder now accumulates fixed-width and varint bytes into a 64
KiB block before writing. The binary format and reader are unchanged. Three
serialized 1M Release runs measured a combined load-plus-write phase of
10.981-11.081 seconds. With corrected non-overlapping metrics, the detailed run
reported 6.282 seconds loading child frequencies and 4.799 seconds encoding and
writing the tree. The preceding unbuffered runs imply 5.692-5.745 seconds of
serialization after subtracting their 6.352-6.396 second loads, so buffering
improves the targeted phase by about 16%.

The complete corrected tree phase total was 36.390 seconds, versus
37.035-37.242 seconds before buffering. End-to-end postings-plus-tree import
was 108.591-108.860 seconds, compared with 109.060-109.225 seconds before; this
small total improvement is close to run variance. Artifact size remained 246
MiB and query latency remained in the preceding range. `index_write_elapsed`
remains inclusive of child lookup/loading so it has the same meaning in the
preload and low-memory binary-search paths; serialization is derived only for
the preload benchmark by subtracting `child_frequency_load_elapsed`.

### Follow-up — Per-game tree scratch reuse

The tree builder now retains per-game edge/root hash-container capacity across
games. Two initial serialized 1M runs measured replay at 10.929-11.074 seconds,
down from the buffered-writer baseline of 11.836-11.942 seconds (6.5-8.5%).
Later runs alongside directory instrumentation measured 11.003-11.218 seconds.
The builder also latches an accumulation failure so partial spill/counter state
cannot be reused or finalized.

### Follow-up — Position-postings directory v6

Exact v5 instrumentation attributed the 828,439,768-byte directory to:

| Field | Bytes |
|---|---:|
| Hash deltas | 403,486,860 |
| Posting-offset deltas | 68,697,012 |
| Posting lengths | 68,966,408 |
| Occurrence counts | 68,919,729 |
| Distinct-game counts | 68,919,693 |
| Minimum plies | 72,266,086 |
| Maximum plies | 72,338,956 |

Format v6 removes the derivable offset deltas, stores 40-bit hash-delta lows
with sparse high-part exceptions, and omits occurrence counts when they equal
distinct-game counts. Three serialized 1M runs produced the same exact section
sizes:

| Metric | v5 | v6 | Change |
|---|---:|---:|---:|
| Directory | 828,439,768 B | 649,481,128 B | -21.6% |
| Full postings artifact | 1,206 MiB | 1,035 MiB | -14.2% |
| Peak temporary estimate | 2,368 MiB | 2,198 MiB | -170 MiB |
| Postings-only import | about 68-70 s | 67.614-68.346 s | modest improvement |
| Postings + tree import | about 108.6-109.9 s | 107.028-107.471 s | modest improvement |

The v6 directory contains 4,845,024 bytes of block headers, 17,226,752 bytes
of bitmaps, 344,317,077 bytes of hash encoding, 68,966,408 bytes of posting
lengths, 601,132 bytes of occurrence exceptions, 68,919,693 bytes of
distinct-game counts, and 144,605,042 bytes of ply bounds. Query semantics,
one-block directory lookup, sorted summary streaming, the 420,327,977-byte
posting payload, and the 9,912,780-byte game-metadata section are unchanged.

### Follow-up - Exact postings as the import default

The production inventory confirmed that exact and filtered position search,
opening statistics, Elo distributions, and deep bounded traversal can all use
the published postings generation. Stale or absent postings fail closed unless
the DuckDB table is known to be synchronized, while explicit DuckDB builds and
existing DuckDB-only bundles retain their fallback behavior.

`import_config {}` now builds exact postings without first rebuilding DuckDB.

Three isolated fresh-process Release runs on `bench/data/twic-1m.pgn` measured:

| Run | Attempted | Committed | Ingest | DuckDB replay/sort/rollup | Total | Attempted throughput |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 1,001,090 | 991,278 | 14.566 s | 0 / 0 / 0 ms | 81.359 s | 12,305 games/s |
| 2 | 1,001,090 | 991,278 | 14.632 s | 0 / 0 / 0 ms | 81.520 s | 12,280 games/s |
| 3 | 1,001,090 | 991,278 | 14.635 s | 0 / 0 / 0 ms | 81.728 s | 12,249 games/s |

The isolated median is 81.520 seconds, 20.6% faster than the old literal
default's 102.734 seconds. A same-session serialized comparison measured DuckDB
at 59.847 seconds, postings at 66.959 seconds with 1,110 MiB peak RSS, and
postings plus the opening tree at 105.280 seconds. Its postings builder spent
12.174 seconds replaying, 6.882 seconds spilling, 16.857 seconds merging, and
2.105 seconds in metadata, directory, and final writes; the derived-path
remainder was 5.343 seconds.

The 14.561-second gap between the isolated median and the comparison's postings
case is larger than normal variance and appears sensitive to process context or
run order. It is not yet attributed. Use the isolated median as the production
default result; use the comparison only for contemporaneous path and peak-RSS
comparisons.

### Release-boundary dependency removal

DuckDB was subsequently removed from source, CMake, Nix, and vcpkg. The change
does not alter the postings format or the measurements above. Legacy bundles
without valid postings rebuild them from canonical SQLite and leave any old
`positions.duckdb` file untouched. Scratch and oracle tests now exercise the
same SQLite-plus-postings architecture as persistent bundles.

---

## 2026-08-28 — Post-Epic-9 Import Regression Check

**Machine:** Linux, 32-core, Clang 21, `build/perf-release` (reconfigured
ad hoc without `clang-tidy` for this run only — a pre-existing clang-tidy
false positive on Catch2's own `ResultDisposition` flag-enum bitwise-OR in
`opening_tree_index_perf_test.cpp` blocks a clean lint build; unrelated to
this session's changes, not investigated further here). Corpus:
`bench/data/twic-1m.pgn`, 1,001,090 attempted / 991,278 committed for every
entry below. Full `test/source/motif_import/import_pipeline_test.cpp`
`[performance]` suite (40 cases), run serially (`-j1`).

### Trigger

A ~5k-game import felt slow through the Slint desktop app during a manual
compositor-check session (Story 9.5, AC-9-5-2). That observation was through
`build/dev` (Debug, `-O0`) over a nested-Wayland/waypipe-forwarded display —
not a valid perf signal on its own. This run re-measures the same import
pipeline the proper way, isolated on `perf-release`, to check whether Epic 9
(Slint migration) or anything since actually regressed import throughput.

### Total Import

| Benchmark | Elapsed |
|---|---:|
| `import_pipeline: default fast path perf` | 78,034 ms |
| `import_pipeline: serial fast path candidate perf` | 92,083 ms |
| `import_pipeline: wide parallel (8 workers) perf` | 77,036 ms |
| `import_pipeline: wide parallel (16 workers) perf` | 78,967 ms |
| `import_pipeline: wide parallel (24 workers) perf` | 78,947 ms |
| `import_pipeline: wide parallel (32 workers) perf` | 79,610 ms |
| `import_pipeline: ingest-only serial perf` | 35,671 ms |
| `import_pipeline: ingest plus postings rebuild perf` | 88,973 ms |
| `import_pipeline: serial postings build perf` | 89,634 ms |
| `import_pipeline: postings rebuild-only perf` | 52,750 ms |

### Peak RSS

| Benchmark | Elapsed | Baseline RSS | Peak RSS | Delta |
|---|---:|---:|---:|---:|
| `import_pipeline: ingest-only peak RSS on 1M` | 23,568 ms | 8 MB | 565 MB | 556 MB |
| `import_pipeline: rebuild path peak RSS on 1M` | 23,696 ms | 8 MB | 660 MB | 651 MB |
| `import_pipeline: inline path peak RSS on 1M` | 75,664 ms | 8 MB | 662 MB | 654 MB |

### Verdict: no regression

The only same-methodology historical baseline for today's default path
(postings-only, no DuckDB — `import_config {}`'s default since the
"Exact postings as the import default" entry above) is that same entry's
isolated-median **81.520 s** on this corpus. Today's **78.034 s** is
~4.3% *faster*, within normal run-to-run variance at worst, not slower.

The much larger-looking deltas against the 2026-08-20/2026-08-22 entries
above (48.6 s / 60.1 s / 68.3 s) are not regressions — those measured a
different code path (DuckDB rollup, or early postings before the "exact
postings as import default" change) and are not comparable to today's
default. Every test in the suite stayed well inside the project's
calibrated ceiling (`import_perf_limit_ms` = 120,000 ms; worst observed was
92,083 ms).

**Conclusion:** the slow ~5k-game import observed through the Slint app was
explained by the Debug build and nested-Wayland-forwarding overhead used for
that manual check, not an import pipeline regression from Epic 9 or later
work.
