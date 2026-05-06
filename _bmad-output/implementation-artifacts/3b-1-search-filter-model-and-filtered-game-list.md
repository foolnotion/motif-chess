# Story 3b.1: Search Filter Model & Filtered Game List

Status: approved

## Story

As a user,
I want to filter the game list by player name, color, Elo range, result, ECO prefix, and/or position (Zobrist hash),
so that I can find relevant games for opponent preparation and opening study without browsing the entire database.

## Acceptance Criteria

1. **Given** the `motif_db` module, **When** the story is complete, **Then** `source/motif/db/types.hpp` defines `motif::db::search_filter` with optional fields: `player_name` (string), `player_color` (enum: `white | black | either`, default `either`), `min_elo` (int), `max_elo` (int), `result` (string), `eco_prefix` (string), `position` (zobrist_hash), `offset` (int, default 0), `limit` (int, default 100, max 500). **And** a `game_list_result` struct with `std::vector<game_list_entry> games` and `std::int64_t total_count`.

2. **Given** a `search_filter` with `player_name` set, **When** `game_store::find_games(search_filter)` is called, **Then** only games where the matching player name is a case-insensitive partial match against white or black player are returned. **When** `player_color` is `white`, only white-player matches; `black` restricts to black; `either` matches either side.

3. **Given** a `search_filter` with `min_elo` and/or `max_elo` set, **When** `find_games` is called, **Then** only games where both white and black Elo are within the range (inclusive) are returned. Games without Elo data are excluded when an Elo filter is active.

4. **Given** a `search_filter` with `result` set, **When** `find_games` is called, **Then** only games with the matching result are returned.

5. **Given** a `search_filter` with `eco_prefix` set to "B9", **When** `find_games` is called, **Then** games with ECO codes B90–B99 are returned; prefix "B" matches all B-code games.

6. **Given** a `search_filter` with `position` set to a Zobrist hash, **When** `find_games` is called, **Then** the query first retrieves matching game IDs from DuckDB `position` table, then applies remaining metadata filters in SQLite via a temp table join or IN clause (batched at 999). No cross-store join occurs on the hot path.

7. **Given** multiple fields set in `search_filter`, **When** `find_games` is called, **Then** all active filters are ANDed; a game must satisfy all conditions.

8. **Given** an empty `search_filter` (all fields nullopt/defaults), **When** `find_games` is called, **Then** all games are returned paginated; this is not treated as an error (NFR08).

9. **Given** any filter combination on a 4M-game corpus, **When** `find_games` is called, **Then** the query completes in under 100ms (NFR01). Player name search in SQLite completes in under 50ms before DuckDB intersection (NFR05).

10. **Given** any test scenario, **Then** filtered results exactly match manually counted results from the test dataset (NFR06). All tests use real in-memory SQLite and DuckDB — no mocks. All tests pass under `cmake --preset=dev-sanitize` with zero ASan/UBSan violations (NFR12). Zero new clang-tidy or cppcheck warnings (NFR11).

## Tasks / Subtasks

