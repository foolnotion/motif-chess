# Story 4d.3: Personal Games CRUD API and LAN Support

Status: done

<!-- Ultimate context engine analysis completed - comprehensive developer guide created -->

## Story

As a developer building motif-chess-web as a domain-native replacement for a required CRUD exercise,
I want backend endpoints for user-added personal games plus simple trusted-LAN configuration,
so that the web frontend can add, list/search, open, edit, and delete chess games without inventing a non-chess TODO feature.

## Acceptance Criteria

1. **Given** the server is running with an open motif-chess database
   **When** `POST /api/games` is called with pasted single-game PGN text
   **Then** the PGN is parsed and validated through `pgnlib`/`chesslib` and persisted through the existing SQLite + DuckDB storage path
   **And** HTTP 201 returns the created game ID plus normalized game metadata
   **And** the created game is marked with user/manual provenance so future edits and deletes can be constrained safely
   **And** malformed PGN, invalid SAN, duplicate games, and empty input return user-readable JSON errors without crashing

2. **Given** a user-added game exists
   **When** `GET /api/games` or `GET /api/games/{id}` is called
   **Then** existing read endpoints include provenance fields in their responses
   **And** user-added game responses expose at minimum `source_type`, optional `source_label`, and `review_status`
   **And** imported/reference games are clearly distinguishable from user/manual games

3. **Given** a user-added game exists
   **When** `PATCH /api/games/{id}` is called with valid editable metadata
   **Then** the editable fields are updated and HTTP 200 returns the normalized updated game
   **And** allowed fields are limited to supported metadata/status fields: player names/Elo where supported, event, site, date, result, source label, notes/tags, and `review_status`
   **And** `review_status` accepts only `new`, `needs_review`, `studied`, or `archived`
   **And** invalid IDs, unknown fields, invalid enum values, invalid result values, and malformed JSON return HTTP 400
   **And** missing games return HTTP 404

4. **Given** a non-user-added imported/reference game exists
   **When** `PATCH /api/games/{id}` or `DELETE /api/games/{id}` is called
   **Then** the backend rejects the request with HTTP 409 and a JSON error
   **And** no imported/reference corpus game metadata, moves, or derived position rows are changed

5. **Given** a user-added game exists
   **When** `DELETE /api/games/{id}` is called
   **Then** the game row and associated tags are removed from SQLite
   **And** the corresponding DuckDB derived position rows are removed or the position store is rebuilt according to existing storage invariants
   **And** a subsequent `GET /api/games/{id}` returns HTTP 404
   **And** position search/opening-stat responses no longer include the deleted game

6. **Given** browser/mobile clients cannot provide server-side file paths
   **When** PGN ingestion is needed from the web frontend
   **Then** the implemented API supports PGN text supplied in an HTTP request body
   **And** `POST /api/games` is the single-game pasted-PGN ingestion path for the CRUD flow
   **And** bulk browser/mobile import (`POST /api/imports/pgn` or multipart upload) is documented as out of scope for this story unless it can be implemented without expanding the storage/import lifecycle design

7. **Given** motif-chess-web may run from another trusted LAN device during development
   **When** the HTTP server starts
   **Then** the bind host and port are configurable, with local development still defaulting to `localhost:8080`
   **And** CORS allowed origins are configurable explicitly for frontend/mobile clients
   **And** no authentication, pairing, multi-user support, remote hosting, or public-network hardening is added in this story

8. **Given** `docs/api/openapi.yaml` is updated
   **When** the OpenAPI document is reviewed
   **Then** it documents `POST /api/games`, `PATCH /api/games/{id}`, `DELETE /api/games/{id}`, provenance fields on game responses, request/response schemas, error schemas, and examples
   **And** the local-network host/origin configuration is described in prose or server metadata where appropriate

9. **Given** all changes are implemented
   **When** HTTP integration tests run
   **Then** they cover create from PGN, read created game through the existing endpoint, list/search visibility, patch success, delete success, malformed PGN 400, invalid patch 400, not-found 404, and imported/reference patch/delete rejection
   **And** tests prove storage consistency between SQLite game rows and DuckDB position rows after create and delete

