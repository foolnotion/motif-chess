# Story 4b.7: OpenAPI Specification

Status: done

## Story

As a developer integrating with the motif-chess HTTP API,
I want a versioned OpenAPI 3.1 specification document,
So that I have a machine-readable and human-readable contract for all endpoints that serves as Epic 4b's exit artifact.

## Acceptance Criteria

1. **Given** all six Epic 4b endpoints are implemented
   **When** `docs/api/openapi.yaml` is loaded by any OpenAPI 3.1-compliant tool (Swagger UI, Redoc, openapi-generator)
   **Then** it validates without errors and documents all 8 routes:
   - `GET /health`
   - `GET /api/positions/{zobrist_hash}`
   - `GET /api/openings/{zobrist_hash}/stats`
   - `POST /api/imports`
   - `GET /api/imports/{import_id}/progress`
   - `DELETE /api/imports/{import_id}`
   - `GET /api/games`
   - `GET /api/games/{id}`

2. **Given** each documented route
   **When** the spec is reviewed
   **Then** every route includes: HTTP method, path, summary, all path/query parameters with types and constraints, request body schema (where applicable), all documented response codes with JSON schemas, and at least one example response

3. **Given** all reusable shapes (e.g. error body, player, game list entry, game full response)
   **When** the spec is reviewed
   **Then** they are declared as named schemas under `components/schemas` and referenced via `$ref` — no inline duplication

4. **Given** the spec is committed
   **When** `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev` is run
   **Then** all existing tests continue to pass

## Tasks / Subtasks

- [x] Task 1: Create `docs/api/` directory and `openapi.yaml` (AC: 1, 2, 3)
  - [x] Set `openapi: 3.1.0`, `info.title: motif-chess API`, `info.version: 0.1.0`
  - [x] Add server entry `url: http://localhost:8080` with description "local dev"
  - [x] Document `GET /health` → 200 `{"status":"ok"}`
  - [x] Document `GET /api/positions/{zobrist_hash}` with path param (uint64 string), query params `offset`/`limit`, 200 array + 400
  - [x] Document `GET /api/openings/{zobrist_hash}/stats` with path param, 200 opening-stats response + 400
  - [x] Document `POST /api/imports` with request body `{"path": string}`, 202 `{"import_id": string}`, 400, 409
  - [x] Document `GET /api/imports/{import_id}/progress` as SSE stream (text/event-stream), events schema in description
  - [x] Document `DELETE /api/imports/{import_id}` → 200, 404
  - [x] Document `GET /api/games` with query params `player`/`result`/`offset`/`limit`, 200 paginated array
  - [x] Document `GET /api/games/{id}` with path param (uint32), 200 full game response, 400, 404
  - [x] Declare all reusable schemas under `components/schemas` (AC: 3)

- [x] Task 2: Build and test validation (AC: 4)
  - [x] Run `cmake --preset=dev && cmake --build build/dev`
  - [x] Run `ctest --test-dir build/dev`
  - [x] Record pass/fail in Dev Agent Record

## Dev Notes

### Output File

`docs/api/openapi.yaml` — create the `docs/api/` directory if it does not exist. Do not create any other files. No code changes required; this is a documentation-only story.

### OpenAPI Version

Use **OpenAPI 3.1.0**. Key differences from 3.0.x relevant here:
- `type` can be an array (e.g. `type: [integer, "null"]`) — prefer this over the 3.0 `nullable: true`
- `examples` (plural) replaces `example` for parameter objects in 3.1

Validate the spec locally with `npx @redocly/cli lint docs/api/openapi.yaml` if available; otherwise note in the Dev Agent Record that linting was skipped and why.

### Endpoint Contracts (derive from server.cpp)

All route registrations are in `source/motif/http/server.cpp`. Key behavioral facts:

**`GET /health`**
- Returns `200 {"status":"ok"}`
- No parameters

