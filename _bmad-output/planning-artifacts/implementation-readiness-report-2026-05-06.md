---
stepsCompleted: ['step-01-document-discovery', 'step-02-prd-analysis', 'step-03-epic-coverage-validation', 'step-04-ux-alignment', 'step-05-epic-quality-review', 'step-06-final-assessment']
documentsUsed:
  prd: '_bmad-output/planning-artifacts/prd-003-search.md'
  architecture: '_bmad-output/planning-artifacts/architecture.md'
  epics: '_bmad-output/planning-artifacts/epics.md'
---

# Implementation Readiness Assessment Report

**Date:** 2026-05-06
**Project:** motif-chess — spec 003 Search & Filtered Opening Explorer

---

## PRD Analysis

### Functional Requirements

**Game Search & List**

- FR01: The user can filter games by player name (case-insensitive partial match) with optional color restriction (White, Black, or either).
- FR02: The user can filter games by ELO range (min and/or max average game Elo, both endpoints inclusive). Games without Elo data are excluded when an Elo filter is active.
- FR03: The user can filter games by result ("1-0", "0-1", "1/2-1/2").
- FR04: The user can filter games by ECO prefix (e.g., "B9" matches B90–B99).
- FR05: Filter fields can be combined arbitrarily; all active filters are ANDed.
- FR06: An empty filter (no fields set) returns all games, paginated.
- FR07: The game list is paginated with configurable limit (default 100, max 500) and offset.
- FR08: The game list returns total_count reflecting the full result set, not just the current page.
- FR09: The game list displays: White player, Black player, Result, White Elo, Black Elo, ECO, Date, Event.
- FR10: Selecting a game in the list navigates the board to that game.

**Filtered Opening Explorer**

- FR11: The opening explorer accepts an optional search_filter and computes all statistics (frequency, white_wins, draws, black_wins, average_white_elo, average_black_elo) over the filtered game set only.
- FR12: When no filter is active, the opening explorer behaves identically to the current implementation.
- FR13: Each continuation exposes an ELO-weighted score (scalar, backend-computed) usable as a sort key in addition to frequency.
- FR14: The user can toggle continuation sort between "by frequency" and "by ELO-weighted score."
- FR15: For each continuation, the backend returns per-bucket ELO distribution data: (avg_elo_bucket, white_wins, draws, black_wins, game_count) at 25-Elo bucket width, covering the full Elo range of matching games (empty buckets included as zeros).
- FR16: The opening tree (opening_tree::open and expand) accept the search filter and pass it through to all underlying queries.

**Sync Toggle**

- FR17: A sync toggle controls whether the game list and opening explorer share a single filter state or maintain independent filters.
- FR18: When sync is ON, any filter change in either panel immediately propagates to the other and triggers a refresh.
- FR19: When sync is OFF, panels refresh only when their own filter changes.
- FR20: The sync toggle defaults to ON at session start.

**HTTP API**

- FR21: GET /api/games accepts query parameters corresponding to search_filter fields (player, color, min_elo, max_elo, result, eco, offset, limit) and returns the filtered game list with total_count.
- FR22: GET /api/positions/{hash}/stats accepts filter query parameters and returns filtered opening statistics including the ELO-weighted score and ELO distribution buckets per continuation.
- FR23: GET /api/positions/{hash}/tree accepts filter query parameters and returns a filtered opening tree up to prefetch_depth.

**Total FRs: 23**

---

### Non-Functional Requirements

**Performance**

- NFR01: Filtered game list query (any filter combination): < 100ms for a 4M-game corpus.
- NFR02: Filtered opening stats at any position (including ELO distribution buckets): < 200ms for a 4M-game corpus.
- NFR03: Filtered opening tree open (prefetch_depth = 3): < 500ms for a 4M-game corpus.
- NFR04: ELO distribution bucket data is returned in a single DuckDB query per position, not one query per continuation.
- NFR05: Player name search (SQLite) completes in under 50ms and returns a game ID set usable for DuckDB intersection.

**Correctness**

- NFR06: Filtered stats are exactly equal to manually counting results from the matching game set — verified by tests against a known dataset.
- NFR07: ELO-weighted scores produce a strict total order (no ties broken arbitrarily) — ties broken by frequency, then SAN alphabetically.
- NFR08: Empty filter and absent filter produce identical results.

