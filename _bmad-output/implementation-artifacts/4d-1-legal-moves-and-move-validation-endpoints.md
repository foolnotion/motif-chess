# Story 4d.1: Legal Moves and Move Validation Endpoints

Status: done

<!-- Ultimate context engine analysis completed - comprehensive developer guide created -->

## Story

As a developer building an interactive web chessboard,
I want HTTP endpoints that return legal moves for a FEN position and validate/apply a candidate move,
so that a display-only frontend board can highlight legal destinations and update only after the backend confirms the move.

## Acceptance Criteria

1. **Given** the server is running
   **When** `GET /api/positions/legal-moves?fen=<url-encoded-fen>` is called with a valid FEN
   **Then** HTTP 200 is returned with the canonical FEN and a `legal_moves` array
   **And** each move contains at minimum `uci`, `san`, `from`, `to`, and optional `promotion`
   **And** SAN/FEN parsing and move legality are delegated to `chesslib`; motif-chess does not reimplement chess rules (NFR19)

2. **Given** the frontend board has a FEN and a candidate drag/drop move
   **When** `POST /api/positions/apply-move` is called with the FEN and candidate move in UCI notation
   **Then** a legal move returns HTTP 200 with the accepted `uci`, `san`, and resulting FEN
   **And** an illegal move returns HTTP 400 with a JSON error response
   **And** the frontend can keep the board renderer free of chess logic by updating only from the returned FEN

3. **Given** the FEN is missing, malformed, or illegal
   **When** either endpoint is called
   **Then** HTTP 400 is returned with a JSON error response
   **And** no undefined behavior, exception escape, or process crash occurs (NFR10)

4. **Given** `docs/api/openapi.yaml` is updated
   **When** the OpenAPI document is reviewed
   **Then** the legal moves and apply-move routes, query/body constraints, response schemas, error schemas, and examples are documented
   **And** C++ integration tests cover initial position moves, castling availability, promotion moves, invalid FEN, and check-constrained moves
   **And** apply-move tests cover legal moves, illegal moves, promotions, and resulting-FEN correctness

5. **Given** all changes are implemented
   **When** `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev` is run
   **Then** all tests pass with zero new clang-tidy or cppcheck warnings

6. **Given** all changes are implemented
   **When** `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize` is run
   **Then** all tests pass with zero ASan/UBSan violations

## Tasks / Subtasks

- [x] Task 1: Add HTTP-local DTOs and chess helper functions (AC: 1, 2, 3)
  - [x] Add DTOs in `source/motif/http/server.cpp` under `motif::http::detail`: `legal_move_response`, `legal_moves_response`, `apply_move_request`, and `apply_move_response`
  - [x] Keep DTOs HTTP-local; do not add frontend wire types to `motif_db`, `motif_search`, or `motif_engine`
  - [x] Add helper(s) near existing anonymous-namespace HTTP conversion helpers for FEN parsing, move conversion, UCI square extraction, and JSON serialization
  - [x] Use `chesslib::fen::read`, `chesslib::legal_moves`, `chesslib::uci::{from_string,to_string}`, `chesslib::san::to_string`, and `chesslib::move_maker`; do not implement chess rules, legal move filtering, SAN, UCI, or FEN parsing in motif-chess
  - [x] Use the non-throwing `chesslib::fen::read` API, not `chesslib::board(std::string_view)` or `chesslib::fen::read_or_throw`

- [x] Task 2: Implement `GET /api/positions/legal-moves` (AC: 1, 3)
  - [x] Register an exact route before the existing `"/api/positions/:zobrist_hash"` route so `legal-moves` is not interpreted as a Zobrist path parameter
  - [x] Require the `fen` query parameter; missing or empty FEN returns HTTP 400 with `{"error":"invalid fen"}` or equivalent stable error text
  - [x] Parse FEN with `chesslib::fen::read`; parse errors return HTTP 400 without exception escape
  - [x] Return canonical FEN using `board.export_fen()` or `chesslib::fen::write(board)`
  - [x] Generate legal moves with `chesslib::legal_moves(board)` from `chesslib/board/move_generator.hpp`
  - [x] For each move, return `uci`, `san`, `from`, `to`, and optional `promotion`; derive `from`/`to` from the UCI string (`e2e4`, `a7a8q`) rather than square enum names
  - [x] Ensure promotion uses the lowercase UCI promotion suffix (`q`, `r`, `b`, `n`) when present; omit `promotion` for non-promotion moves