10. **Given** all changes are implemented
    **When** `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev` is run
    **Then** all tests pass with zero new clang-tidy or cppcheck warnings
    **And** `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize` passes with zero ASan/UBSan violations

## Tasks / Subtasks

- [ ] Task 1: Extend game metadata/provenance model safely (AC: 1, 2, 3, 4, 5)
  - [ ] Add domain/API representation for game provenance: `source_type` (`imported`, `manual`, optionally `reference` if needed), optional `source_label`, and `review_status`
  - [ ] Prefer representing `review_status` as a constrained string/enum at the API boundary with allowed values `new`, `needs_review`, `studied`, `archived`
  - [ ] Decide whether `notes` is a first-class column or stored as a reserved game tag; document the choice in the story Dev Agent Record
  - [ ] Preserve imported/reference games by defaulting existing imported games to non-editable provenance, not to `manual`
  - [ ] Ensure all game response DTOs include provenance fields without breaking existing list/detail response semantics

- [ ] Task 2: Add SQLite schema evolution for provenance/editable metadata (AC: 1, 2, 3, 4)
  - [ ] Update `source/motif/db/schema.hpp` `current_version` and schema initialization to include any new `game` columns
  - [ ] Update both schema creation paths currently present in `schema.cpp` and `game_store::create_schema()`; do not let the two DDL definitions drift
  - [ ] If existing database bundles must open after this change, add a simple migration path rather than only bumping `PRAGMA user_version` and rejecting old bundles
  - [ ] Add storage tests for new schema defaults: imported/reference rows are non-editable; `POST /api/games` rows are manual/editable
  - [ ] Keep SQL in raw string literals and use SQLite table/column naming conventions from `CONVENTIONS.md`

- [ ] Task 3: Add storage APIs for user-game create/update/delete (AC: 1, 3, 4, 5)
  - [ ] Add `game_store` APIs as needed: create/manual insert support, provenance-aware `patch_metadata`, provenance check helper, and `remove_user_game` or equivalent
  - [ ] Ensure `PATCH` updates player/event references safely; if player/event deduplication makes in-place edits unsafe, create/reuse player/event rows and repoint the game row instead of mutating shared player/event rows
  - [ ] Ensure metadata updates do not modify the moves blob; changing moves from `PATCH` is out of scope
  - [ ] Ensure duplicate identity conflicts return a distinct error path that maps to HTTP 409
  - [ ] Add or expose a `position_store` delete-by-game-id operation, or use `database_manager::rebuild_position_store()` after deletion if that is the only safe option
  - [ ] Keep SQLite authoritative and DuckDB derived; do not introduce cross-store joins or a two-phase commit

- [ ] Task 4: Add single-game PGN text ingestion path (AC: 1, 6)
  - [ ] Add HTTP request DTO for `POST /api/games`, e.g. `{ "pgn": "...", "source_label": "...", "review_status": "new" }`
  - [ ] Parse and validate exactly one game from the submitted PGN using the existing PGN/chess libraries; reject empty input and multi-game ambiguity unless deliberately supported and documented
  - [ ] Reuse the existing per-game import processing path (`motif::import::import_worker` or extracted shared helper) so SQLite insert and DuckDB position indexing match bulk import behavior
  - [ ] Map `pgnlib` parse errors and `chesslib` SAN errors to user-readable HTTP 400 JSON responses
  - [ ] Map duplicate game insertion to HTTP 409 unless product intent explicitly chooses idempotent create
  - [ ] Mark created rows as `manual` provenance before returning

