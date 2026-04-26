# Epic 4b Retrospective — HTTP API Layer

**Date:** 2026-04-26
**Facilitator:** Amelia (Developer)
**Participants:** Alice (Product Owner), Charlie (Senior Dev), Dana (QA Engineer), Winston (Architect), Paige (Technical Writer), Bogdan (Project Lead)

---

## Epic Summary

| Metric | Value |
|---|---|
| Stories delivered | 7/7 (100%) |
| API routes documented | 8/8 |
| Exit artifact | `docs/api/openapi.yaml` |
| Tests at close | 167 passing, 5 performance tests skipped in dev gate |
| OpenAPI lint | 0 errors, 3 accepted warnings |
| Primary residual risk | HTTP import/SSE lifecycle and API wire-format consistency |

**Stories completed:**
- 4b-1: HTTP Server Scaffold
- 4b-2: Position Search Endpoint
- 4b-3: Opening Statistics Endpoint
- 4b-4: Import Trigger & SSE Progress Stream
- 4b-5: Game List Endpoint
- 4b-6: Single Game Retrieval Endpoint
- 4b-7: OpenAPI Specification

---

## What Went Well

1. **Code review materially improved quality.** Review found issues that would have become accepted behavior if left unchecked: pagination semantics, invalid input coverage, OpenAPI contract drift, import cancellation, SSE response fields, worker ownership, and serialization errors.

2. **The HTTP layer stayed mostly thin.** Epic 4b introduced `motif_http` as an adapter over existing `motif_db`, `motif_search`, and `motif_import` APIs without turning HTTP into a second business-logic layer.

3. **Story-to-story learning improved execution.** Pagination lessons from 4b-2 carried into 4b-5. Glaze reflection lessons from 4b-3 carried into 4b-6 and 4b-7. Database mutex and route patterns also stabilized over the epic.

4. **OpenAPI became a real exit artifact.** The final spec reflects actual server behavior, uses reusable schemas, documents SSE pragmatically, and records known contract tradeoffs rather than hiding them.

5. **Validation discipline held.** Story records show dev and sanitizer gates passing where relevant, with performance checks handled using the established release/dev/sanitizer split.

---

## What Didn't Go Well

### 1. Hash wire-format friction remained unresolved

The API currently exposes some 64-bit values as JSON integers. That is faithful to C++ `std::uint64_t`, but unsafe for common JSON clients that use IEEE-754 numbers and cannot represent all 64-bit integers exactly.

**Resolution:** All externally exposed hashes should serialize as JSON strings, regardless of magnitude. Internal C++ storage and APIs should remain `std::uint64_t`; only the HTTP wire contract changes.

### 2. Formatting paths are inconsistent

The project already standardizes on `fmt`, but some code still uses iostream-style output or ad hoc formatting. Hash string serialization makes consistency more important because the wire representation must be exact and uniform.

**Resolution:** New code should use `fmt::format` for string construction and `fmt::print` for console output. Avoid `std::cout`, `std::cerr`, `std::ostringstream`, and `std::to_string` unless a library API specifically requires iostreams.

### 3. Import/SSE produced the densest residual risk cluster

Story 4b-4 delivered the important feature, but later reviews repeatedly deferred issues around long-lived import sessions, `jthread` construction failure, session cleanup, SSE escaping, blocking sleeps in the content provider, and thread-visibility assumptions.

**Resolution:** Treat import/SSE lifecycle cleanup as explicit follow-up work, not background noise in unrelated UI stories.

### 4. OpenAPI was consolidated at the end

Epic 3's agreement was to build the OpenAPI spec incrementally per endpoint story. Epic 4b captured contracts in story notes and consolidated the actual spec in 4b-7. This worked, but it left more reconciliation work for the final story.

**Resolution:** Future API stories should update the OpenAPI document alongside implementation, with a final review pass only for consistency.

---

## Key Technical Discoveries

### API boundaries need frontend-language compatibility

C++ type fidelity is not enough for a stable HTTP contract. A field that is correct as `std::uint64_t` can still be unsafe as a JSON number. OpenAPI should describe the safe wire contract, not merely mirror the internal type.

### `fmt` should own formatting and console output

Using `fmt` everywhere reduces representation drift. Console output can be handled with `fmt::print(stdout, ...)` and `fmt::print(stderr, ...)`; stream APIs should not be used as a general formatting mechanism.

### Review should remain mandatory for every story

Epic 4b was straightforward in implementation but review-heavy in quality improvement. The team should keep adversarial review as a required story-cycle step, especially for API contracts and concurrency-sensitive code.

---

## Epic 3 Action Item Follow-Through