- [x] Task 3: Implement `POST /api/positions/apply-move` (AC: 2, 3)
  - [x] Parse a JSON body with required `fen` and `uci` strings; invalid JSON or missing/empty fields returns HTTP 400
  - [x] Parse FEN with `chesslib::fen::read`; malformed or illegal FEN returns HTTP 400
  - [x] Parse and validate the candidate move with `chesslib::uci::from_string(board, uci)`; illegal or malformed UCI returns HTTP 400
  - [x] Compute SAN from the original position before mutating the board
  - [x] Apply the accepted move with `chesslib::move_maker {board, move}.make()`
  - [x] Return HTTP 200 with accepted `uci`, `san`, and `fen` containing the resulting canonical FEN

- [x] Task 4: Update OpenAPI in the same story (AC: 4)
  - [x] Add `GET /api/positions/legal-moves` with required `fen` query parameter and 200/400 responses
  - [x] Add `POST /api/positions/apply-move` with request body schema and 200/400 responses
  - [x] Add schemas for `LegalMove`, `LegalMovesResponse`, `ApplyMoveRequest`, and `ApplyMoveResponse`
  - [x] Include examples for the initial position, a promotion move, an invalid FEN error, and an illegal move error
  - [x] Keep examples frontend-safe: FEN and UCI are strings; no numeric hash representation is introduced by this story

- [x] Task 5: Add HTTP integration tests (AC: 1, 2, 3, 4)
  - [x] Add tests to `test/source/motif_http/http_server_test.cpp` following the existing server/client pattern and using currently unused ports
  - [x] Test initial position legal moves: response is HTTP 200, canonical FEN is present, and there are 20 legal moves including `e2e4` with SAN `e4`
  - [x] Test castling availability with a FEN where castling is legal; assert `e1g1`/`O-O` (or black-side equivalent) appears
  - [x] Test promotion moves with a promotion-ready FEN; assert all expected promotion UCI suffixes are represented and `promotion` is present
  - [x] Test check-constrained moves with a checked king position; assert illegal non-evasion moves are absent
  - [x] Test missing FEN, malformed FEN, and structurally illegal FEN return HTTP 400 JSON errors
  - [x] Test apply-move legal move: `e2e4` from the initial FEN returns SAN `e4` and the expected resulting FEN
  - [x] Test apply-move illegal move: malformed UCI or illegal source/target returns HTTP 400 JSON error and does not mutate server state
  - [x] Test apply-move promotion: promotion UCI returns HTTP 200, SAN includes promotion, and resulting FEN is correct

- [x] Task 6: Validation (AC: 5, 6)
  - [x] Run `cmake --preset=dev`
  - [x] Run `cmake --build build/dev`
  - [x] Run `ctest --test-dir build/dev`
  - [x] Run `cmake --preset=dev-sanitize`
  - [x] Run `cmake --build build/dev-sanitize`
  - [x] Run `ctest --test-dir build/dev-sanitize`
  - [x] Apply `clang-format` to all touched C++ files and record results in the Dev Agent Record

### Review Findings

- [x] [Review][Dismissed] Clarify whether "illegal FEN" requires semantic position legality beyond `chesslib::fen::read` — resolved by checking local chesslib: `fen::read` is documented as malformed-FEN parsing and no public semantic-position validation API exists. For this story, invalid/illegal FEN means rejected by chesslib parsing; semantic validation would require upstream chesslib support.
- [x] [Review][Patch] Strictly reject malformed five-character UCI input before applying moves [source/motif/http/server.cpp:539]
- [x] [Review][Patch] Assert castling SAN in the legal-moves integration test [test/source/motif_http/http_server_test.cpp:2101]
- [x] [Review][Patch] Assert exact promotion resulting FEN in the apply-move promotion test [test/source/motif_http/http_server_test.cpp:2252]
- [x] [Review][Patch] Re-run or correct the full dev and sanitize validation evidence for AC5/AC6 [_bmad-output/implementation-artifacts/4d-1-legal-moves-and-move-validation-endpoints.md:90]

## Dev Notes

### Scope Boundary

This story is an HTTP API prerequisite for a separate local web frontend. It is not Qt desktop work and does not start the deferred Epic 4 desktop application.

Keep the implementation inside `motif_http` and `docs/api/openapi.yaml`. The HTTP layer owns request validation, JSON DTOs, route registration, CORS behavior, and OpenAPI documentation. `chesslib` owns FEN parsing, UCI parsing, legal move generation, SAN generation, and move application. No new dependency is needed; do not modify `flake.nix` or `vcpkg.json`.