- [ ] Task 5: Implement `POST /api/games`, `PATCH /api/games/{id}`, and `DELETE /api/games/{id}` in `motif_http` (AC: 1, 3, 4, 5)
  - [ ] Add HTTP-local DTOs in `motif::http::detail`; do not leak frontend wire types into unrelated modules
  - [ ] Register routes with correct ordering relative to existing `GET /api/games` and `GET /api/games/:id`
  - [ ] Reuse `parse_game_id`, `set_json_error`, `glz::read_json`, `glz::write_json`, and existing `database_mutex` patterns for DB operations
  - [ ] Return HTTP 201 for successful create, HTTP 200 for successful patch, and HTTP 204 for successful delete
  - [ ] Return HTTP 400 for malformed JSON/invalid fields, HTTP 404 for missing game IDs, HTTP 409 for duplicate create or non-editable imported/reference game mutation attempts
  - [ ] Do not add auth, sessions, user accounts, or destructive imported-corpus deletion in this story

- [ ] Task 6: Add local-network bind/CORS configuration (AC: 7)
  - [ ] Extend `source/motif/http/server.hpp` / `server.cpp` so `server::start` can bind to a configurable host as well as a port; current public API accepts only `port`
  - [ ] Extend `source/motif/http/main.cpp` argument/env parsing to support configurable bind host in addition to port
  - [ ] Preserve default local development behavior: bind host `localhost`, port `8080`
  - [ ] Add explicit allowed CORS origins configuration via CLI and/or environment variable; keep wildcard only if no explicit origin is configured and existing tests expect dev permissiveness
  - [ ] Ensure `Access-Control-Allow-Origin` echoes only configured allowed origins when explicit origins are set
  - [ ] Add tests or focused coverage for configured origin behavior without introducing auth/pairing

- [ ] Task 7: Update OpenAPI (AC: 2, 3, 4, 5, 6, 8)
  - [ ] Document `POST /api/games` with request body, 201 response, 400 malformed PGN, and 409 duplicate examples
  - [ ] Document `PATCH /api/games/{id}` with editable fields, 200/400/404/409 responses, and review-status enum
  - [ ] Document `DELETE /api/games/{id}` with 204/400/404/409 responses
  - [ ] Update `GameListEntry` and full game response schemas to include provenance fields
  - [ ] Include examples for manual created game, imported/reference non-editable error, and PGN parse error
  - [ ] Document that `POST /api/games` is the browser/mobile pasted-PGN path for one game; bulk browser/mobile PGN import remains a separate future surface unless implemented here

- [ ] Task 8: Add HTTP and storage integration tests (AC: 1, 2, 3, 4, 5, 9)
  - [ ] Test create from valid pasted PGN returns 201, created game ID, normalized metadata, and `source_type: manual`
  - [ ] Test `GET /api/games/{id}` can read the created game and includes provenance
  - [ ] Test `GET /api/games` lists/searches the created game using existing filters where applicable
  - [ ] Test patching supported metadata/status on a manual game returns the updated response
  - [ ] Test deleting a manual game removes it from `GET /api/games/{id}` and position search/opening stats
  - [ ] Test malformed PGN and invalid SAN return 400 with user-readable errors
  - [ ] Test invalid patch fields/status/result return 400 and missing IDs return 404
  - [ ] Seed an imported/reference game and prove patch/delete return 409 without changing it
  - [ ] Test SQLite/DuckDB consistency after create and delete, using real in-memory/on-disk test database managers per existing patterns

- [ ] Task 9: Validation (AC: 10)
  - [ ] Run `cmake --preset=dev`
  - [ ] Run `cmake --build build/dev`
  - [ ] Run `ctest --test-dir build/dev`
  - [ ] Run `cmake --preset=dev-sanitize`
  - [ ] Run `cmake --build build/dev-sanitize`
  - [ ] Run `ctest --test-dir build/dev-sanitize`
  - [ ] Apply `clang-format` to all touched C++ files and record results in the Dev Agent Record

## Dev Notes

### Scope Boundary

This story exists to make motif-chess-web satisfy a real full-stack CRUD learning requirement using the project’s actual domain. The CRUD object is a user-added chess game, not an artificial TODO entity.

Keep the scope backend-only:
- No frontend UI work.
- No authentication, multi-user accounts, public-network exposure, or pairing.
- No engine analysis work.
- No destructive imported/reference corpus deletion.
- No storage architecture rewrite unless required for correctness.

