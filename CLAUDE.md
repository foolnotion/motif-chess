# Motif Chess — Agent Instructions

**Read `CONVENTIONS.md` before writing any code.** It is the authoritative source for naming, SQL style,
DuckDB API restrictions, error handling, module boundaries, packaging workflow, and story-done criteria.

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

- Each feature has a spec in specs/NNN-name/spec.md
- One spec at a time, one branch per spec
- Done = all acceptance criteria checked off
- Do not start a spec whose dependencies are incomplete

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
