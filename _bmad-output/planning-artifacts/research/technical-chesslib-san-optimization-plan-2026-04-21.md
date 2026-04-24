---
date: '2026-04-21'
type: 'technical-design-note'
topic: 'Upstream chesslib SAN optimization plan'
status: 'proposed'
related:
  - bench/san_ab/main.cpp
  - _bmad-output/implementation-artifacts/2-6-adopt-upstream-chesslib-san-optimization.md
  - _bmad-output/implementation-artifacts/2-8-reprofile-and-tune-remaining-import-storage-path.md
  - flake.lock
---

# chesslib SAN Optimization Plan

## Purpose

This note captures the next recommended optimization work for upstream `foolnotion/chesslib` after the latest motif-chess profiling and the direct A/B SAN benchmark against `~/src/chess-library`.

The goal is not to replace `chesslib`. The goal is to use `chess-library` as a measured upper bound on plausible near-term SAN-path headroom, then prioritize the highest-value upstream improvements in `chesslib`.

## Inputs

### Pinned Revision Under Test

Motif-chess currently uses:

- `foolnotion/chesslib@97fd1b5679a4c0871b5b3a40b441e81645fd9770`

### Current Hot Path in Motif-Chess

Motif-chess import calls:

- `chesslib::san::from_string(board, node.san)`
- followed by `chesslib::move_maker::make()`

This happens in:

- `source/motif/import/import_pipeline.cpp`
- `source/motif/import/import_worker.cpp`

### Stripped Import Profile

On the stripped motif-chess import path, the top remaining compute symbols are dominated by SAN/legality work:

- `chesslib::move_generator::moves` about `18.6%`
- `find_san_move` about `4.5%`
- `board::attacked_by` variants about `10%+` combined across hot instantiations
- `move_maker::make()` about `2.0%`

Storage and pipeline overhead are no longer the dominant buckets on that stripped path.

## A/B Benchmark Result

Standalone benchmark harness:

- `bench/san_ab/main.cpp`

Reference baseline library location used locally:

- `~/src/chess-library`

Benchmark shape:

- parse PGN once using motif `pgn_reader`
- build an in-memory SAN corpus
- run only SAN decode + make-move loops
- exclude SQLite, DuckDB, taskflow, logging, and import pipeline orchestration
- verify final board states between libraries, ignoring final en-passant target formatting differences

Corpus used:

- `/home/bogdb/scid/twic/10k_games.pgn`
- `9999` games
- `811,813` SAN moves
- one game skipped because it contained unsupported SAN token `--`

Observed results over three full runs:

- `chesslib`: about `148 ns/move`, about `6.7M moves/s`
- `chess-library`: about `74-75 ns/move`, about `13.3-13.5M moves/s`
- relative speedup: about `1.98x-2.00x`

Interpretation:

- `chesslib` appears to retain about `2x` headroom on the SAN decode + make path
- this does not imply a `2x` full import speedup
- but it strongly justifies spending more optimization effort upstream before making bigger architectural changes in motif-chess

## Current chesslib Implementation Shape

Relevant files in pinned revision:

- `source/util/san.cpp`
- `include/chesslib/board/move_generator.hpp`
- `source/board.cpp`

Important current observation:

- `san.cpp` already contains a targeted SAN resolution path (`find_san_move_targeted`)
- therefore the easy "stop generating all pseudo-legal moves" win has already largely been taken
- the remaining cost is primarily legality validation of candidates and attack-check logic

## Optimization Priorities

### Priority 1: Cheaper legality test for targeted SAN candidates

#### Why

The targeted SAN path already narrows candidate source squares to a small set. The remaining waste is that each candidate still goes through the generic legality machinery:

- construct candidate move
- run `move_maker`
- call legality check that relies on general move semantics

For SAN parsing, candidate legality is narrower than general move execution. The hot question is usually:

- does this candidate leave our king in check?

That suggests a specialized legality fast path for SAN candidate verification.

#### Proposed direction

Add a SAN-specific legality check helper that:

- assumes source/target/promotion structure is already narrowed
- applies only the minimum board mutation needed
- checks king safety directly
- avoids generic path branches not needed for SAN candidate filtering

#### Expected payoff

High.

This attacks the highest-value remaining work after candidate narrowing.

#### Risk

Moderate.

Legality logic is correctness-sensitive, especially for:

- pins
- en passant discovered attacks
- castling through attacked squares
- promotions that change attack lines

#### Recommendation

This should be the first upstream experiment.

## Priority 2: Faster `board::attacked_by` / `is_attacked`

### Why

The stripped motif profile shows multiple `attacked_by` instantiations in the hot path. This strongly suggests king-safety checks are still expensive.

### Proposed direction

Review and optimize attack detection for the SAN legality path specifically:

- reduce repeated board lookups
- reduce branching in sliding-piece attack checks
- avoid redundant recomputation when the king square is known in advance
- ensure attack detection uses the cheapest available occupancy path for the current board representation

### Expected payoff

High.

Even modest gains here should compound across SAN legality checks, castling checks, and move generation.

### Risk

Moderate.

Attack generation is core engine logic and widely shared.

### Recommendation

Do immediately after Priority 1 if Priority 1 alone does not close most of the A/B gap.

## Priority 3: Remove generic castling SAN parsing path

### Why

In the current code, castling SAN still goes through `find_san_move(...)`, which falls back to move generation.

Castling can be resolved directly from:

- side to move
- castling rights
- board occupancy
- attacked path squares

without generic candidate enumeration.

### Proposed direction

Handle:

- `O-O`
- `O-O-O`

with a dedicated path in `san::from_string()` that directly constructs and validates the castling move.

### Expected payoff

Low to medium.

Castling frequency is low, so it will not explain the full A/B gap, but it is a clean correctness-preserving simplification.

### Risk

Low.

### Recommendation

Good cleanup win, but not the first thing to do.

## Priority 4: Reduce transient allocations and move-list materialization in SAN helpers

### Why

Current SAN helper code still builds temporary move lists in some fallback/helper paths.

Examples worth reviewing:

- `has_any_legal_move()`
- `find_san_move()` fallback path
- `disambiguation_flags()`

### Proposed direction

- avoid allocating or populating full move lists where a small fixed buffer or direct scan suffices
- prefer direct candidate enumeration when the candidate count is bounded

### Expected payoff

Medium.

This likely will not dominate by itself, but it may smooth remaining overhead once legality checks are cheaper.

### Risk

Low.

### Recommendation

Useful second-wave cleanup after the first legality work lands.

## Priority 5: Review `move_maker::make()` cost on the import-only path

### Why

The A/B benchmark includes both decode and move application. Even after decode, every successful SAN move still pays full board mutation cost.

### Proposed direction

Audit `move_maker::make()` for import-oriented use:

- unnecessary work done for features import does not need
- avoidable bookkeeping during common quiet moves
- branch reduction in the frequent non-castle, non-promotion path

### Expected payoff

Medium.

### Risk

Moderate.

This touches shared move execution semantics.

### Recommendation

Pursue only after candidate legality work has been benchmarked, so decode and apply improvements do not get conflated.

## Proposed Implementation Order

Recommended upstream sequence:

1. Add or adapt a dedicated SAN microbenchmark inside `chesslib`.
2. Implement SAN-specific legality fast path for narrowed candidates.
3. Re-run benchmark.
4. Optimize `attacked_by` / `is_attacked` in the SAN legality path.
5. Re-run benchmark.
6. Add direct castling SAN handling.
7. Re-run benchmark.
8. Review transient move-list creation and helper allocations.
9. Re-run benchmark.
10. Only then consider deeper board-representation or move-application changes.

## Benchmark Plan

### Primary Acceptance Benchmark

Use the motif harness:

- `bench/san_ab/main.cpp`

against:

- current pinned `chesslib`
- optimized `chesslib` branch
- local `~/src/chess-library` as reference ceiling

### Success Metrics

Primary metric:

- `ns/move` on the 10k SAN corpus

Secondary metrics:

- zero failures on the filtered corpus
- final board-state equivalence on the correctness pass
- no regressions on upstream `chesslib` SAN tests

### Target

Reasonable near-term goal:

- close at least half of the measured gap to `chess-library`

Numerically, starting from about `148 ns/move`, this means:

- first target: about `110-120 ns/move`
- stretch target: below `100 ns/move`

Matching `chess-library` exactly is not necessary to justify the work. A solid first win in the `20-35%` range would likely produce a meaningful full-import improvement.

## What Not To Do Yet

Avoid these before the above work is measured:

- changing motif-chess architecture again
- replacing `chesslib` outright
- adding parser-side optimizations to the same experiment
- mixing storage-path changes with SAN-path experiments

The point of this phase is to isolate upstream `chesslib` headroom.

## Recommendation

The data supports a dedicated upstream `chesslib` performance story.

Recommended next story scope:

1. Add a native SAN benchmark to `chesslib` based on the motif corpus shape.
2. Implement a SAN-specific candidate legality fast path.
3. Optimize attack detection on that path.
4. Re-run the motif A/B harness after each step.

## Final Judgment

The benchmark against `~/src/chess-library` provides strong empirical evidence that `chesslib` still has meaningful optimization headroom.

The most likely profitable work is no longer broad candidate generation, but specialized legality and attack-check acceleration inside SAN resolution.

That is the right next place to invest before resuming larger motif-chess architectural changes for import throughput.