| # | Epic 3 Action | Status | Evidence |
|---|---|---|---|
| 1 | Adopt PR-based workflow | Partial | Story records continued to capture review findings, but workflow evidence is mixed. |
| 2 | Commit per logical unit | Improved | Later stories separated implementation and review fixes more clearly, though this remains a team habit to maintain. |
| 3 | Limit story scope to stated purpose | Improved | Most Epic 4b stories stayed thin; 4b-4 naturally carried more concurrency scope because import/SSE required lifecycle work. |
| 4 | DuckDB prepared statements for position_store queries | Not addressed | Still present in deferred work; not central to HTTP story delivery. |
| 5 | Import config validation | Not addressed | Still deferred from earlier import work. |
| 6 | Skip position_rows when write_positions=false | Not addressed | Still deferred. |
| 7 | Bundle validation for missing `positions.duckdb` | Not addressed | Still deferred. |
| 8 | Triage deferred items before Epic 4b | Partial | Some items were noted and carried forward; deferred list still grew. |
| 9 | Document `dominant_eco` tie-break rule | Not verified | Did not appear as a completed Epic 4b action. |
| 10 | Codebase consolidation pass | Not addressed | No dedicated consolidation pass in Epic 4b. |

---

## Deferred Technical Items

### Promote before HTTP-consuming frontend work

- Serialize all externally exposed hashes as JSON strings, including `result_hash` and any future hash fields.
- Update HTTP DTO conversion, OpenAPI schemas/examples, and tests together.
- Use `fmt` for all hash-to-string conversion and all new formatting/console output.

### Import/SSE lifecycle cleanup candidates

- Handle `std::jthread` construction failure without leaving an orphaned active import session.
- Remove or use dead `import_session::pgn_path` and `cancel_requested` fields.
- JSON-escape SSE error event payloads.
- Avoid blocking httplib worker threads with `sleep_for` inside the SSE content provider if concurrent SSE usage becomes important.
- Document or strengthen thread-visibility guarantees for `summary`, `error_message`, `done`, and `failed`.
- Prune completed `import_workers` and completed `sessions` to avoid unbounded growth.
- Reorder import conflict checks before expensive pipeline/session construction.

---

## Action Items

| # | Action | Owner | Priority | Success Criteria |
|---|---|---|---|---|
| 1 | Change the HTTP wire contract so all externally exposed hashes serialize as strings | Developer agent | High | OpenAPI, DTOs, examples, and tests agree; both small and large hashes serialize as JSON strings. |
| 2 | Adopt fmt-only formatting for new code | All | High | New string construction uses `fmt::format`; new console output uses `fmt::print`; iostream formatting is avoided. |
| 3 | Promote repeated import/SSE lifecycle concerns into explicit cleanup work | Bogdan + Developer agent | High | Cleanup story or deferred-work entries are grouped with owners and acceptance criteria before frontend work depends on them. |
| 4 | Keep code review mandatory after each implementation story | Bogdan + reviewer agent | High | Every story reaches done only after review findings are patched, deferred with rationale, or explicitly accepted. |
| 5 | Update OpenAPI incrementally for future API changes | Developer agent + Paige | Medium | API stories change implementation, tests, and OpenAPI in the same story unless explicitly out of scope. |

---

## Next Epic Preparation

The backlog points back to **Epic 4: Desktop Application**, which was deferred while Epic 4b took priority.

Before starting Epic 4 stories, decide whether the Qt desktop app should consume:

- direct C++ backend APIs through Qt worker threads, matching the original architecture;
- the local HTTP API as a frontend boundary;
- or a hybrid where HTTP remains an integration surface and Qt uses direct C++ APIs.

The hash-string wire contract only blocks HTTP-consuming frontend work. It does not require changing internal C++ storage or search APIs.

---

## Readiness Assessment

Epic 4b is complete from a story and acceptance-criteria perspective. The HTTP API layer delivered the intended integration surface and exit OpenAPI artifact.

The epic is ready to close with two important carry-forwards:

1. Fix the hash wire contract before treating HTTP as a stable frontend-consumed API.
2. Triage import/SSE lifecycle risks before building UI workflows that rely heavily on HTTP import progress.

---

## Team Agreements

- All externally exposed hashes serialize as JSON strings, regardless of value.
- Internal C++ APIs keep `std::uint64_t` unless there is a direct reason to change them.
- `fmt` is the default and expected mechanism for string formatting and console output in new code.
- `std::cout`, `std::cerr`, `std::ostringstream`, and `std::to_string` are avoided in new code except when a library API specifically requires iostreams.
- Code review remains a required part of the story lifecycle.

---

## Significance Detection

No discovery invalidates the overall Epic 4b approach. The HTTP adapter strategy remains sound.

One contract update is required before HTTP becomes a stable frontend boundary: hash values exposed over JSON should be strings, not numbers.

---

*Fourth retrospective for this project. Epic 4b delivered the HTTP API layer and OpenAPI contract; next work should either prepare the desktop app path or harden the HTTP contract first.*