**Code Quality**

- NFR09: All new public API functions have at least one Catch2 v3 test.
- NFR10: Tests run against real SQLite + DuckDB instances (no mocks).
- NFR11: Zero new clang-tidy or cppcheck warnings.
- NFR12: All code passes ASan + UBSan under cmake --preset=dev-sanitize.

**Total NFRs: 12**

---

### Additional Requirements & Constraints

- **Filter model:** `search_filter` struct with optional fields; empty filter is valid and equivalent to no filter — must not be treated as an error.
- **ELO-weighted score formula:** Intent defined (wins in higher-rated games count more); exact formula to be finalized during implementation. Suggested: weighted average outcome normalized by Elo. Score used for ranking only, not displayed as a raw value.
- **Continuous ELO distribution:** 25-Elo bucket width; empty buckets included as zeros so the frontend has a contiguous range. Smoothing applied in the Qt visualization layer, not in the backend query.
- **Player name search cross-store:** SQLite returns matching game IDs; DuckDB intersects for opening stats. Two-store coordination without two-phase commit — DuckDB is derived data.
- **Sync toggle:** Session-only state, not persisted to user configuration.
- **Architectural constraint:** Filtered opening explorer extends existing `opening_stats` / `opening_tree` APIs — not a replacement. Filter parameter is optional; absent filter = existing behavior.

### PRD Completeness Assessment

The PRD is clear, well-scoped, and directly traceable to the two concrete user journeys. Requirements are specific and testable. Key strengths:

- FR/NFR numbering is complete and unambiguous.
- Performance targets are concrete (corpus size + latency specified).
- Out-of-scope items explicitly listed, preventing scope creep.
- Domain-specific constraints (cross-store query pattern, ELO-weighted score intent) are documented.

Gaps to investigate in the epic/architecture review:
- No epics or stories exist yet for spec 003 — this is expected and is the primary output gap.
- The ELO-weighted score formula is deferred to implementation; the architecture document should specify the data contract precisely enough that implementation can proceed.
- FR14 (sort toggle) and FR17–FR20 (sync toggle) are UI-layer concerns — the backend API needs to support them but the spec 003 backend work does not implement the Qt UI (that is spec 004). Need to confirm the HTTP API surface (FR21–FR23) is sufficient for Qt to implement the toggles.

---

## Epic Coverage Validation

### Coverage Matrix

| FR | Requirement (summary) | Existing Epic Coverage | Status |
|----|----------------------|----------------------|--------|
| FR01 | Filter games by player name + color restriction | 4b.5: player filter exists but no color restriction or partial-match spec | ⚠️ PARTIAL |
| FR02 | Filter by ELO range | None | ❌ MISSING |
| FR03 | Filter by result | 4b.5: `?result=` param | ⚠️ PARTIAL |
| FR04 | Filter by ECO prefix | None | ❌ MISSING |
| FR05 | Combine filters with AND | None | ❌ MISSING |
| FR06 | Empty filter returns all games, paginated | 4b.5: basic pagination exists | ⚠️ PARTIAL |
| FR07 | Pagination: limit (max 500) + offset | 4b.5: pagination exists; max limit is 200 not 500 | ⚠️ PARTIAL |
| FR08 | total_count in response | None | ❌ MISSING |
| FR09 | Game list includes ELO columns | 4b.5: no ELO in response schema | ⚠️ PARTIAL |
| FR10 | Selecting game navigates board | 7.3/7.5: Qt stories cover this (spec 004 scope) | ⚠️ PARTIAL |
| FR11 | Filtered opening explorer stats | None — Epic 3 is unfiltered only | ❌ MISSING |
| FR12 | Empty filter = current behavior | None | ❌ MISSING |
| FR13 | ELO-weighted score per continuation | None | ❌ MISSING |
| FR14 | Sort toggle: frequency vs ELO-weighted | None | ❌ MISSING |
| FR15 | ELO distribution buckets per continuation | None | ❌ MISSING |
| FR16 | Opening tree accepts filter | None | ❌ MISSING |
| FR17 | Sync toggle between game list and explorer | None | ❌ MISSING |
| FR18 | Sync ON: filter change propagates | None | ❌ MISSING |
| FR19 | Sync OFF: panels independent | None | ❌ MISSING |
| FR20 | Sync toggle defaults ON | None | ❌ MISSING |
| FR21 | GET /api/games with full filter params | 4b.5: missing ELO, ECO, color, total_count | ⚠️ PARTIAL |
| FR22 | GET /api/positions/{hash}/stats with filter | None — existing story has no filter params | ❌ MISSING |
| FR23 | GET /api/positions/{hash}/tree with filter | None | ❌ MISSING |

