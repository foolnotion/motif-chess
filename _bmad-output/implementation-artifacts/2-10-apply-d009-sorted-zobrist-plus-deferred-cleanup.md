# Story 2.10: Apply D009 — Drop ART Index, Sorted-by-Zobrist Default + Deferred Cleanup

Status: done

## Story

As a developer,
I want to apply [D009] by dropping the ART Zobrist index in favor of sorted-by-zobrist with zonemaps as the default position storage strategy,
so that position queries achieve 4x better P99 latency with 8x less memory, while also addressing deferred cleanup items from Epic 2.

## Acceptance Criteria

1. **Given** the ART Zobrist index currently exists in the codebase,
    when Story 2.10 is complete,
    then the ART index is removed entirely (no `create_zobrist_index()` or `drop_zobrist_index()` methods),
    and `sort_by_zobrist()` becomes the default and only position optimization strategy.

2. **Given** `rebuild_position_store()` is called with default parameters,
    when the rebuild completes,
    then positions are sorted by zobrist_hash (zonemap-friendly layout),
    and no ART index is created.

3. **Given** `import_config` struct exists with current defaults,
    when Story 2.10 is complete,
    then:
    - `num_workers` defaults to `std::max(1u, std::thread::hardware_concurrency())`
    - `k_default_lines` (via `num_lines`) defaults to `64` (or hardware concurrency)
    - `sort_positions_by_zobrist_after_rebuild` defaults to `true`
    - `create_position_index_after_rebuild` is removed (ART no longer exists)

4. **Given** `import_config` receives zero values for `num_workers` or `num_lines`,
    when validation runs,
    then the pipeline returns `error_code::invalid_state` with a clear error message
    (zero values cause undefined behavior in taskflow).

5. **Given** `write_positions=false` in the import config,
    when `prepare_game()` is called,
    then `position_rows` vector is not populated at all (CPU/memory savings),
    and the import pipeline skips position preparation entirely.

6. **Given** `find_tag()`, `parse_elo()`, `pgn_result_to_string()`, and `pgn_result_to_int8()` helpers exist,
    when Story 2.10 is complete,
    then these functions are extracted to a shared header/implementation
    and imported from both `import_worker.cpp` and `import_pipeline.cpp`,
    eliminating the current duplication.

7. **Given** all changes are implemented,
    when tests run,
    then all existing tests pass,
    and new tests verify:
    - sorted-by-zobrist is the default
    - ART index methods are gone
    - zero config values are rejected
    - helper extraction doesn't break functionality

## Tasks / Subtasks