**`GET /api/positions/{zobrist_hash}`**
- `zobrist_hash`: uint64 as decimal string; rejects non-numeric, negative, empty → 400 `{"error":"invalid zobrist hash"}`
- Query: `offset` (integer ≥ 0, default 0), `limit` (integer 1–200, default 50)
- 200: array of `{ game_id, ply, result, white_elo, black_elo }`

**`GET /api/openings/{zobrist_hash}/stats`**
- Same hash validation → 400
- 200: `{ continuations: [ { move, frequency, white_wins, draws, black_wins, avg_white_elo, avg_black_elo } ] }`

**`POST /api/imports`**
- Body: `{ "path": "/absolute/path/to/file.pgn" }` (string, required)
- 202: `{ "import_id": "<uuid>" }`
- 400: file not found / unreadable
- 409: import already in progress

**`GET /api/imports/{import_id}/progress`**
- Response: `text/event-stream`
- Progress events: `data: { games_processed, games_committed, games_skipped, elapsed_seconds }`
- Final event: `data: { done: true, summary: { total, committed, skipped, errors } }`
- 404 if import_id unknown (document in spec; actual server behavior may return event-stream with error event)

**`DELETE /api/imports/{import_id}`**
- Signals graceful stop; checkpoint preserved
- 200 on success, 404 if not found

**`GET /api/games`**
- Query: `player` (string, optional), `result` (enum: `1-0`, `0-1`, `1/2-1/2`, optional), `offset` (int, default 0), `limit` (int 1–200, default 50)
- 200: array of `{ id, white, black, result, event, date, eco }`
- `white`/`black` are strings (player names) at the list level

**`GET /api/games/{id}`**
- `id`: uint32 (1–4294967295); rejects empty, non-numeric, zero, overflow → 400 `{"error":"invalid game id"}`
- 200: full game object (see DTO schema below)
- 404: `{"error":"not_found"}`

### Reusable Schemas to Define under `components/schemas`

```
ErrorResponse:
  type: object
  required: [error]
  properties:
    error: { type: string }

PositionHit:
  type: object
  properties:
    game_id: { type: integer }
    ply:     { type: integer }
    result:  { type: string }
    white_elo: { type: [integer, "null"] }
    black_elo: { type: [integer, "null"] }

OpeningContinuation:
  type: object
  properties:
    move:          { type: string, description: "SAN notation" }
    frequency:     { type: integer }
    white_wins:    { type: integer }
    draws:         { type: integer }
    black_wins:    { type: integer }
    avg_white_elo: { type: [number, "null"] }
    avg_black_elo: { type: [number, "null"] }

GameListEntry:
  type: object
  properties:
    id:     { type: integer }
    white:  { type: string }
    black:  { type: string }
    result: { type: string }
    event:  { type: [string, "null"] }
    date:   { type: [string, "null"] }
    eco:    { type: [string, "null"] }

GamePlayer:
  type: object
  required: [name]
  properties:
    name:    { type: string }
    elo:     { type: [integer, "null"] }
    title:   { type: [string, "null"] }
    country: { type: [string, "null"] }

GameEvent:
  type: object
  required: [name]
  properties:
    name: { type: string }
    site: { type: [string, "null"] }
    date: { type: [string, "null"] }

GameTag:
  type: object
  required: [key, value]
  properties:
    key:   { type: string }
    value: { type: string }

GameFull:
  type: object
  required: [id, white, black, result, moves]
  properties:
    id:     { type: integer }
    white:  { $ref: '#/components/schemas/GamePlayer' }
    black:  { $ref: '#/components/schemas/GamePlayer' }
    event:  { $ref: '#/components/schemas/GameEvent', nullable: true }
    date:   { type: [string, "null"] }
    result: { type: string }
    eco:    { type: [string, "null"] }
    tags:   { type: array, items: { $ref: '#/components/schemas/GameTag' } }
    moves:  { type: array, items: { type: integer }, description: "16-bit encoded moves" }

ImportStartResponse:
  type: object
  required: [import_id]
  properties:
    import_id: { type: string, format: uuid }
```

### SSE Endpoint Note

