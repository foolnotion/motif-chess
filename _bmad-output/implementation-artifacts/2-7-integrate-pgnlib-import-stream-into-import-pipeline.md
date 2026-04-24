# Story 2.7: Integrate pgnlib import_stream into the Import Pipeline

Status: ready-for-dev

## Story

As a developer,
I want the import pipeline to consume `pgnlib::import_stream` instead of fully
materialized PGN game objects,
so that PGN import avoids unnecessary parser allocations and object
materialization.

## Acceptance Criteria

1. Given the approved upstream `pgnlib` `import_stream` API is available,
when motif-chess imports a PGN file, then the import path consumes tags and SAN
moves from `pgnlib::import_stream` rather than fully materialized PGN game
objects.
2. Import correctness, malformed-game handling, resume behavior, and logging
semantics remain unchanged.
3. The import pipeline does not retain parser-backed `string_view` data beyond
safe lifetime boundaries.
4. The story documents and preserves the current handling of legal PGN features
required by `NFR16` for the import path, including how recursive variations,
NAGs, comments, and `%clk` annotations are treated after the move to
`pgnlib::import_stream`.
5. Given the current 10k PGN perf fixture and existing perf commands,
when the SAN-only and full import performance paths are rerun after adopting the
approved upstream `pgnlib` revision, then the story records before/after timings
for:
   - `build/release/test/motif_import_test "[performance][motif-import][san-prepare]"`
   - `build/release/test/motif_import_test "[performance][motif-import]"`
   using `MOTIF_IMPORT_PERF_PGN="/home/bogdb/scid/twic/10k_games.pgn"`
   and the full import path shows measurable improvement relative to the
   pre-adoption baseline.

## Tasks / Subtasks

- [ ] Task 1: Update the project to consume the approved upstream `pgnlib`
  revision (AC: 1, 3)
  - [ ] Record the merged upstream `pgnlib` commit/branch/tag adopted by this
        story in the Dev Agent Record before implementation is marked done.
  - [ ] Update the project's `pgnlib` input using the existing dependency
        workflow only; do not introduce alternate acquisition paths.
  - [ ] Rebuild the project in `dev`, `release`, and `dev-sanitize` as needed
        to confirm compatibility.
- [ ] Task 2: Replace hot-path PGN materialization with `pgnlib::import_stream`
  in motif-chess import code (AC: 1, 2, 3, 4)
  - [ ] Identify the current adapter/reader boundaries that still depend on
        fully materialized `pgn::game` objects.
  - [ ] Switch the import path to consume import-oriented tags and SAN moves from
        `pgnlib::import_stream`.
  - [ ] Keep parser-backed `string_view` values within safe lifetime scope;
        convert or copy only where persistence or delayed use requires it.
  - [ ] Do not fold SAN algorithm changes into this story; continue relying on
        `chesslib::san::from_string()`.
- [ ] Task 3: Verify behavioral compatibility of import semantics (AC: 2, 3, 4)
  - [ ] Run existing import tests without weakening assertions.
  - [ ] Confirm malformed-game handling, resume behavior, structured summary,
        and enriched logging remain unchanged.
  - [ ] Explicitly document how recursive variations, NAGs, comments, and `%clk`
        annotations are preserved, ignored safely, or otherwise handled without
        regressing the current Epic 2 import contract.
- [ ] Task 4: Re-run targeted performance checks and document deltas (AC: 5)
  - [ ] Run the SAN-only perf path on the current 10k PGN sample.
  - [ ] Run the full import perf path on the same sample with current best
        settings.
  - [ ] Record before/after timing deltas in the Dev Agent Record.

## Dev Notes

- This story is the second item in the approved Epic 2 performance track.
- Upstream `pgnlib` work reported approximately 5-6x parser-side speedups in
  import-oriented scenarios, but motif-chess must validate the real end-to-end
  effect after integration.
- The merged upstream revision must be pinned as part of implementation. This
  story is not complete if motif-chess only builds against a floating branch
  without recording the adopted revision.
- Keep this story narrowly scoped to parser/materialization improvement. Do not
  fold in new downstream storage tuning here; that belongs to Story 2.8.
- `pgnlib::import_stream` is the preferred import-path integration once
  available, per the approved architecture update.

### Technical Requirements

- Preserve Epic 2 import guarantees from the PRD:
  - FR09-FR15 and FR20 remain intact.
  - NFR03, NFR04, NFR07, NFR08, NFR10, NFR16, and NFR20 remain intact.
- Maintain current import correctness and observability semantics:
  - malformed inputs never crash import
  - resume behavior remains correct
  - structured summary and enriched logging remain unchanged
- Treat this as upstream parser adoption, not downstream PGN parser redesign.

### Architecture Compliance

- `pgnlib` owns text parsing and import-oriented PGN streaming.
  [Source: `_bmad-output/planning-artifacts/architecture.md#Cross-Cutting-Concerns`]
- `chesslib` continues to own SAN resolution and Zobrist identity; this story
  must not move chess-rule logic into `pgnlib` or motif-chess.
  [Source: `_bmad-output/planning-artifacts/architecture.md#Cross-Cutting-Concerns`]
- The hot import path should avoid full PGN materialization and consume
  import-oriented views where safe.
  [Source: `_bmad-output/planning-artifacts/architecture.md#Cross-Cutting-Concerns`]
