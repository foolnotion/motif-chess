# Story 2.9: Import Pipeline Performance Optimization

Status: backlog

## Story

As a developer,
I want to optimize the import pipeline's performance to close the NFR03 gap,
by addressing memory locality, pipeline batching friction, and allocation overhead
identified through Story 2.8 profiling and cache miss analysis.

## Context

Story 2.8 profiling revealed that the NFR03 gap (10M games in <20 minutes) is distributed across:
- **Futex contention** (~8% of wall time): Per-game token handoff in taskflow pipeline
- **Heap allocation pressure** (~11% of CPU cycles): Small allocations in hot paths
- **SQLite per-game transactions**: I/O overhead even with WAL buffering
- **DuckDB background threads** (~6% of CPU cycles): TaskScheduler overhead

Cache miss analysis (47% miss rate) suggests the `prepared_game` struct layout
is causing poor memory locality. The struct contains interleaved AoS (array-of-structures)
for game metadata, move lists, and position rows, all sharing the same cache line.

The architecture boundary between `motif_import` and `motif_db` requires that:
- Import pipeline calls `position_store::insert_batch()` - cannot directly access DuckDB
- All storage operations go through motif_db's public API

## Acceptance Criteria

1. Given the import pipeline memory hotspots are identified,
    when optimization changes are implemented,
    then cache miss rate reduces by at least 15% (from 47% to ≤40%)
    AND this reduction is verified through perf stat on 10k games fixture.

2. Given the taskflow pipeline batching friction is identified,
    when optimization changes are implemented,
    then futex contention time reduces by at least 50% (from 8% to ≤4% of wall time)
    AND this reduction is verified through perf stat on 10k games fixture.

3. Given the allocation pressure in hot paths is identified,
    when optimization changes are implemented,
    then malloc/free rate in prepare_game and position generation reduces by at least 60%
    AND this reduction is verified through perf stat on 10k games fixture.

4. Given all optimizations are implemented,
    when benchmarked on 10k games fixture with deferred position index build,
    then wall time improves by at least 20% compared to post-2.8 baseline (13.5s)
    OR extrapolated time for 10M games reaches ≤25 minutes (down from 3.7 hours).

5. Given any downstream tuning is applied,
    then all Epic 2 correctness guarantees remain intact:
    - Malformed inputs never crash (NFR10)
    - Resume behavior is correct (NFR08)
    - Structured summary and enriched logging are unchanged (FR14, AR06)
    - All tests pass under `dev-sanitize` with zero ASan/UBSan violations (NFR11).

6. Given the architectural boundary between import pipeline and storage is preserved,
    when optimizations are implemented,
    then import pipeline continues to use motif_db::position_store public API only,
    AND no direct DuckDB C API calls bypass motif_db abstraction.

## Tasks / Subtasks

- [ ] Task 1: Profile current memory layout and cache behavior
  - [ ] Run perf stat with `--cache-misses` and `--cache-references` on 10k games import
  - [ ] Record baseline cache metrics: L1/L2/L3 miss rates, LLC miss rates
  - [ ] Run perf record with `-g -e cpu-cycles,u -p l1d,load,l1d,store,l1d,load,l1d,store-prefetch:u`
  - [ ] Identify cache lines being thrashed by `prepared_game` struct access patterns
  - [ ] Document findings in Dev Agent Record

- [ ] Task 2: Optimize prepared_game memory layout (AC: 1)
  - [ ] Refactor `prepared_game` struct to use SoS (Structure of Arrays)
    - SoS all hot fields: `game_row`, `moves` (encoded), `position_rows`
  - [ ] Ensure 64-byte alignment for all fields
  - [ ] Pad to 128-byte cache line boundaries where beneficial
  - [ ] Verify cache miss reduction in micro-benchmarks or real perf runs
  - [ ] Run clang-tidy and cppcheck - zero warnings required