### Missing Requirements

**All missing FRs are expected** — the existing epics.md was authored against the overall project PRD (April 2026) before the spec 003 search PRD existed. There are no spec 003 stories yet. This is the primary gap to close.

#### Critical Missing (new backend API work)

- **FR11, FR16** — filter parameters threaded into `opening_stats` and `opening_tree`; core backend change
- **FR13, FR15** — ELO-weighted score and ELO distribution bucket query; new DuckDB query design
- **FR02, FR04, FR05, FR08** — ELO range, ECO prefix, filter AND composition, total_count; extend game list query

#### Medium (API extension)

- **FR22, FR23** — filter query params on existing HTTP endpoints; thin adapter work over the new filtered backend

#### UI-layer (deferred to spec 004)

- **FR14, FR17–FR20** — sort toggle and sync toggle; stateful UI concerns, not backend. Backend API (FR21–FR23) must expose enough for Qt to implement these without additional round-trips.

#### Partially covered FRs — gaps to close in stories

- **FR01**: add `color` filter param and enforce case-insensitive partial match semantics
- **FR07**: raise max limit from 200 → 500
- **FR09**: add `white_elo`, `black_elo` columns to game list response

### Coverage Statistics

- Total spec 003 FRs: 23
- FRs with full coverage in existing epics: 0
- FRs with partial coverage: 6 (FR01, FR03, FR06, FR07, FR09, FR10)
- FRs with no coverage: 17
- **Coverage: 0% full / 26% partial — new epic required**

---

## UX Alignment Assessment

### UX Document Status

Not found — no `*ux*.md` in planning artifacts.

### Assessment

No UX document is needed for spec 003. The spec delivers a **backend search API and filtered query layer** — not a Qt UI. The Qt panels (game list with filter controls, opening explorer with filter bar, sync toggle button) are spec 004 work. The PRD correctly defers all UI concerns to spec 004 and scopes spec 003 to the backend + HTTP API surface.

The two UI-adjacent requirements (FR14 sort toggle, FR17–FR20 sync toggle) are API contracts, not UI implementations: the backend must expose enough query flexibility that Qt can implement those controls without additional endpoints. FR21–FR23 cover this adequately.

### Warnings

- ⚠️ **FR10 (selecting game navigates board)** is listed in the PRD but is a pure Qt concern. The backend has no action here. This FR belongs in spec 004 stories, not spec 003 stories. It should be noted in the spec 003 epic as an out-of-scope UI responsibility.
- ℹ️ No UX design document is planned for spec 003 — this is intentional and correct.

---

## Epic Quality Review

### Scope of Review

No spec 003 epic exists yet — this is the primary structural gap. The review below therefore covers: (a) the existing epics that spec 003 extends, for quality baseline, and (b) structural requirements for the spec 003 epic to be created.

### Existing Epic Quality (Epic 3 — directly extended by spec 003)

✅ **Epic 3: Position Search & Opening Statistics** — user-centric framing, delivers standalone value, stories are properly ordered (3.1 → 3.2 → 3.3 with clear dependency chain), ACs are Given/When/Then with measurable outcomes and error cases. No violations.

✅ **Epic 4b Story 4b.5: Game List Endpoint** — partially covers spec 003 FR01/FR03/FR06/FR07/FR09. Story is well-formed but will need ACs extended rather than replaced (filter param additions, ELO columns, total_count). Extending a story's ACs is acceptable when scope is clearly additive.

### Spec 003 Epic to Be Created: Required Structural Properties

The new epic should be named **"Epic 3b: Search Filters & Filtered Opening Explorer"** (consistent with the 4b/4c/4d naming pattern for follow-on epics).

#### 🔴 Critical: FR10 misplacement