- The import path remains a staged pipeline: parse -> resolve -> persist. This
  story affects the parse/materialization stage only.
  [Source: `_bmad-output/planning-artifacts/research/technical-motif-chess-pgn-import-performance-optimization-research-2026-04-20.md`]

### Library / Framework Requirements

- Use the existing Nix/flake-based upstream dependency workflow for `pgnlib`.
  Do not use `FetchContent`, submodules, or ad hoc vendoring.
  [Source: `_bmad-output/project-context.md#Technology Stack & Versions`]
- `pgnlib` exposes text parsing only; motif-chess remains responsible for SAN
  validation through `chesslib`.
  [Source: `_bmad-output/project-context.md#Technology Stack & Versions`]
- `NFR16` still applies after this integration; the import path must not narrow
  its effective PGN feature support without an explicit approved scope change.
  [Source: `_bmad-output/planning-artifacts/prd.md#Integration`]
- Use `fmt::format`, `tl::expected`, Catch2 v3, Clang 21, and the existing
  project conventions exactly as already established.
  [Source: `_bmad-output/project-context.md`]
- Preserve current profiling workflow:
  - optimized debuggable build for performance
  - `perf stat` and `perf record --call-graph dwarf` for verification
  [Source: `_bmad-output/planning-artifacts/research/technical-motif-chess-pgn-import-performance-optimization-research-2026-04-20.md`]

### File Structure Requirements

- Likely touch points in motif-chess:
  - dependency input / package wiring for `pgnlib`
  - `motif_import` reader/adapter/import pipeline boundaries
  - import performance test/benchmark paths if baselines or notes are updated
- Avoid new parser abstractions unless the existing adapter layer truly cannot
  accommodate `import_stream` cleanly.
- Likely no changes should be needed in:
  - `chesslib` integration semantics
  - DuckDB schema/index logic
  - UI code

### Testing Requirements

- Run at minimum:
  - `cmake --build build/dev -j 16`
  - `build/dev/test/motif_import_test "[motif-import]"`
  - `cmake --build build/dev-sanitize -j 16`
  - `build/dev-sanitize/test/motif_import_test "[motif-import]"`
- Re-run focused performance checks:
  - `build/release/test/motif_import_test "[performance][motif-import][san-prepare]"`
  - `build/release/test/motif_import_test "[performance][motif-import]"`
  using the existing environment variables and fixture paths.
- Use the existing 10k fixture path for comparison runs:
  - `MOTIF_IMPORT_PERF_PGN="/home/bogdb/scid/twic/10k_games.pgn"`
- Add or update tests for any import adapter API changes required by
  `pgnlib::import_stream` adoption.

### Previous Story Intelligence

- Story 2.6 already adopted the upstream `chesslib` SAN optimization and kept
  import behavior stable; this story should build on that new baseline rather
  than re-litigating SAN ownership.
- Story 2.5 tightened import summary/logging correctness; those semantics must
  not regress.
- The current import pipeline profiling infrastructure and 10k PGN fixture path
  are already established and should be reused rather than reinvented.
- Deferred DuckDB index tuning exists but belongs to Story 2.8, not here.

### Project Structure Notes

- Core import work remains in `motif_import` and Qt-free modules only.
- Keep changes minimal and local to the upstream parser-adoption path.
- Parser-backed `string_view` values should be consumed synchronously and not
  stored beyond the backing buffer lifetime; copy only when delayed ownership is
  required.

### References

- [Source: `_bmad-output/planning-artifacts/epics.md#Story-27-Integrate-pgnlib-import_stream-into-the-Import-Pipeline`]
- [Source: `_bmad-output/planning-artifacts/prd.md#PGN-Import`]
- [Source: `_bmad-output/planning-artifacts/prd.md#Performance`]
- [Source: `_bmad-output/planning-artifacts/prd.md#Reliability`]
- [Source: `_bmad-output/planning-artifacts/prd.md#Integration`]
- [Source: `_bmad-output/planning-artifacts/architecture.md#Technical-Constraints--Dependencies`]
- [Source: `_bmad-output/planning-artifacts/architecture.md#Cross-Cutting-Concerns`]
- [Source: `_bmad-output/project-context.md`]
- [Source: `_bmad-output/implementation-artifacts/2-5-import-completion-summary-error-logging.md`]
- [Source: `_bmad-output/implementation-artifacts/2-6-adopt-upstream-chesslib-san-optimization.md`]
- [Source: `_bmad-output/planning-artifacts/research/technical-motif-chess-pgn-import-performance-optimization-research-2026-04-20.md`]

## Dev Agent Record

### Agent Model Used

opencode/gpt-5.4

### Debug Log References

- Existing import perf commands and results are documented in conversation and
  should be rerun after upstream adoption.

### Completion Notes List

- Story created after upstream `pgnlib` `import_stream` work was reported merged.
- This story intentionally excludes new downstream storage retuning, which is
  reserved for Story 2.8.
- Record the exact merged upstream `pgnlib` revision adopted.
- Record how `NFR16` PGN features are preserved or handled after integration.
- Record before/after timings for the SAN-only and full import perf paths.

### File List

- _bmad-output/implementation-artifacts/2-7-integrate-pgnlib-import-stream-into-import-pipeline.md
