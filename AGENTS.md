# Motif Chess — Agent Instructions

**Read `CONVENTIONS.md` before writing any code.** It is the authoritative source for naming, SQL style,
DuckDB API restrictions, error handling, module boundaries, packaging workflow, and story-done criteria.

## Build

    cmake --preset=dev
    cmake --build build/dev
    ctest --test-dir build/dev

## Code

- C++23, Clang 21
- Zero warnings from clang-tidy and cppcheck
- clang-format before every commit
- tl::expected for errors, not exceptions
- const correctness everywhere
- No raw owning pointers

## Dependencies

- Nix (Linux/macOS) or vcpkg (Windows)
- find_package() only — never FetchContent, ExternalProject, or submodules
- Adding a dep means updating both flake.nix and vcpkg.json

## Testing

- Catch2 v3
- Every public API function must have tests
- Sanitizers: cmake --preset=dev-sanitize

## Workflow

- Feature specs, BMAD artifacts, and sprint state live in the sibling repo `motif-chess-meta`
  (path: `../motif-chess-meta`). Specs are at `specs/NNN-name/spec.md` there.
- One spec at a time, one branch per spec
- Done = all acceptance criteria checked off
- Do not start a spec whose dependencies are incomplete

## Metadata

- Story and spec files use YAML frontmatter: `id`, `uuid`, `type`, `title`, `epic`, `status`, `assignee`, `depends_on`, `implements`, `acceptance_criteria`, `provenance`.
- `meta/registry.yaml` (in `motif-chess-meta`) maps slugs to UUIDs — add an entry whenever you create a new entity.
- `sprint-status.yaml` is a **generated view** — never hand-edit it.
- Conflicts (two sessions modified the same field on the same entity) go to `meta/conflicts/` — do not silently overwrite.
- All `depends_on`, `implements`, and `affects` references use **slugs only** — never write UUIDs into these fields.

## Session Write (required at end of every session)

Write `../motif-chess-meta/meta/sessions/session-YYYY-MM-DD-{short-id}.yaml` before closing the
conversation. Use a short random ID (4–6 hex chars). Set `status: pending-reconciliation`.

Claim types:
- `status_update` — story/spec status changed
- `field_update` — any other frontmatter field changed (AC verified, assignee set, etc.)
- `insight` — technical finding worth preserving across sessions

```yaml
id: session-2026-05-10-a3f2
uuid: <generate-fresh-uuid>
type: session
agent: <your-agent-id>          # e.g. opencode, claude-sonnet-4-6, gemini-2-5-pro
started: "2026-05-10T10:00:00Z" # approximate
ended: "2026-05-10T11:30:00Z"   # approximate
status: pending-reconciliation
claims:
  - type: status_update
    target: story-7-2
    field: status
    value: review
  - type: field_update
    target: story-7-2
    field: provenance.modified
    value: {at: "2026-05-10", by: claude-sonnet-4-6}
  - type: insight
    text: "Qt QML compiler generates float equality comparisons in property bindings; suppress -Werror=float-equal on the app target only"
    affects: [story-7-2]
```

Omit claims for things you only read. Include one claim per distinct field changed.

## Devlog

When asked "devlog", produce an entry for docs/devlog/YYYY-WNN.md:

    # Week NN — YYYY-MM-DD

    ## Decisions
    - [DNNN] Title: what and why. What was rejected.

    ## Learned
    - Technical insights.

    ## Done
    - Deliverables completed.

    ## Problems and Solutions
    - Problem: X — Solution: Y

    ## Open Questions
    - Things to revisit.

Decisions are numbered sequentially across all entries (D001, D002, ...).

## Commits

Conventional commits: feat, fix, refactor, docs, test, chore.

## Do Not

- Modify flake.nix without explicit approval
- Add deps without updating both flake.nix and vcpkg.json
- Skip acceptance criteria
- Write code that triggers ASan or UBSan