- [x] Task 1: Define `search_filter` and `game_list_result` types (AC: #1)
  - [x] Add `player_color` enum class to `types.hpp`
  - [x] Add `search_filter` struct with all fields from AC#1
  - [x] Add `game_list_result` struct with `games` vector and `total_count`
  - [x] Remove or deprecate `game_list_query` — replace all callers with `search_filter`
- [x] Task 2: Implement `game_store::find_games(search_filter)` (AC: #2–#8)
  - [x] Write the new parameterized SQL query with `? IS NULL OR ...` pattern for all metadata filters
  - [x] Handle `player_color` filter: when `white`, only match against `w.name`; when `black`, only match against `b.name`; when `either`, match either
  - [x] Implement the DuckDB→SQLite cross-store position filter path
  - [x] Implement `count_games(search_filter)` or inline the count in `find_games` using `SELECT COUNT(*) OVER()` window function
  - [x] Batch game IDs in chunks of 999 for SQLite IN clause when position filter is active
- [x] Task 3: Update `database_manager` facade (AC: #6)
  - [x] Add `find_games(search_filter)` that coordinates DuckDB position lookup + SQLite metadata query
  - [x] Add `count_games(search_filter)` if separated from `find_games`
- [x] Task 4: Update HTTP endpoint (AC: #1, #2, #5, #6, #7, #8)
  - [x] Extend `GET /api/games` to accept new params: `color` (white|black|either), `position_hash` (uint64 as string), raise max `limit` from 200 to 500
  - [x] Add `total_count` to JSON response
  - [x] Add `white_elo` and `black_elo` to response if not already present (they are — verify)
  - [x] Return HTTP 400 for invalid filter params
- [x] Task 5: Update tests (AC: #9, #10)
  - [x] Test each filter field in isolation: player_name, player_color, min_elo, max_elo, result, eco_prefix, position
  - [x] Test filter combinations (AND semantics)
  - [x] Test empty filter returns all games
  - [x] Test pagination (offset, limit, total_count)
  - [x] Test games without Elo data excluded when Elo filter active
  - [x] Test player_color restriction
  - [x] Test position filter cross-store path
  - [x] Update existing `game_store_test.cpp` and `http_server_test.cpp` tests that used `game_list_query`
- [x] Task 6: Build and lint validation (AC: #10)
  - [x] `cmake --preset=dev && cmake --build build/dev` — zero warnings
  - [x] `ctest --test-dir build/dev` — all tests pass
  - [x] `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize` — zero violations

## Dev Notes

### Scope Boundary

This story creates the shared `search_filter` type that all subsequent 3b stories depend on. It also implements the filtered game list query. It does **not** implement:
- Filtered opening stats (story 3b-2)
- ELO distribution (story 3b-3)
- Filtered opening tree (story 3b-4)
- Any Qt or web frontend changes

### Existing Code to Extend

**`game_list_query`** in `types.hpp:132-149` is the current filter model. It has overlapping fields (`player`, `result`, `eco_prefix`, `date_from`, `date_to`, `min_elo`, `max_elo`, `limit`, `offset`). Replace it with the new `search_filter` type. Update all callers:
- `game_store::list_games()` → becomes `game_store::find_games()`
- `server.cpp` `GET /api/games` handler

**`game_store::list_games`** in `game_store.cpp:675-769` uses a parameterized SQL query with the `? IS NULL OR ...` pattern (19 bind params in `list_param` namespace, lines 105-126). Extend this query with:
- `player_color` filter: modify the `instr(lower(...))` clause to conditionally match only `w.name` or `b.name`
- Position filter: add a subquery or temp table join for game IDs from DuckDB

The bind pattern uses helpers from `sqlite_impl.hpp`: `bind_optional_text(stmt, col, opt)` and `bind_optional_int(stmt, col, opt)`.

### Cross-Store Position Filter Design

When `search_filter::position` is set:
1. Query DuckDB: `SELECT DISTINCT game_id FROM position WHERE zobrist_hash = ?` → get `std::vector<game_id>`
2. Build SQLite query with an additional filter: `AND g.id IN (...)` batched at 999 per clause, OR create a temp table with the game IDs and JOIN against it
3. Recommended: temp table approach — avoids SQLite's 999-parameter limit and is more efficient for large game ID sets. Create temp table, bulk insert IDs, JOIN, drop temp table. This is the pattern used elsewhere in the codebase for batch operations.

The `database_manager` facade should own this coordination since it has access to both `game_store` (SQLite) and `position_store` (DuckDB).

### SQL Guidance

Extended query (metadata filters only, no position filter):

```sql
SELECT g.id, w.name, b.name, COALESCE(g.result,''), COALESCE(e.name,''),
       COALESCE(g.date,''), COALESCE(g.eco,''), COALESCE(g.source_type,'imported'),
       g.source_label, COALESCE(g.review_status,'new'), w.elo, b.elo,
       COUNT(*) OVER() AS total_count
FROM game g
JOIN player w ON w.id = g.white_id
JOIN player b ON b.id = g.black_id
LEFT JOIN event e ON e.id = g.event_id
WHERE (:player_name IS NULL
       OR (CASE :color
             WHEN 'white' THEN instr(lower(w.name), lower(:player_name)) > 0
             WHEN 'black' THEN instr(lower(b.name), lower(:player_name)) > 0
             ELSE instr(lower(w.name), lower(:player_name)) > 0
                  OR instr(lower(b.name), lower(:player_name)) > 0
           END))
  AND (:result IS NULL OR g.result = :result)
  AND (:eco_prefix IS NULL OR g.eco LIKE :eco_prefix || '%')
  AND (:min_elo IS NULL OR (w.elo >= :min_elo AND b.elo >= :min_elo))
  AND (:max_elo IS NULL OR (w.elo <= :max_elo AND b.elo <= :max_elo))
ORDER BY g.id ASC
LIMIT :limit OFFSET :offset
```

When position filter is active, add: `JOIN _filter_games fg ON fg.game_id = g.id` where `_filter_games` is a temp table populated with DuckDB results.

For `total_count`: use `COUNT(*) OVER()` window function so the count is returned with each row without a separate query. Extract from the first row. If no rows, run a separate `SELECT COUNT(*)` with the same WHERE clause minus LIMIT/OFFSET.

### Elo Filter Semantics Change

The existing `game_list_query` uses `OR` for Elo filters (`w.elo >= ? OR b.elo >= ?`). The spec 003 PRD says both sides must satisfy each bound. Change to `AND`:
- `min_elo`: `w.elo >= :min AND b.elo >= :min`
- `max_elo`: `w.elo <= :max AND b.elo <= :max`

This is a **breaking change** from the existing behavior. Verify no tests depend on the OR semantics.

### Date Filter

The existing `game_list_query` has `date_from`/`date_to` but the spec 003 `search_filter` does not include date filters. **Drop date filtering from the new `search_filter`** — it's out of scope for spec 003 (noted in the PRD "Out of Scope" section). If needed, add back in a follow-up.

Alternatively: keep `date_from`/`date_to` in `search_filter` as optional fields for backward compatibility, but they are not tested against spec 003 ACs.

### player_color Implementation

The `player_color` enum controls which player columns the `player_name` filter matches against:
- `either` (default): match against `w.name OR b.name` — same as current behavior
- `white`: match only against `w.name`
- `black`: match only against `b.name`

Use a SQL `CASE` expression on the color parameter to conditionally match. Alternative: build the SQL dynamically (if color is set, add only the relevant column to the `instr` clause). The `CASE` approach avoids dynamic SQL and keeps the prepared-statement pattern.

### HTTP API Contract

```
GET /api/games?player=Carls&color=black&min_elo=2000&result=1-0&eco=B9&position_hash=12345&offset=0&limit=100
```

Response:
```json
{
  "games": [
    {
      "id": 42,
      "white": "Carlsen, Magnus",
      "black": "Nepomniachtchi, Ian",
      "result": "1-0",
      "event": "Candidates",
      "date": "2024.04.20",
      "eco": "B90",
      "white_elo": 2830,
      "black_elo": 2795,
      "source_type": "imported",
      "source_label": "twic",
      "review_status": "new"
    }
  ],
  "total_count": 87
}
```

New params vs existing:
- `color` — NEW (white|black|either, default either)
- `position_hash` — NEW (uint64 as decimal string)
- `limit` max raised from 200 → 500
- `player` — already exists, semantics unchanged
- `result`, `eco`, `min_elo`, `max_elo` — already exist, semantics unchanged (except Elo AND change)
- `date_from`, `date_to` — already exist, keep if `search_filter` retains them

### Testing Guidance

Create a test fixture with a known dataset: 10–20 games with varied Elo, results, ECO codes, and player names. Use this fixture for all filter tests. Verify:

- Each filter field in isolation returns expected game IDs
- Combined filters produce the intersection (AND)
- `player_color` restricts correctly
- Position filter returns only games that reached the queried position
- Empty filter returns all games with correct `total_count`
- Pagination: offset/limit/total_count correctness
- Games without Elo excluded when Elo filter active
- SQLite parameter limit: test with >999 matching game IDs for position filter

Run `cmake --preset=dev-sanitize` to verify zero ASan/UBSan violations.

### Previous Story Intelligence

- **Story 4b-5** (game list endpoint): Established the `game_list_query` type and the `? IS NULL OR ...` SQL pattern. This story extends that work.
- **Story 4d-4** (file upload): Added `count_games()` and `source_type`/`review_status` fields. The `count_games` has no filter support — this story adds it.
- **Recent refactoring**: `game_id` is now a strong struct (not a `using` alias). All APIs use `game_id` instead of `uint32_t`. `game_store::list_games` takes `game_list_query const&`. The new `find_games` should follow the same pattern.
- **Chess facade**: `motif::chess` module wraps chesslib. Position-related queries may need the facade for hash computation in tests.

### References

- [Source: source/motif/db/types.hpp#L132-149] — existing `game_list_query` to be replaced
- [Source: source/motif/db/game_store.cpp#L675-769] — existing `list_games` SQL query
- [Source: source/motif/db/game_store.cpp#L105-126] — `list_param` bind parameter indices
- [Source: source/motif/db/sqlite_impl.hpp#L68-84] — `bind_optional_text`/`bind_optional_int` helpers
- [Source: source/motif/db/position_store.hpp#L26] — `query_by_zobrist` for position-based game ID lookup
- [Source: source/motif/http/server.cpp#L952-1033] — `GET /api/games` handler
- [Source: _bmad-output/planning-artifacts/prd-003-search.md] — spec 003 PRD with FR01–FR09, NFR01–NFR12
- [Source: _bmad-output/planning-artifacts/epics.md#Story-3b.1] — epic AC definitions
- [Source: _bmad-output/planning-artifacts/architecture.md#Module-Structure] — module boundaries (`motif_search` never accesses SQLite/DuckDB directly)
- [Source: CONVENTIONS.md] — SQL must use raw string literals, DuckDB C API only, no mocks for storage
- [Source: source/motif/db/game_store.hpp#L60] — `list_games` API signature to deprecate

## Dev Agent Record

### Agent Model Used

gpt-5.4

### Debug Log References

- `cmake --build build/dev`
- `ctest --test-dir build/dev --output-on-failure`
- `cmake --preset=dev-sanitize`
- `cmake --build build/dev-sanitize`
- `ctest --test-dir build/dev-sanitize --output-on-failure`
- `ctest --test-dir build/dev-sanitize --output-on-failure -R "game_store: find_games|database_manager::find_games|server: game list"`
- `ctest --test-dir build/dev-sanitize --output-on-failure -R "opening_tree::open from starting position aggregates first moves correctly|opening_tree::open returns root with correct continuations at depth 1"`

### Completion Notes List

- Replaced `game_list_query` with `search_filter` and `game_list_result`, including `player_color`, `position`, and shared default/max search limit constants.
- Implemented metadata filtering in `game_store::find_games()` with case-insensitive player matching, ANDed Elo bounds, pagination, and total-count reporting.
- Added the DuckDB-backed cross-store position filter path in `database_manager::find_games()` using ordered distinct game IDs from `position_store` and batched SQLite `IN` clauses.
- Updated `GET /api/games` to accept `color` and `position_hash`, clamp `limit` to 500, and return `{ "games": [...], "total_count": ... }`.
- Added DB and HTTP coverage for isolated filters, AND semantics, pagination, total counts, color restriction, and position intersection.
- Fixed a pre-existing `opening_tree::open()` ASan bug by avoiding invalid lifetime when caching `chesslib::board` values during tree construction.
- `cmake --build build/dev` and `ctest --test-dir build/dev --output-on-failure` passed.
- Touched-path sanitizer coverage passed via `ctest --test-dir build/dev-sanitize --output-on-failure -R "game_store: find_games|database_manager::find_games|server: game list"`.
- `cmake --build build/dev-sanitize` and `ctest --test-dir build/dev-sanitize --output-on-failure` passed after the `opening_tree` lifetime fix.

### File List

- source/motif/db/database_manager.cpp
- source/motif/db/database_manager.hpp
- source/motif/db/game_store.cpp
- source/motif/db/game_store.hpp
- source/motif/db/position_store.cpp
- source/motif/db/position_store.hpp
- source/motif/db/types.hpp
- source/motif/http/server.cpp
- source/motif/search/opening_tree.cpp
- test/source/motif_db/database_manager_test.cpp
- test/source/motif_db/game_store_test.cpp
- test/source/motif_http/http_server_test.cpp

### Review Findings

- [x] [Review][Patch] `bind_find_filter` silently ignores `bind_optional_text`/`bind_optional_int` return values — a failed bind produces wrong query results with no error propagated [source/motif/db/game_store.cpp:133-150]
- [x] [Review][Patch] `find_games` runs COUNT and SELECT as two independent statements with no wrapping transaction — concurrent writes between them cause `total_count` to disagree with the rows returned [source/motif/db/game_store.cpp:796-872]
- [x] [Review][Patch] No test covering the >999 position-ID batch boundary — `SQLITE_LIMIT_VARIABLE_NUMBER` path is untested despite being called out in the spec testing guidance [test/source/motif_db/game_store_test.cpp]
- [x] [Review][Defer] `database_manager::find_games` acquires and releases the mutex between the DuckDB call and the SQLite call — pre-existing design; safe for current single-HTTP-request callers but fragile for future internal callers [source/motif/db/database_manager.cpp:88-101] — deferred, pre-existing

### Change Log

- 2026-05-06: Applied three code-review patches: checked bind return values in `bind_find_filter`; wrapped COUNT+SELECT in `txn_guard` for both `find_games` and `find_games_with_ids`; added >999 batch-boundary test. 277/277 tests pass under ASan+UBSan. Story approved.
- 2026-05-06: Implemented `search_filter`-based game list filtering across SQLite and DuckDB, updated `/api/games`, added DB/HTTP coverage, and fixed the `opening_tree` board-lifetime bug that was blocking the sanitize suite.