### Current HTTP Structure

The route implementation lives in `source/motif/http/server.cpp`. Public `httplib.h` exposure is intentionally hidden behind the pImpl in `source/motif/http/server.hpp`. All existing JSON response DTOs live in `motif::http::detail` because glaze aggregate reflection cannot reflect anonymous-namespace types.

Important existing patterns to preserve:

- `set_json_error(res, status, message)` writes `{"error":"..."}` using glaze.
- CORS is already registered globally through `register_cors`.
- Existing route handlers use `glz::read_json` and `glz::write_json`.
- `database_mutex` protects DB/search operations, but these new endpoints do not touch the database and should not acquire it.
- `GET /api/positions/:zobrist_hash` already exists, so register `GET /api/positions/legal-moves` before the parameter route.

### chesslib API Facts

Use these existing chesslib APIs:

- `#include <chesslib/util/fen.hpp>` for `chesslib::fen::read(std::string_view)` and `chesslib::fen::write(board const&)`
- `#include <chesslib/board/move_generator.hpp>` for `chesslib::legal_moves(board const&)`
- `#include <chesslib/util/uci.hpp>` for `chesslib::uci::from_string(board const&, std::string_view)` and `chesslib::uci::to_string(move)`
- `#include <chesslib/util/san.hpp>` for `chesslib::san::to_string(board const&, move)`
- `#include <chesslib/board/board.hpp>` for `board::export_fen()`
- `chesslib::move_maker {board, move}.make()` applies an accepted move

`chesslib::fen::read` returns `tl::expected<chesslib::board, chesslib::fen::parse_error>`. Treat any parse error as HTTP 400. Avoid `read_or_throw` and avoid `board(std::string_view)` because those are throwing convenience paths.

`chesslib::legal_moves` internally generates pseudo-legal moves and filters king safety via `move_maker::check()`. Do not duplicate this filtering in the HTTP layer.

`chesslib::uci::from_string` parses UCI into a legal move for the given board and returns `tl::expected<chesslib::move, std::string>`. Use this for apply-move validation; do not manually search the legal move list unless the API proves insufficient during implementation.

### Suggested Wire Shapes

Use stable, frontend-oriented JSON names:

```json
{
  "fen": "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
  "legal_moves": [
    {"uci": "e2e4", "san": "e4", "from": "e2", "to": "e4"}
  ]
}
```

Promotion move example:

```json
{"uci": "a7a8q", "san": "a8=Q", "from": "a7", "to": "a8", "promotion": "q"}
```

Apply-move request:

```json
{"fen": "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "uci": "e2e4"}
```

Apply-move response:

```json
{"uci": "e2e4", "san": "e4", "fen": "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"}
```

If glaze serializes `std::optional<std::string>` as `null` rather than omitting it in the current configuration, either representation is acceptable only if the OpenAPI schema matches the actual implementation. Prefer omission for non-promotion moves if it matches existing optional-field behavior.

### Testing Guidance

Tests belong in `test/source/motif_http/http_server_test.cpp`. Reuse existing helpers such as `tmp_dir`, `wait_for_ready`, and the `httplib::Client` pattern. Existing tests use hardcoded ports; continue that pattern with unused ports and do not refactor all HTTP tests for dynamic port allocation in this story.

Useful FEN fixtures:

- Initial position: `rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1`
- Castling availability: `r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1`
- Promotion: `8/P7/8/8/8/8/8/4k2K w - - 0 1`
- In check: choose a simple legal FEN where side to move is in check and only king moves or captures/blocking moves are legal; assert a normal pawn move from elsewhere is absent

For resulting-FEN assertions, prefer exact expected FEN for simple fixtures. If a chesslib canonicalization detail differs, assert the board-equivalent canonical string returned by `chesslib::fen::write` from the same operation in the test.

### Architecture Compliance

- `motif_http` remains a thin adapter; do not move chessboard helper logic into `motif_search`, `motif_import`, or `motif_db`.
- Keep `httplib.h` confined to `server.cpp`; do not expose it through headers.
- Keep JSON DTOs in `motif::http::detail`.
- Use `fmt::format` for string construction and `fmt::print` for any touched console output. Do not add `std::format`, `std::ostringstream`, `std::to_string`, `std::cout`, or `std::cerr`.
- All identifiers use `lower_snake_case`; every new public/fallible API must use `tl::expected` if any is added. This story likely needs only private helpers returning `std::optional` or `tl::expected`.
- No Qt headers in `motif_http` dependencies and no new dependency additions.