- [ ] Task 3: Implement pipeline batching (AC: 2)
  - [ ] Add `run_state` extension with `batch_config` field:
    ```cpp
    struct batch_config {
        std::size_t target_batch_size {};  // e.g., 500 games per batch
        std::chrono::milliseconds target_window {};  // e.g., 100ms
    };
    ```
  - [ ] Refactor `run_from()` to group 500 games before writing to DuckDB
  - [ ] Pass aggregated game and position data through pipeline stages in bulk
  - [ ] Replace per-game taskflow tokens with batch handoff tokens
  - [ ] Maintain checkpoint granularity at batch boundaries
  - [ ] Verify futex contention reduction through perf stat
  - [ ] Add unit tests for batch processing correctness
  - [ ] Update import_pipeline_test.cpp with batch test cases

- [ ] Task 4: Implement arena allocator for prepared_game slots (AC: 3)
  - [ ] Design simple arena allocator interface:
    ```cpp
    class prepared_game_arena {
        prepared_game* allocate();
        void deallocate(prepared_game*);
    };
    ```
  - [ ] Pre-allocate pool of prepared_game structs (e.g., 128 slots × 4KB each)
  - [ ] Modify `pipeline_slot` to use arena instead of inline `prepared_game`
  - [ ] Reset arena between batches to reuse memory
  - [ ] Add micro-benchmarks to measure allocation overhead reduction
  - [ ] Verify malloc/free rate reduction through perf stat
  - [ ] Add arena unit tests

- [ ] Task 5: Validate NFR03 improvement (AC: 4)
  - [ ] Run full import benchmark on 10k games with all optimizations
  - [ ] Compare to post-2.8 baseline (13.5s deferred index)
  - [ ] Calculate extrapolated 10M game time
  - [ ] If ≥20% improvement achieved, mark AC4 as satisfied
  - [ ] If AC4 not met but still ≥10% improvement, document rationale
  - [ ] Run full test suite in `dev-sanitize` - zero violations required
  - [ ] Document final performance metrics in Dev Agent Record

## Dev Notes

- **Architectural Boundary:** Import pipeline (motif_import) MUST NOT access DuckDB directly. All storage operations go through motif_db::position_store public API. This is a hard constraint from Epic 2 design.

- **Performance Targets:** Based on Story 2.8 profiling, the baseline is ~13.5s for 10k games. The NFR03 target is 10M games in <20 minutes. Extrapolating: 13.5s × 1000 ≈ 13,500s (3.7 hours) - we need a **~3.5× improvement** to meet the target.

- **Priority Order:** 
  1. **Memory layout (Task 2)** - Highest impact (47% cache miss rate)
  2. **Pipeline batching (Task 3)** - High impact (8% futex)
  3. **Arena allocator (Task 4)** - Medium impact (11% malloc)
  
  Rationale: Memory layout has highest leverage (47% → potentially 20-30% improvement) and lowest risk. Pipeline batching has good leverage (8% → potentially 50% reduction) but higher complexity. Arena allocator has moderate leverage (11% → 15-25% improvement) but adds significant complexity.

- **SoS Approach for Task 2:**
  - Use std::array with fixed-size elements for each field
  - Example:
    ```cpp
    struct prepared_game {
        std::array<std::uint64_t, 64> zobrist_hashes;  // 64 moves max
        std::array<motif::db::position_row, 64> position_rows; // 64 moves max
        // ... other fields
    };
    ```
  - Benefits: Contiguous memory layout, compiler knows size at compile time, 64-byte alignment guaranteed
  - Trade-off: Fixed-size arrays waste memory for games with fewer moves (still acceptable)

- **Cache Line Analysis from Story 2.8:**
  - 47% miss rate is extremely high for sequential access patterns
  - Likely cause: `prepared_game` struct (288 bytes in vtable + ~200 bytes per game) creates false sharing
  - SoS reduces to ~128-192 bytes per game, improving spatial locality
  - Expected gain: Each cache line can now fit ~2-3× more games

