# Sprint Change Proposal — 2026-04-26

## 1. Issue Summary

Web frontend design revealed two backend API prerequisites that are not covered by the current HTTP API contract:

- Interactive board drag-and-drop needs legal move generation from an arbitrary FEN so the UI can highlight legal destinations before a move is dropped, plus backend move validation/application so a logic-free board can update only after the API confirms the move and returns the resulting FEN.
- The engine analysis panel cannot talk directly to a local UCI engine from the browser, so the backend must proxy engine lifecycle and stream analysis output.

This is not a failure of the completed Epic 4b/4c work. Those epics correctly exposed the database, import, game, and opening-statistics surfaces. The new gap appears because the frontend is now moving from read-only database browsing into interactive board and engine workflows.

## 2. Impact Analysis

### Epic Impact

- **Epic 4b / 4c:** Completed work remains valid. The API is frontend-safe for existing database/search/import routes, but not complete for interactive board and engine panels.
- **Epic 4:** Board and navigation stories gain a backend dependency if implemented through the local HTTP API or web frontend. Legal moves and move validation/application must be available before drag/drop board UX is treated as ready.
- **Epic 5:** Engine integration must become backend-first. The current stories are desktop-oriented and mention Qt signals directly; web usage requires HTTP/SSE start, stream, and stop contracts over `motif_engine`.

### Artifact Conflicts

- **PRD:** The local web frontend/API sandbox needs to be acknowledged without redefining the product as a hosted web application.
- **Epics:** A new API-prerequisite epic is needed between 4c and UI-dependent work.
- **Architecture:** The HTTP adapter must be documented as a local frontend boundary, and engine streaming must preserve `ucilib` ownership.
- **OpenAPI:** Future stories must update `docs/api/openapi.yaml` for legal moves and engine analysis endpoints.

### Technical Impact

- Legal move generation and move application must use `chesslib` exclusively. Motif-chess must not reimplement move legality, SAN, or FEN parsing.
- Engine lifecycle must use `ucilib` exclusively. The HTTP layer manages sessions and streaming, not raw engine subprocess details.
- SSE is preferred over WebSocket for the first engine API because analysis output is primarily server-to-client and the backend already has hardened SSE lifecycle patterns.
- The frontend rendering model is feasible if the board library remains display-only: it accepts FEN, renders pieces, handles drag/drop affordances, sends candidate moves to the API, and updates only from the confirmed API response.
- Game-list rendering must use virtual scrolling with paginated API fetching. The current game-list API cap of 200 rows per request is compatible with 1M+ row lists as long as the frontend keeps only the visible window and a small prefetch buffer in memory.
- Engine streaming should run in a Web Worker using SSE/EventSource or equivalent fetch streaming so analysis updates do not block the main rendering thread.
- Local deployment remains `localhost:8080`, no auth, no multi-tenancy, no CDN, and no data leaves the machine.

## 3. Recommended Approach

Use **Direct Adjustment**: add a short Epic 4d for web frontend API prerequisites, then rework Epic 5 as backend-first engine integration.

No rollback is needed. No MVP reset is needed. The change is a moderate backlog reorganization with clear sequencing:

1. Add legal moves and move validation/application endpoints.
2. Define engine analysis HTTP/SSE contract.
3. Implement engine lifecycle and streaming analysis over `motif_engine` + `ucilib`.
4. Let web UI board and engine panels depend on those contracts.

Risk is moderate. Legal moves is low-risk and small. Engine streaming is higher-risk because it combines process lifecycle, cancellation, SSE session management, and SAN conversion of PV lines.

## 4. Detailed Change Proposals

### Epics

Add **Epic 4d: Web Frontend API Prerequisites** after Epic 4c:

- `4d-1-legal-moves-and-move-validation-endpoints`
- `4d-2-engine-analysis-api-contract`

Revise Epic 5 to avoid Qt-first implementation assumptions and make the backend engine manager/API stream the foundation for both desktop and web clients.

### PRD

Clarify that the project is still local-first and offline-first. The web frontend is a local frontend/API consumer, not a hosted web product. Pull forward only the API prerequisites needed for board interaction and engine analysis.

### Architecture

Document `motif_http` as the local HTTP adapter over `motif_db`, `motif_import`, `motif_search`, and `motif_engine`. Preserve module ownership:

- `chesslib`: legal moves, FEN, SAN, Zobrist.
- `ucilib`: UCI engine subprocess lifecycle.
- `motif_http`: request validation, JSON/SSE contract, session lifecycle.

## 5. Implementation Handoff

Scope classification: **Moderate**.

Handoff:

- Product/backlog: accept Epic 4d insertion and sprint-status update.
- Developer: create `4d-1-legal-moves-and-move-validation-endpoints` as the next story when ready.
- Developer/architect: use `4d-2-engine-analysis-api-contract` to settle engine SSE payloads before implementation.

Success criteria:

- `sprint-status.yaml` shows Epic 4d before UI/engine-dependent work.
- `epics.md` contains actionable stories with Given/When/Then acceptance criteria.
- PRD and architecture no longer imply the existing API is sufficient for interactive board/engine panels.
- Future OpenAPI updates define every new endpoint before frontend code depends on it.