The smallest acceptable browser/mobile PGN ingestion path is `POST /api/games` with PGN text for one game. Bulk browser/mobile import (`POST /api/imports/pgn` or multipart upload) can be split into a later story if it starts to expand import lifecycle, checkpoint, progress, or temp-file design.

### Current Backend Surface

Already implemented:
- `GET /health`
- `GET /api/games`
- `GET /api/games/{id}`
- `GET /api/positions/{zobrist_hash}`
- `GET /api/openings/{zobrist_hash}/stats`
- `POST /api/imports` with server-side path
- `GET /api/imports/{import_id}/progress`
- `DELETE /api/imports/{import_id}`
- `GET /api/positions/legal-moves`
- `POST /api/positions/apply-move`

This story adds mutating game endpoints and provenance guardrails around them.

### Storage Facts and Constraints

Current domain/storage state:
- `motif::db::game` has players, optional event, date, result, optional ECO, encoded moves, and extra tags.
- `game_store::insert()` returns `error_code::duplicate` for duplicates; there is no skip-duplicates option.
- `game_store::remove()` deletes SQLite `game` and `game_tag` rows but does not currently remove DuckDB position rows.
- `position_store` currently has `insert_batch`, query methods, and no public delete-by-game-id API.
- `database_manager::rebuild_position_store()` can rebuild DuckDB from SQLite and is the safe fallback if targeted DuckDB delete is not added.
- SQLite is authoritative; DuckDB is derived and rebuildable (NFR09).
- Existing schema version is `schema::current_version = 1`; adding provenance columns likely requires version/migration work.

Critical guardrail: deleting a game must not leave stale DuckDB `position` rows. Either add a targeted DuckDB delete operation for `game_id` or rebuild the position store after deletion. Do not claim delete is complete if position search can still find the removed game.

### Provenance Design Guidance

Use explicit provenance to constrain patch/delete:
- `source_type`: string enum at API boundary. Minimum values: `manual`, `imported`. Add `reference` only if existing data model needs it.
- `source_label`: optional user-facing label such as `motif-chess-web`, `lichess blitz`, or `manual PGN`.
- `review_status`: string enum: `new`, `needs_review`, `studied`, `archived`.

Existing imported/reference games must default to non-editable. Do not infer that absence of provenance means editable.

For player/event edits, remember players/events are deduplicated shared entities. A patch to game 12’s white player name must not rename a shared player row used by game 99. Prefer find-or-insert/repoint semantics for game rows.

### PGN Parsing Guidance

The implementation should reuse existing PGN/chess logic rather than duplicate it:
- `motif::import::import_worker::process(pgn::game const&)` already converts a single `pgn::game` into SQLite game row plus DuckDB position rows.
- It extracts metadata in `import_worker.cpp`, parses SAN through `chesslib::san::from_string`, encodes moves with `chesslib::codec::encode`, and inserts DuckDB `position_row`s.
- If `import_worker::process` needs a provenance-aware variant, prefer extending it or extracting a shared helper over creating a parallel HTTP-only PGN conversion path.

Read pgnlib docs/examples before using any new pgnlib API. If pgnlib lacks an in-memory single-game parser, use the least invasive adapter that preserves current parser behavior and document the choice.

### HTTP Implementation Patterns

Use established `server.cpp` patterns:
- DTOs live in `motif::http::detail`.
- JSON parsing: `glz::read_json`.
- JSON writing: `glz::write_json`.
- Errors: `set_json_error(res, status, message)`.
- Game IDs: existing `parse_game_id`.
- DB/search operations: protect with existing `database_mutex`.
- Route order matters: exact routes and static paths before parameterized `:id` routes.
- `httplib.h` remains confined to `server.cpp`.

HTTP status mapping guidance:
- `POST /api/games` success: 201 Created.
- `PATCH /api/games/{id}` success: 200 OK with updated game.
- `DELETE /api/games/{id}` success: 204 No Content.
- Invalid JSON/fields/IDs/PGN: 400.
- Missing game: 404.
- Duplicate create or imported/reference mutation attempt: 409.