FR10 ("Selecting a game in the list navigates the board to that game") is a Qt UI behavior — the board widget is spec 004. It cannot be tested or implemented in spec 003 at the backend level. **FR10 must be excluded from spec 003 stories and added to Epic 7 (spec 004) story 7.3.**

#### 🟠 Major: Sync toggle (FR17–FR20) backend scope clarity

FR17–FR20 describe a UI toggle that keeps two panels in sync. The **backend is stateless** — it has no concept of "sync." The backend only needs to accept an optional filter on each endpoint. The sync is a Qt/client-side concern (shared filter state in the view model). Stories must make this boundary explicit: spec 003 delivers the stateless filter API; spec 004 delivers the toggle behavior in Qt. Stories should not claim to implement FR17–FR20 at the backend level.

#### 🟡 Minor: ELO-weighted score formula deferred

FR13 specifies the scoring intent but defers the formula to implementation. This is acceptable in a PRD but the spec 003 story AC must include the formula (or a testable proxy) before the story can be marked done. The story author must nail this down before implementation begins.

### Best Practices Compliance for the Upcoming Epic

| Check | Status |
|-------|--------|
| Epic delivers user value (not "add filter to DB") | ✅ Planned correctly |
| Epic can function independently (extends Epic 3, no forward deps) | ✅ |
| Stories appropriately sized | ⚠️ To be verified when stories are written |
| No forward dependencies | ✅ Epic 3 is complete; no spec 004 deps in backend work |
| FR10 scoped correctly | ❌ Must be moved to spec 004 |
| Sync toggle backend scope clear | ❌ Must be explicit in story ACs |
| ELO score formula testable | ⚠️ Must be pinned before story starts |
| Traceability to FRs maintained | ✅ FR numbering is clear in PRD |

---

## Summary and Recommendations

### Overall Readiness Status

**NEEDS WORK** — PRD is solid and ready; epics and stories for spec 003 do not yet exist. Three issues must be resolved before story creation begins. Implementation cannot start without the stories.

### Issues Requiring Action Before Story Creation

**🔴 Critical (blockers)**

1. **No spec 003 epic or stories exist.** Create Epic 3b: "Search Filters & Filtered Opening Explorer" following the established naming convention. All 23 FRs (minus FR10) need stories.

2. **FR10 is misplaced in the spec 003 PRD.** "Selecting a game navigates the board" is a Qt behavior (spec 004, Epic 7, Story 7.3). Remove it from spec 003 stories entirely. It is already covered by 7.3.

**🟠 Major (must be resolved in stories, not blockers for epic creation)**

3. **Sync toggle (FR17–FR20) scope must be explicit.** Stories must state: the backend is stateless — it accepts an optional filter parameter per request and nothing more. The sync toggle is a Qt view-model concern (spec 004). Spec 003 stories deliver the filter API only.

4. **ELO-weighted score formula must be pinned** in the story AC before implementation begins. The PRD defers the formula intentionally; the story must not. Suggested starting formula is documented in the PRD.

**🟡 Minor (can be handled during story writing)**

5. **Story 4b.5 ACs need extension** (not replacement): add `white_elo`/`black_elo` columns, raise max limit 200 → 500, add `total_count` to response, add ELO range and ECO prefix query params. These are additive AC changes on an already-complete story.

### Recommended Next Steps

1. **Create Epic 3b stories** via `bmad-create-epics-and-stories` or `bmad-create-story`, scoped as: (a) `search_filter` struct + filtered game list query, (b) filtered opening stats + ELO-weighted score + ELO bucket data, (c) filtered opening tree, (d) HTTP endpoint extensions for FR21–FR23.
2. **Extend Story 4b.5 ACs** with the five additive changes listed above — no new story needed.
3. **Move FR10 to Epic 7 Story 7.3** — add an AC: "given a game is selected in the filtered game list, when the user activates it, then the board navigates to that game."
4. **Pin ELO-weighted score formula** in the Epic 3b story AC before implementation starts.

### Final Note

This assessment identified **5 issues** across **3 categories** (coverage, scope, story quality). The PRD for spec 003 is well-formed and implementation-ready at the requirements level. The sole blocker is the absence of stories — expected at this stage of planning. Address the critical issues in order; the minor ones can be handled inline during story writing.

**Report:** `_bmad-output/planning-artifacts/implementation-readiness-report-2026-05-06.md`