- **Task 3 Implementation Notes:**
  - Current taskflow uses producer/consumer pattern with per-game synchronization
  - Need new stage: `batch_aggregator` that collects 500 games then hands off to storage stages
  - Challenge: Maintaining checkpoint granularity (every 500 games) while reducing futex contention
  - Alternative: Lock-free ring buffer between stage2 and storage (complex but maximum reduction)

- **Why Not in Story 2.8:** Task 3 requires taskflow redesign which is risky for a "reprofile and tune" story. Better to create focused performance story.

### Prior Art: How Similar Projects Handle This

**Cache-Oblivious Data Structures:**
- ClickHouse: Uses column-oriented storage (CoW) with fixed column types
- ScyllaDB: Uses SoS (Structure of Arrays) for in-memory data structures
- Key insight: Sequential field access in cache-friendly structures outperforms pointer-chasing in AoS

**Batching Strategies:**
- Apache Arrow: Processes data in row groups (batch_size default 64) for vectorized operations
- Parquet: Similar columnar approach with row group sizes tunable
- Key pattern: Larger batches improve both CPU cache utilization and I/O efficiency

**Object Pooling:**
- jemalloc: General-purpose allocator optimized for multithreaded workloads
- mimalloc: Small object allocator with size classes
- Redis: Uses simple zmalloc-based arena allocator for objects with known lifetimes
- Application: Arena allocators commonly used for per-request objects in server frameworks

### Technical Requirements

- SoS for prepared_game: Use `std::array` with compile-time capacities
- Arena allocator: Simple bump-pointer allocator with reset capability
- Pipeline batching: Keep taskflow for parallel stages, add batch aggregation stage
- Maintain architectural boundary: Import pipeline → motif_db public API → DuckDB (via motif_db)
- No breaking changes to `motif_import` or `motif_db` public APIs
- All public API changes require test coverage
- Use Catch2 v3 for all benchmarks (use `REQUIRE_THAT` for performance checks)

### File Structure Requirements

Files likely to change:
- `source/motif/import/prepared_game.hpp` - New header with SoS prepared_game
- `source/motif/import/arena_allocator.hpp` - New arena allocator
- `source/motif/import/import_pipeline.cpp` - Memory layout changes
- `source/motif/import/import_pipeline.hpp` - Add batch_config to import_config
- `source/motif/db/database_manager.cpp` - May need batch query API
- `test/source/motif_import/prepared_game_test.cpp` - SoS layout micro-benchmarks
- `test/source/motif_import/arena_allocator_test.cpp` - Arena allocator tests
- `test/source/motif_import/import_pipeline_test.cpp` - Add batch processing tests

### Testing Requirements

Minimum verification sequence:
```bash
cmake --build build/dev -j 16
build/dev/test/motif_import_test -j 8
build/dev/test/motif_import/arena_allocator_test -j 8
cmake --build build/release -j 16
MOTIF_IMPORT_PERF_PGN="/home/bogdb/scid/twic/10k_games.pgn" \
  build/release/test/motif_import_test "[performance][motif-import]"
perf stat -e cache-misses,cache-references -d ...
```

- Perf benchmarks must establish baseline and verify AC1, AC2, AC3
- Arena allocator must pass stress tests with multithreaded allocation patterns
- Batch processing tests must verify correctness (same result as per-game)
- All changes must pass `dev-sanitize` with zero violations

### References

- [Source: Story 2.8 Dev Agent Record]
- [Source: Cache miss analysis and recommendations]
- [Source: Task 2 deferred items from Story 2.6 code review]
- [Source: CONVENTIONS.md - architectural boundary rules]
- [Prior Art: ClickHouse/ScyllaDB CoW and Parquet documentation]
- [Prior Art: jemalloc/mimalloc/Redis arena allocator implementations]