- [x] Task 1: Remove ART Zobrist index (AC: #1, #2)
  - [x] Remove `create_zobrist_index()` from `position_store.hpp` and `.cpp`
  - [x] Remove `drop_zobrist_index()` from `position_store.hpp` and `.cpp`
  - [x] Update `rebuild_position_store()` signature: remove `create_index` parameter
  - [x] Update `rebuild_position_store()` implementation: always call `sort_by_zobrist()`, never create ART index
  - [x] Update all call sites to remove `create_index` argument

- [x] Task 2: Update import_config defaults (AC: #3)
  - [x] Change `k_default_lines` from `1` to `64` in `import_pipeline.hpp`
  - [x] Change `num_workers` default from `1` to `std::max(1u, std::thread::hardware_concurrency())`
  - [x] Change `sort_positions_by_zobrist_after_rebuild` default from `false` to `true`
  - [x] Remove `create_position_index_after_rebuild` field entirely
  - [x] Update all code that references `create_position_index_after_rebuild`

- [x] Task 3: Add input validation for zero config values (AC: #4)
  - [x] Add validation in `import_pipeline::run_from()` method
  - [x] Return `error_code::invalid_state` if `num_workers == 0` or `num_lines == 0`
  - [x] Add descriptive error logging via `motif.import` logger
  - [x] Add test cases for zero value rejection

- [x] Task 4: Skip position_rows when write_positions=false (AC: #5)
  - [x] Modify `prepare_game()` in `import_pipeline.cpp` to check config
  - [x] If `write_positions == false`, skip position row population entirely
  - [x] Ensure position_rows vector is not constructed/allocated
  - [x] Existing test covers correctness (all existing tests pass)

- [x] Task 5: Extract shared helpers (AC: #6)
  - [x] Create `source/motif/import/pgn_helpers.hpp` with extracted function declarations
  - [x] Create `source/motif/import/pgn_helpers.cpp` with implementations
  - [x] Functions to extract: `find_tag()`, `parse_elo()`, `pgn_result_to_string()`, `pgn_result_to_int8()`
  - [x] Remove duplicate implementations from `import_worker.cpp` anonymous namespace
  - [x] Remove duplicate implementations from `import_pipeline.cpp` anonymous namespace
  - [x] Add `#include "motif/import/pgn_helpers.hpp"` to both files
  - [x] Verify builds, tests pass, no functionality change

- [x] Task 6: Update tests and verify (AC: #7)
  - [x] Update `checkpoint_test.cpp` — no ART index references found
  - [x] Update `import_pipeline_test.cpp` — adjust for new defaults
  - [ ] Update `import_pipeline_test.cpp` — adjust for new defaults
  - [x] Add test: sorted-by-zobrist is default after rebuild
  - [x] Add test: ART index methods are gone (compilation verifies)
  - [x] Add test: zero config values are rejected
- [x] Run full test suite: `cmake --preset=dev && ctest --test-dir build/dev` — 105 pass, 0 fail
- [x] Run sanitizer tests: `cmake --preset=dev-sanitize && ctest --test-dir build/dev-sanitize` — 105 pass, 0 fail
- [ ] Run benchmark to confirm performance characteristics unchanged (requires PGN corpus)

### Review Findings

- [x] [Review][Patch] Partitioned rebuild no longer creates Zobrist indexes [source/motif/db/database_manager.cpp]
- [x] [Review][Patch] `rebuild_position_store()` now keeps the sort step inside the rebuild transaction [source/motif/db/database_manager.cpp]
- [x] [Review][Patch] Default sorted-rebuild test now verifies on-disk zobrist ordering [test/source/motif_db/database_manager_test.cpp]
- [x] [Review][Patch] Helper extraction now has focused regression test coverage [test/source/motif_import/import_pipeline_test.cpp]
- [x] [Review][Defer] `rebuild_position_store()` can return after starting a DuckDB transaction without rolling it back when SQLite statement preparation fails [source/motif/db/database_manager.cpp:522] — deferred, pre-existing

---

## Dev Notes

### [D009] Context — Why Drop ART Index

From Story 2.8 benchmarking (3.4M games, 288M position rows, 200 sampled hashes):

| Variant | P50 | P99 | Total | RSS |
|---|---|---|---|---|
| ART indexed | 630 us | 224,278 us | 2,428 ms | ~24 GB |
| No index | 50,413 us | 129,151 us | 10,770 ms | ~3 GB |
| **Sorted no index** | **1,254 us** | **58,719 us** | **608 ms** | **~3 GB** |

Sorted zonemaps give near-index P50, 4x better P99, and 8x less memory. ART index construction is also expensive during rebuild.

### Current Code State

**`position_store.hpp` (current):**
```cpp
class position_store {
public:
    auto create_zobrist_index() -> result<void>;  // REMOVE
    auto drop_zobrist_index() -> result<void>;    // REMOVE
    auto sort_by_zobrist() -> result<void>;       // KEEP as default
    // ...
};
```

**`database_manager.hpp` (current):**
```cpp
auto rebuild_position_store(bool create_index = true,   // REMOVE parameter
                            bool sort_by_zobrist = false)  // CHANGE default to true
    -> result<void>;
```

**`import_pipeline.hpp` (current):**
```cpp
struct import_config {
    static constexpr std::size_t k_default_lines = 1;  // CHANGE to 64
    std::size_t num_workers {1};  // CHANGE to hardware_concurrency
    bool create_position_index_after_rebuild {true};  // REMOVE
    bool sort_positions_by_zobrist_after_rebuild {false};  // CHANGE to true
};
```

### Architectural Compliance

- `motif_db` owns storage; `motif_import` owns pipeline
- No breaking changes to `motif_db` public API except removing ART methods
- Sorting is a storage-layer concern, stays in `position_store`
- Config defaults are pipeline-layer, stay in `import_pipeline`

### Helper Extraction Pattern

Create shared helpers in new files:
- `source/motif/import/pgn_helpers.hpp` — declarations with `#pragma once`
- `source/motif/import/pgn_helpers.cpp` — implementations

Functions to extract (from both `import_worker.cpp` and `import_pipeline.cpp`):
- `auto find_tag(std::vector<pgn::tag> const& tags, std::string_view key) -> std::string`
- `auto parse_elo(std::string const& raw) -> std::optional<std::int16_t>`
- `auto pgn_result_to_string(pgn::result res) noexcept -> std::string`
- `auto pgn_result_to_int8(pgn::result res) noexcept -> std::int8_t`

Both TUs currently have identical anonymous namespace implementations.

### Zero Value Validation

Taskflow pipeline with `num_workers == 0` or `num_lines == 0` causes undefined behavior (likely crash or hang). Validation must happen before pipeline construction:

```cpp
if (config.num_workers == 0 || config.num_lines == 0) {
    return tl::unexpected(error_code::invalid_state);
}
```

### Performance Impact

- Removing ART index: ~24 GB → ~3 GB RSS for 3.4M games
- Sorted zonemaps: 4x better P99 query latency
- Skipping position_rows when `write_positions=false`: CPU/memory savings on fast import path

### Breaking Changes

1. `position_store::create_zobrist_index()` — REMOVED
2. `position_store::drop_zobrist_index()` — REMOVED
3. `rebuild_position_store(bool create_index, bool sort_by_zobrist)` — signature changes to `rebuild_position_store(bool sort_by_zobrist = true)`
4. `import_config::create_position_index_after_rebuild` — REMOVED
5. `import_config` default values — CHANGED (restored to sensible defaults)

All breaking changes are within the codebase (no external API). Test fixes required.

---

## Project Structure Notes

**Files to create:**
- `source/motif/import/pgn_helpers.hpp`
- `source/motif/import/pgn_helpers.cpp`

**Files to modify:**
- `source/motif/db/position_store.hpp` — remove ART methods
- `source/motif/db/position_store.cpp` — remove ART implementations
- `source/motif/db/database_manager.hpp` — update rebuild signature
- `source/motif/db/database_manager.cpp` — update rebuild implementation
- `source/motif/import/import_pipeline.hpp` — update defaults, remove field
- `source/motif/import/import_pipeline.cpp` — add validation, skip position_rows
- `source/motif/import/CMakeLists.txt` — add pgn_helpers.cpp
- `source/motif/import/import_worker.cpp` — use shared helpers
- `test/source/motif_import/checkpoint_test.cpp` — verify no ART references
- `test/source/motif_import/import_pipeline_test.cpp` — update for new defaults

**Files NOT to modify:**
- `source/motif/db/game_store.*` — unrelated to position storage
- `source/motif/search/*` — Epic 3 territory
- UI code — no changes needed

---

## Testing Requirements

**New test cases to add:**
1. `position_store` no longer has ART index methods (compilation test)
2. `rebuild_position_store()` defaults to sorted-by-zobrist
3. `import_config` zero values are rejected with `invalid_state`
4. `write_positions=false` skips position row construction (performance observable)
5. Shared helpers produce identical results to previous duplicates

**Regression tests:**
- All existing import pipeline tests must pass
- All existing position_store tests must pass
- Benchmark: 10k games import performance unchanged (or improved)

**Verification commands:**
```bash
cmake --preset=dev && cmake --build build/dev
ctest --test-dir build/dev --output-on-failure

cmake --preset=dev-sanitize && cmake --build build/dev-sanitize
ctest --test-dir build/dev-sanitize --output-on-failure
```

---

## References

- [D009] Source: `_bmad-output/implementation-artifacts/epic-2-retro-2026-04-22.md#Key-Technical-Discoveries`
- Story 2.8 benchmark results: `_bmad-output/implementation-artifacts/2-8-reprofile-and-tune-remaining-import-storage-path.md`
- ART index removal rationale: Epic 2 retrospective, section "Key Technical Discoveries"
- Current `position_store` API: `source/motif/db/position_store.hpp`
- Current `import_config` defaults: `source/motif/import/import_pipeline.hpp:18-30`
- Duplicate helpers: `source/motif/import/import_worker.cpp` and `source/motif/import/import_pipeline.cpp` anonymous namespaces
- Deferred items list: `_bmad-output/implementation-artifacts/epic-2-retro-2026-04-22.md#Deferred-Technical-Items`

---

## Dev Agent Record

### Agent Model Used

Claude (glm-5.1)

### Debug Log References

N/A

### Completion Notes List

- ✅ Removed `create_zobrist_index()` and `drop_zobrist_index()` from `position_store` — ART index fully eliminated
- ✅ Updated `rebuild_position_store()` signature: `bool create_index, bool sort_by_zobrist` → `bool sort_by_zobrist = true`
- ✅ Updated `rebuild_partitioned_position_store()` to remove `drop_zobrist_index()` call
- ✅ `import_config` defaults updated: `k_default_lines=64`, `num_workers=hardware_concurrency`, `sort_positions_by_zobrist_after_rebuild=true`, removed `create_position_index_after_rebuild`
- ✅ Zero config validation added in `run_from()` with `error_code::invalid_state` and logging
- ✅ `prepare_game()` skips position_rows when `write_positions=false` — vector not populated
- ✅ Extracted `find_tag()`, `parse_elo()`, `pgn_result_to_string()`, `pgn_result_to_int8()` to `pgn_helpers.hpp/.cpp`
- ✅ Removed duplicates from `import_worker.cpp` and `import_pipeline.cpp`
- ✅ All 105 tests pass (dev + sanitize), no regressions
- ✅ New tests added for zero config validation and sorted-by-zobrist default

### File List

**Created:**
- `source/motif/import/pgn_helpers.hpp`
- `source/motif/import/pgn_helpers.cpp`

**Modified:**
- `source/motif/db/position_store.hpp`
- `source/motif/db/position_store.cpp`
- `source/motif/db/database_manager.hpp`
- `source/motif/db/database_manager.cpp`
- `source/motif/import/import_pipeline.hpp`
- `source/motif/import/import_pipeline.cpp`
- `source/motif/import/import_worker.cpp`
- `source/motif/import/CMakeLists.txt`
- `test/source/motif_import/import_pipeline_test.cpp`
- `test/source/motif_db/database_manager_test.cpp`

---

*Story completed. All acceptance criteria satisfied. Benchmark test deferred (requires PGN corpus).*

### Change Log

- 2026-04-22: Story 2.10 implementation complete — D009 applied, ART index removed, sorted-by-zobrist default, zero config validation, helper extraction, position skip optimization
