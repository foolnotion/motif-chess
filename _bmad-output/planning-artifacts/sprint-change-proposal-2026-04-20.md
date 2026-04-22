# Sprint Change Proposal

**Date:** 2026-04-20
**Project:** motif-chess
**Change Scope:** Moderate
**Triggering Story:** `2-5-import-completion-summary-error-logging`

## 1. Issue Summary

During implementation and profiling around story `2-5-import-completion-summary-error-logging`, the team discovered that PGN import performance is not a narrowly local issue in the current story. The measured bottlenecks span three layers:

- upstream `chesslib` SAN resolution
- upstream `pgnlib` PGN materialization / import streaming
- downstream motif-chess SQLite/DuckDB import-path behavior

The issue is therefore not a defect in the current story requirements. It is a newly clarified performance workstream revealed during implementation.

## 2. Impact Analysis

### Epic Impact

- **Epic 2 remains valid** and should continue as the home for PGN import performance work.
- **Story `2-5` remains separate** and should finish its current review independently.
- **New stories are needed** to represent the performance work explicitly:
  - `2-6-adopt-upstream-chesslib-san-optimization`
  - `2-7-integrate-pgnlib-import-stream-into-import-pipeline`
  - `2-8-reprofile-and-tune-remaining-import-storage-path`

### Artifact Impact

- **PRD:** no change required
- **Epics:** add three new Epic 2 stories for the performance track
- **Architecture:** minor adjustment recommended to reflect:
  - import-oriented `pgnlib` streaming as the preferred hot-path integration
  - explicit DuckDB secondary index creation as a post-load concern when possible
- **Sprint Status:** add the three new stories as `backlog`

### Technical Impact

- local profiling indicates that the current best 10k import path still spends significant time in persistence/finalization even after initial tuning
- upstream `chesslib` SAN work reports large isolated gains
- upstream `pgnlib` `import_stream` work reports strong parser/materialization gains
- allocator/arena work is not yet justified as the next highest-priority change

## 3. Recommended Approach

**Selected path:** Direct Adjustment

### Rationale

- The issue does **not** require rollback of completed work.
- The issue does **not** require PRD or MVP scope reduction.
- The issue **does** require explicit backlog/sprint reorganization so that performance work is tracked separately from the currently reviewed story.
- The most effective next implementation sequence is staged adoption of proven upstream improvements, followed by fresh full-path profiling.

### Rejected Alternatives

- **Rollback:** not justified; current Epic 2 work remains valid and usable.
- **New epic:** not yet justified; the work still cleanly belongs under Epic 2 import concerns.
- **Allocator-first redesign:** premature without integrating the known upstream wins first.

## 4. Detailed Change Proposals

### 4.1 Epics Update

**Artifact:** `_bmad-output/planning-artifacts/epics.md`

**Proposed change:** add three new Epic 2 stories after story 2.5.

**Stories to add:**

- `2.6 Adopt Upstream chesslib SAN Optimization`
- `2.7 Integrate pgnlib import_stream into the Import Pipeline`
- `2.8 Reprofile and Tune the Remaining Import Storage Path`

**Rationale:** keeps the current story review isolated while giving the newly discovered performance work explicit scope.

### 4.2 Sprint Status Update

**Artifact:** `_bmad-output/implementation-artifacts/sprint-status.yaml`

**Proposed change:** add the following backlog entries under Epic 2:

```yaml
  2-6-adopt-upstream-chesslib-san-optimization: backlog
  2-7-integrate-pgnlib-import-stream-into-import-pipeline: backlog
  2-8-reprofile-and-tune-remaining-import-storage-path: backlog
```

**Rationale:** exposes the approved scope change to sprint execution without disturbing `2-5`.

### 4.3 Architecture Update

**Artifact:** `_bmad-output/planning-artifacts/architecture.md`

**Proposed change:** make a narrow clarification update only:

- `pgnlib` import-oriented streaming API is the preferred import-path integration once available
- full PGN materialization should be avoided on the hot import path
- explicit DuckDB secondary indexes should be treated as post-load artifacts when immediate query-serving is not required
- `chesslib` remains the sole owner of SAN resolution and Zobrist identity

**Rationale:** aligns the architecture baseline with the approved performance direction without redesigning the system.

## 5. Implementation Handoff

**Scope classification:** Moderate

### Handoff Recipients

- **Product Owner / Story authoring workflow**
  - create the new Epic 2 stories and attach the approved acceptance criteria
- **Developer workflow**
  - complete review and closure of `2-5`
  - implement `2-6`, `2-7`, and `2-8` in sequence
- **Architecture maintenance**
  - apply the narrow architecture clarification after proposal approval

### Recommended Sequencing

1. Finish review of `2-5`
2. Create and validate `2-6`
3. Implement `2-6` after upstream `chesslib` approval is available
4. Create and implement `2-7`
5. Run `2-8` only after both upstream integrations are landed

### Success Criteria

- `2-5` closes without absorbing unrelated performance work
- new performance scope is visible in `epics.md` and `sprint-status.yaml`
- architecture wording reflects the agreed import-path direction
- follow-on implementation prioritizes measured upstream wins before deeper downstream redesign

## 6. Summary

- **Issue addressed:** PGN import performance work discovered during Epic 2 implementation
- **Change scope:** Moderate
- **Artifacts affected:** `epics.md`, `architecture.md`, `sprint-status.yaml`
- **No PRD/MVP change required**
- **Recommended route:** continue under Epic 2 with explicit new stories