OpenAPI 3.1 has limited native support for Server-Sent Events. Document `GET /api/imports/{import_id}/progress` with:
- `responses['200'].content['text/event-stream']`
- `schema.type: string` (raw stream)
- A detailed `description` field explaining the event format (progress events and final done event)

Do not attempt to model the SSE wire format as a structured schema — it will not validate correctly and is not idiomatic OpenAPI.

### Conventions Compliance

- No new source files under `source/`; no changes to `CMakeLists.txt`, `flake.nix`, or `vcpkg.json`
- YAML indentation: 2 spaces throughout
- All `$ref` paths use `'#/components/schemas/...'` (single-file spec; no `$ref` to external files)
- Keep descriptions concise — one sentence per endpoint summary, one sentence per parameter

### Previous Story Intelligence

- The 4b.6 DTO shapes (`game_response`, `game_player_response`, `game_event_response`, `game_tag_response`) are defined in `source/motif/http/server.cpp` in the `motif::http::detail` namespace. Map them 1:1 to the `GameFull` / `GamePlayer` / `GameEvent` / `GameTag` schemas above.
- The list endpoint (`GET /api/games`) returns player names as plain strings (not player objects) — confirmed in 4b.5. Do not promote them to `GamePlayer` objects in the spec.
- The Epic 3 retro established that "OpenAPI spec is built incrementally per endpoint story, consolidated as the epic's exit artifact." This story is the consolidation pass.

### References

- [Source: `_bmad-output/planning-artifacts/epics.md` — Story 4b.7]
- [Source: `_bmad-output/implementation-artifacts/epic-3-retro-2026-04-24.md` — team agreement and deliverable definition]
- [Source: `source/motif/http/server.cpp` — authoritative route registrations and response shapes]
- [Source: `_bmad-output/implementation-artifacts/4b-6-single-game-retrieval-endpoint.md` — GameFull DTO]
- [Source: `_bmad-output/implementation-artifacts/4b-5-game-list-endpoint.md` — GameListEntry shape]
- [Source: `_bmad-output/implementation-artifacts/4b-4-import-trigger-sse-progress-stream.md` — import/SSE contracts]

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

- Redocly lint iteration 1: 8 errors (security-defined), 11 warnings (operationId missing). Fixed by adding `security: []` at root and `operationId` to every operation.
- Redocly lint iteration 2: 0 errors, 3 warnings (info-license, no-server-example.com, operation-4xx-response for /health). All three are expected and appropriate for this API.
- Redocly lint after code review patches: 0 errors, 3 warnings (same expected warnings as iteration 2).
- Key schema deviation from Dev Notes: `PositionHit.result` serializes as integer (C++ `int8_t`) not string; `OpeningContinuation` uses actual C++ field names (`san`, `result_hash`, `average_white_elo`, `average_black_elo`, `eco`, `opening_name`) derived from server.cpp.
- Build: `cmake --preset=dev && cmake --build build/dev` — exit 0.
- Tests: `ctest --test-dir build/dev` — 167/167 passed, 0 failed (5 performance tests skipped).

### Completion Notes List

- Created `docs/api/openapi.yaml` — OpenAPI 3.1.0 spec for all 8 routes across 6 endpoints of Epic 4b.
- All reusable schemas declared under `components/schemas`: ErrorResponse, PositionHit, OpeningStats, OpeningContinuation, GameListEntry, GamePlayer, GameEvent, GameTag, GameFull, ImportStartResponse.
- Every route includes: operationId, summary, all parameters with types/constraints, response codes with JSON schemas, and at least one example.
- SSE endpoint documented with text/event-stream content type and detailed description of progress/complete/error event formats.
- Spec validates clean with `@redocly/cli` (0 errors, 3 acceptable warnings).
- No source code changes; no CMakeLists.txt, flake.nix, or vcpkg.json changes. Documentation-only story.

### Change Log

- 2026-04-26: Created `docs/api/openapi.yaml` — OpenAPI 3.1.0 specification for all Epic 4b endpoints (Story 4b.7)

### File List

- `_bmad-output/implementation-artifacts/4b-7-openapi-spec.md`
- `docs/api/openapi.yaml`

### Review Findings

