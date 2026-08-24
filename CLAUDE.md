# Motif Chess — Agent Instructions

**Read `CONVENTIONS.md` before writing any code.** It is the authoritative source for naming, SQL style,
error handling, module boundaries, packaging workflow, and story-done criteria.

## Build

    cmake --preset=dev
    cmake --build build/dev
    ctest --test-dir build/dev

## Code

- C++23, Clang 21 (llvmPackages_21)
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
- After cloning, create these symlinks so BMAD skill scripts resolve correctly:
  ```
  ln -s ../motif-chess-meta/bmad _bmad
  ln -s ../motif-chess-meta/bmad-output _bmad-output
  ln -s ../motif-chess-meta/specs specs
  ```
- One spec at a time, one branch per spec
- Done = all acceptance criteria checked off
- Do not start a spec whose dependencies are incomplete

## Metadata

- Story and spec files use YAML frontmatter: `id`, `uuid`, `type`, `title`, `epic`, `status`, `assignee`, `depends_on`, `implements`, `acceptance_criteria`, `provenance`.
- `meta/registry.yaml` (in `motif-chess-meta`) maps slugs to UUIDs — add an entry whenever you create a new entity.
- At session end, write `meta/sessions/session-YYYY-MM-DD-{short-id}.yaml` with `status: pending-reconciliation` and a `claims` list. Claim types:
  - `status_update` — entity status changed (e.g. story moved to review)
  - `field_update` — any other frontmatter field changed (e.g. AC status flipped to verified)
  - `insight` — technical finding worth preserving across sessions

  Minimal example:
  ```yaml
  id: session-2026-05-10-a3f2
  uuid: <generate-fresh-uuid>
  type: session
  agent: claude-sonnet-4-6
  status: pending-reconciliation
  claims:
    - type: status_update
      target: story-7-2
      field: status
      value: review
    - type: insight
      text: "Qt QML compiler generates float equality comparisons in property bindings; suppress -Werror=float-equal on the app target only"
      affects: [story-7-2]
  ```
- `sprint-status.yaml` is a **generated view** — never hand-edit it. Run `bmad-sprint-status` to regenerate.
- Conflicts (two sessions modified the same field on the same entity) go to `meta/conflicts/` — do not silently overwrite.
- All `depends_on`, `implements`, and `affects` references use **slugs only** — never write UUIDs into these fields.

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

## Research Order

When investigating a library or framework API (Slint, chesslib, pgnlib, etc.):
1. **Read project docs / official docs online first** — prefer authoritative documentation over source spelunking
2. **Grep / read source only as a fallback** — when docs are absent, ambiguous, or the answer requires seeing the actual implementation

## After Every New Binary

Run the app briefly and check stderr before declaring a build successful:

    SLINT_BACKEND=winit-software ./build/bin/motif_slint_app 2>&1 | head -40

A clean compile does not mean a clean runtime. `motif_slint_app` requires a compositor
(`WAYLAND_DISPLAY` or `DISPLAY`); without one it panics immediately on startup — that is
expected outside a graphical session, not a build failure. `SLINT_BACKEND=winit-software`
avoids GPU/driver issues when testing over a remote session (e.g. waypipe).

## Commits

Conventional commits: feat, fix, refactor, docs, test, chore.

Never add AI authorship attribution to a commit message. Do not add
`Co-Authored-By` trailers for any AI agent and do not add `Claude-Session`
trailers. Before committing in a fresh checkout, run
`scripts/install-git-hooks.sh`; CI enforces this rule.

## Do Not

- Modify flake.nix without explicit approval
- Add deps without updating both flake.nix and vcpkg.json
- Skip acceptance criteria
- Write code that triggers ASan or UBSan