### Local-Network Configuration

Current `source/motif/http/main.cpp` supports:
- `--db <path>` / `MOTIF_DB_PATH`
- `--port <port>` / `MOTIF_HTTP_PORT`
- default port from `motif::http::server::default_port`

Current `source/motif/http/server.hpp` only exposes `server::start(std::uint16_t port = default_port)`. Host binding requires a small public API change, such as adding a host parameter or an options struct. Keep `httplib.h` hidden behind the existing pImpl.

This story should add:
- bind host configuration, defaulting to `localhost`
- explicit allowed CORS origins for trusted local frontend/mobile clients

Keep the language honest in docs and errors: trusted LAN only, no auth. Do not imply this is safe for public network exposure.

### Architecture Compliance

- `motif_http` remains an adapter over `motif_db` and `motif_import`.
- `motif_import` and `motif_search` must not include SQLite/DuckDB headers directly except through `motif_db` APIs.
- DuckDB code must use the C API only.
- No Qt headers in `motif_db`, `motif_import`, `motif_search`, `motif_engine`, or `motif_http`.
- No new dependencies; do not modify `flake.nix` or `vcpkg.json`.
- All identifiers use `lower_snake_case`.
- Public fallible APIs return `tl::expected<T, motif::<module>::error_code>`.
- Use `fmt`, not `std::format`, `std::ostringstream`, `std::cout`, or `std::cerr`.

### Previous Story Intelligence

Story 4d.1 established:
- Legal moves/apply-move endpoints are stateless and do not acquire `database_mutex`; this story is different because it mutates storage and must use existing DB locking patterns.
- Strict input validation belongs in HTTP before delegating to library helpers when library syntax acceptance is broader than the API contract.
- Code review found and fixed test gaps by strengthening exact contract assertions; apply the same standard here.
- Full `dev` and `dev-sanitize` gates must be rerun before marking done.

Story 4d.2 is `ready-for-dev` and focuses on engine API contract. This `4d.3` story is independent of engine analysis; do not mix the two.

Epic 4b established the game list/detail routes and HTTP test patterns. Reuse those tests’ `tmp_dir`, `wait_for_ready`, hardcoded unused ports, and `httplib::Client` style.

### References

- [Source: `_bmad-output/planning-artifacts/epics.md` — Epic 4d, Epic 4b game/import endpoints, FR02, FR05, FR06, FR09, FR24, FR26]
- [Source: `_bmad-output/planning-artifacts/prd.md` — local web frontend course correction, local-first constraints, NFR09, NFR10]
- [Source: `_bmad-output/planning-artifacts/architecture.md` — HTTP adapter, local deployment constraints, SQLite authoritative/DuckDB derived model]
- [Source: `CONVENTIONS.md` — naming, SQL, DuckDB C API, fmt, testing gates]
- [Source: `source/motif/http/server.cpp` — route registration, DTOs, `parse_game_id`, `set_json_error`, CORS, DB mutex]
- [Source: `source/motif/http/main.cpp` — CLI/env parsing for DB path and port]
- [Source: `source/motif/db/types.hpp` — current game/player/event data model]
- [Source: `source/motif/db/game_store.hpp` / `.cpp` — insert/get/list/remove behavior and duplicate policy]
- [Source: `source/motif/db/position_store.hpp` — DuckDB position APIs; no delete-by-game-id yet]
- [Source: `source/motif/db/database_manager.hpp` — rebuild position store fallback]
- [Source: `source/motif/import/import_worker.hpp` / `.cpp` — single-game PGN-to-storage processing]
- [Source: `docs/api/openapi.yaml` — current API contract organization]
- [Source: `test/source/motif_http/http_server_test.cpp` — HTTP integration test patterns]

## Dev Agent Record

### Agent Model Used

{{agent_model_name_version}}

### Debug Log References

### Completion Notes List

### File List

### Change Log