- [x] [Review][Patch] `zobrist_hash` parameters lack uint64 upper-bound constraints [docs/api/openapi.yaml:41] — Fixed: added maxLength and an explicit uint64 valid-range/overflow note to both Zobrist path parameters.
- [x] [Review][Patch] `import_id` values are unconstrained despite a fixed wire format [docs/api/openapi.yaml:200] — Fixed: constrained import IDs to 32 lowercase hex characters on both path parameters and `ImportStartResponse.import_id`.
- [x] [Review][Patch] SSE examples use literal `\n` sequences [docs/api/openapi.yaml:234] — Fixed: converted multi-line SSE examples to YAML block scalars with real line breaks.
- [x] [Review][Patch] `POST /api/imports` omits the invalid JSON body error example [docs/api/openapi.yaml:170] — Fixed: broadened the 400 description and added an `invalid_request_body` example.
- [x] [Review][Patch] Fixed status responses are under-constrained [docs/api/openapi.yaml:431] — Fixed: added enum constraints for fixed status values and required `CancelResponse.status`.
- [x] [Review][Decision] Nullable schemas don't match actual serialization — Resolved: removed `'null'` variants from all optional field types. Fields are now plain types (e.g. `type: integer`) and documented as "omitted when not recorded". Non-required fields represent optionality via key-absence. [blind+edge]
- [x] [Review][Decision] result_hash uint64 JSON precision loss — Resolved: added description note warning about values exceeding 2^53 and that future versions may serialize as string. [edge]
- [x] [Review][Decision] game_list_entry event/date/eco empty string vs nullable — Resolved: added description notes that these may be empty strings when unrecorded. [edge]
- [x] [Review][Patch] Missing `required` arrays on PositionHit, OpeningContinuation, GameListEntry [docs/api/openapi.yaml] — Fixed: added required arrays matching non-optional C++ fields. [blind+edge+auditor]
- [x] [Review][Patch] SSE endpoint lacks formal `examples` key [docs/api/openapi.yaml] — Fixed: added progress_event, complete_event, and error_event examples under text/event-stream. [auditor]
- [x] [Review][Patch] Inline schemas on /health and DELETE /api/imports/{import_id} not in components [docs/api/openapi.yaml] — Fixed: extracted to HealthResponse and CancelResponse component schemas. [auditor]
- [x] [Review][Defer] Missing 500 responses on multiple endpoints — server.cpp can return 500 on internal errors (search failed, stats query failed, game retrieval failed). Internal errors are generally not spec'd in OpenAPI. [edge]
- [x] [Review][Defer] result type inconsistency across endpoints — PositionHit.result is integer (int8_t encoded), GameListEntry/GameFull.result is string ("1-0", etc.). Pre-existing C++ design accurately reflected by the spec. [blind]
- [x] [Review][Defer] result enum/int8_t no constraint — PositionHit.result and game result query param lack enum constraints. Pre-existing C++ types allow arbitrary values. [blind+edge]
- [x] [Review][Defer] moves array lacks 16-bit range constraint — Nice-to-have but not caused by this change. [blind]
- [x] [Review][Defer] anyOf vs type array style inconsistency — GameFull.event uses `anyOf`, other nullable fields use `type: ['X', 'null']`. Cosmetic. [blind]
- [x] [Review][Defer] searchPositions operationId naming subjective — Not a bug. [blind]
- [x] [Review][Defer] GameFull.date duplicates GameEvent.date — Pre-existing C++ struct design accurately reflected. [blind]
- [x] [Review][Defer] Bare array responses — Pre-existing API design. [blind]
- [x] [Review][Defer] Cancel on completed import returns 200 — Server behavior issue, spec accurately documents what the server returns. [edge]
- [x] [Review][Defer] SSE reconnect replays final event — Server behavior, not a spec accuracy issue. [edge]
- [x] [Review][Defer] result query param not validated server-side — Server issue, spec documents intended enum. [edge]
- [x] [Review][Defer] import_id no format/pattern — Opaque by design, 32-char random hex. [blind+edge]