### Previous Story Intelligence

Epic 4b established the HTTP adapter, route registration, CORS, JSON error responses, and integration-test style. Epic 4c hardened hash wire formats and import/SSE lifecycle. Carry forward these lessons:

- OpenAPI updates happen in the same story as route implementation.
- Frontend-visible values should be represented in browser-safe strings where relevant.
- Use HTTP-local DTOs for wire-format concerns rather than changing internal C++ domain types.
- Treat route ordering as part of the contract; exact/static paths must be registered before parameterized catch-all paths.
- Review concurrency and lifetime risks explicitly, but avoid adding shared state for these stateless chessboard endpoints.

Recent commit history confirms the branch is focused on documentation and API prerequisites (`docs: add web frontend API prerequisites`) after the Epic 4c/4b work. Keep this story narrowly scoped to the two chessboard endpoints, their tests, and OpenAPI.

### References

- [Source: `_bmad-output/planning-artifacts/epics.md` — Epic 4d and Story 4d.1]
- [Source: `_bmad-output/planning-artifacts/prd.md` — Phase 1 course correction, FR21, NFR05, NFR17, NFR19]
- [Source: `_bmad-output/planning-artifacts/architecture.md` — Local HTTP/SSE adapter and frontend rendering feasibility constraints]
- [Source: `CONVENTIONS.md` — fmt, DuckDB C API restriction, naming, module boundaries, testing checklist]
- [Source: `source/motif/http/server.cpp` — existing route handlers, DTOs, JSON error helper, route ordering]
- [Source: `test/source/motif_http/http_server_test.cpp` — HTTP integration test patterns]
- [Source: `docs/api/openapi.yaml` — current OpenAPI route and schema organization]
- [Source: `chesslib` headers from the Nix dependency — `board.hpp`, `move_generator.hpp`, `fen.hpp`, `san.hpp`, `uci.hpp`]

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-5

### Debug Log References

- Fixed check-constrained FEN to include both kings: `4q3/8/8/8/8/8/P7/4K2k w - - 0 1`
- Fixed apply-move expected FEN to include en passant square: chesslib correctly includes `e3` after `e2e4`
- Added `#include <chesslib/core/types.hpp>` to satisfy include-cleaner for `chesslib::move`
- Renamed parameter `m` → `mov` and loop variable `m` → `mov` to satisfy identifier-length linter
- Extracted named constants `uci_promotion_length` and `uci_promotion_index` to replace magic number `5`/`4`
- Renamed lambda parameters `mv` → `move_entry` throughout test file for identifier-length compliance
- Added `// NOLINTNEXTLINE(readability-function-cognitive-complexity)` suppressions for assertion-heavy test cases
- Updated OpenAPI example FEN for apply-move response to match actual chesslib output (with en passant square)
- Code review follow-up: checked local chesslib FEN semantics, added strict HTTP UCI syntax validation, strengthened castling SAN and promotion FEN assertions, and reran full dev + sanitize gates

### Completion Notes List

- All 6 tasks completed. DTOs and routes were pre-authored in `server.cpp`; tests were pre-authored in `http_server_test.cpp`; OpenAPI was pre-authored. This session: resolved all clang-tidy/cppcheck warnings introduced by new code, fixed two test correctness bugs (missing black king in check FEN, wrong expected FEN after e2e4), and ran full dev + sanitize validation.
- 50/50 server tests pass under both dev and ASan/UBSan builds. Zero new warnings from project source files.
- Code review findings resolved. `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev --output-on-failure` passed. `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize --output-on-failure` passed.

### File List

- `source/motif/http/server.cpp`
- `test/source/motif_http/http_server_test.cpp`
- `docs/api/openapi.yaml`

### Change Log

- Implemented `GET /api/positions/legal-moves` and `POST /api/positions/apply-move` endpoints with DTOs, helpers, and route registration (2026-04-26)
- Added full integration test suite for both endpoints (9 test cases, ports 18120-18128) (2026-04-26)
- Updated OpenAPI with `LegalMove`, `LegalMovesResponse`, `ApplyMoveRequest`, `ApplyMoveResponse` schemas and examples (2026-04-26)
- Fixed linter warnings: renamed identifiers, added header include, extracted named constants (2026-04-26)
- Fixed test correctness: check-constrained FEN now includes black king; apply-move expected FEN includes en passant square (2026-04-26)
- Resolved code review findings and marked story done after full dev and sanitize validation (2026-04-26)

