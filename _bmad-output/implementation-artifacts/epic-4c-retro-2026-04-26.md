# Epic 4c Retrospective — HTTP API Contract Hardening

**Date:** 2026-04-26
**Facilitator:** Amelia (Developer)
**Participants:** Alice (Product Owner), Charlie (Senior Dev), Dana (QA Engineer), Winston (Architect), Bogdan (Project Lead)

---

## Epic Summary

| Metric | Value |
|---|---|
| Stories completed | 1/1 (100%) |
| Story | 4c-1: HTTP API Contract Hardening |
| Agent model | GPT-5.5 |
| Review findings patched | 2 (`[[nodiscard]]` on converters; eco/opening_name test coverage) |
| Review findings deferred | 5 (all pre-existing or micro-optimizations) |
| Build gate | ✅ Passed |
| Sanitizer gate | ✅ Passed |
| Production incidents | 0 |
| Technical debt incurred | 5 deferred items (minor, all pre-existing) |

**Stories completed:**
- 4c-1: HTTP API Contract Hardening

---

## What Went Well

1. **Tightest scope of any epic so far.** One story, one clear boundary (HTTP wire contract only — no internal type changes, no Qt work). The dev notes explicitly called out what was out of scope and the boundary held throughout.

2. **Epic 4b action items closed cleanly.** Items 1 (hash strings), 2 (fmt-only), 4 (review mandatory), and 5 (OpenAPI updated in-story) were all delivered directly by 4c-1. The retro-to-execution feedback loop worked as intended.

3. **Review found real issues and patched them.** `[[nodiscard]]` missing on converters and the eco/opening_name coverage gap were caught and fixed — not deferred. That is the standard.

4. **Both build and sanitizer gates passed.** Zero regressions, zero ASan/UBSan violations.

5. **OpenAPI contract updated in the same story.** Schema, examples, and stale warnings were all addressed together — consistent with the team agreement from Epic 4b.

---

## What Didn't Go Well

### 1. Import/SSE lifecycle debt persists — now two retros old

Action item 3 from Epic 4b — promote import/SSE lifecycle risks into explicit cleanup work — was correctly out of 4c scope, but it has no owner and no story. The web frontend sandbox will exercise SSE directly and will surface these issues as visible failures.

**Critical items identified:**
- `jthread` constructor can throw `std::system_error` → orphaned session permanently blocks all future imports
- SSE error event embeds `error_message` without JSON escaping — malformed JSON to browser on errors containing `"`, `\`, or newline
- `error_message`/`summary` read by SSE callback with undocumented ordering — one refactor from a data race
- `import_workers` and `sessions` grow without bound — never pruned; web frontends generate far more sessions than CLI tools
- SSE content provider calls `sleep_for(250ms)` on httplib's thread — blocks thread pool slots; concurrent browser tabs will exhaust it

**Lower severity items also to be addressed in the same story:**
- `cancel_requested` atomic set by DELETE handler but never read — dead state
- Conflict check runs after session/pipeline construction — wasted resources on every 409
- `session->pipeline` null guard missing in SSE handler
- Destructor joins jthreads after releasing `sessions_mutex` — workers may still run when destructor returns
- SSE chunk provider loops forever if worker deadlocks before `done.store`
- `memory_order_relaxed` for `failed` flag — relies on implicit ordering through `done`; document or upgrade

**Resolution:** Story 4c-2 (Import/SSE Lifecycle Hardening) — one comprehensive story covering all items. This is a critical path blocker before web frontend work starts.

### 2. Epic status not updated on completion

`epic-4c` remained `in-progress` in sprint-status after 4c-1 was marked done. Minor hygiene issue; corrected in this retrospective.

---

## Epic 4b Action Item Follow-Through

| # | Epic 4b Action | Status | Evidence |
|---|---|---|---|
| 1 | Hash wire contract → JSON strings | ✅ Done | 4c-1 primary deliverable |
| 2 | fmt-only formatting for new code | ✅ Done | Task 4: `fmt::print`/`fmt::format` throughout |
| 3 | Import/SSE lifecycle cleanup story | ❌ Not addressed | Correctly out of 4c scope; becomes 4c-2 |
| 4 | Code review mandatory every story | ✅ Done | Review ran, findings patched or deferred with rationale |
| 5 | OpenAPI updated incrementally | ✅ Done | Schema + examples updated in same story |

---

## Strategic Discussions

### Product direction: opening preparation tool for personal use

Clarified during this retro: the product is an **opening preparation tool with advanced analysis and statistics, geared towards personal preparation**. This is not a generic game database browser.

Implication: when a user imports their own games, the current API already yields personal win/draw/loss rates per continuation (the corpus is personal). Epic 6 adds slicing dimensions not currently available: breakdown by which side the user played, opponent rating band, game phase, and time management correlation.

### Web frontend as sandbox

The immediate next step is a separate web frontend that will serve as a testing sandbox for the HTTP API, not the deferred Qt desktop application. This is the right sequencing — it validates the API contract (hardened in 4c) before committing to a desktop shell.

### Epic 6 scope and architecture

Epic 6 (Statistical Analysis) requirements extend beyond a statistics panel. The personal preparation use case implies player-perspective filtering across multiple query types:
- Position search filtered by player name + color played
- Opening stats broken down by which side the user played
- Performance aggregates by opening, color, opponent rating band, and game phase

**Team agreement:** Epic 6 should be left architecture-open. Do not precommit its design to the current HTTP REST API pattern. The integration approach should be decided when Epic 6 is designed.

---

## Action Items

| # | Action | Owner | Priority | Success Criteria |
|---|---|---|---|---|
| 1 | Create story 4c-2: Import/SSE Lifecycle Hardening | Bogdan | High — blocks web frontend | Story file created; all identified items in scope; story done before web frontend work starts |
| 2 | Update `epic-4c` status to `done` in sprint-status.yaml | Bogdan | Low | sprint-status reflects actual completion state |
| 3 | When designing Epic 6, treat as architecture-open — no premature coupling to current HTTP API pattern | Bogdan (planning) | Medium | Epic 6 story creation does not assume REST HTTP delivery |

---

## Critical Path Before Web Frontend

1. ✅ HTTP wire contract stable (hash strings, OpenAPI clean) — done in 4c-1
2. ❌ **Story 4c-2: Import/SSE Lifecycle Hardening** — must complete before web frontend starts

---

## Readiness Assessment

| Area | Status |
|---|---|
| Hash wire contract | ✅ Stable — all hashes serialized as JSON strings |
| OpenAPI spec | ✅ Clean — matches implementation, 0 errors |
| fmt/formatting | ✅ Consistent across HTTP layer |
| Import/SSE lifecycle | ❌ Known risks — story 4c-2 required before web frontend |

Epic 4c is complete from a story and acceptance-criteria perspective. The API contract is stable and frontend-safe. One story (4c-2) is required to close the import/SSE lifecycle risks before the web frontend sandbox depends on the import/SSE endpoints.

---

## Team Agreements

- Story 4c-2 is a prerequisite for web frontend work — no frontend work starts until it is done.
- Epic 6 design is architecture-open; do not assume REST HTTP delivery.
- Personal stats use case is core to the product; the current API serves it partially (corpus-level aggregates); Epic 6 adds player-perspective filtering.

---

*Fifth retrospective for this project. Epic 4c closed the hash wire contract and fmt consistency gaps from Epic 4b. Epic 4c-2 (Import/SSE Lifecycle Hardening) is the final gate before web frontend sandbox work begins.*
