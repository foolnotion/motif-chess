---
project_name: 'motif-chess'
user_name: 'Bogdan'
date: '2026-04-14'
status: 'complete'
sections_completed: ['technology_stack', 'language_rules', 'testing_rules', 'code_quality', 'workflow', 'critical_rules']
rule_count: 45
optimized_for_llm: true
---

# Project Context for AI Agents

_This file contains critical rules and patterns that AI agents must follow when implementing code in this project. Focus on unobvious details that agents might otherwise miss._

---

## Technology Stack & Versions

> **Agent constraints:** Never use FetchContent, ExternalProject, or submodules.
> Never modify `flake.nix` without explicit approval. `find_package()` is the
> only dependency acquisition method.

- **Language:** C++20, `CMAKE_CXX_EXTENSIONS=OFF` — standard C++ only, no GNU extensions
- **Compiler:** Clang 21 (`llvmPackages_21` via Nix)
- **Build system:** CMake 4.1.2 (Nix-provided)
  - Configure: `cmake --preset=dev`
  - Build: `cmake --build build/dev`
  - Test: `ctest --test-dir build/dev`
  - Sanitizers: `cmake --preset=dev-sanitize` (ASan + UBSan) — all code must be
    clean; verify before committing
- **Package manager:** Nix only. vcpkg is deferred (Windows port comes later).
- **String formatting:** Use `fmt::format` exclusively. Never `std::format` —
  it is unreliable in libc++ on Clang 21. fmt 11 uses `consteval`-checked
  format strings; do not pass runtime strings where a format string is expected.
  - `fmt` ≥ 11.0.2
- **Error handling:** `tl::expected<T, E>` — never `std::expected`.
  Include: `#include <tl/expected.hpp>`. Monadic ops (`.and_then`, `.or_else`,
  `.transform`) are available. Do not use exceptions.
- **Testing:** Catch2 v3 (≥ 3.7.1) — v3 API only. `Approx()` is removed; use
  `REQUIRE_THAT` with matchers. Every public API function must have tests.
- **In-house libraries (Nix flake inputs, static, author-controlled):**
  - `chesslib` (`github:foolnotion/chesslib`; Sourcehut mirror) — board
    representation, move generation, SAN parsing, Zobrist hashing. Owns all
    chess logic and move legality.
  - `pgnlib` (`github:foolnotion/pgnlib`) — PGN text parsing only. No board
    state, no SAN validation (that is motif-chess's job).
  - `ucilib` (`github:foolnotion/ucilib`) — UCI engine subprocess wrapper.
    No chess logic; FEN validation is the caller's responsibility.
  - These are Nix-only. No vcpkg ports exist. Do not attempt `FetchContent`
    or vcpkg sourcing. APIs evolve with the project — consult headers.
- **Storage:**
  - SQLite (WAL mode) — game metadata, move blobs. WAL is enabled at
    connection open time. These stores are not transactionally coupled with
    DuckDB.
  - DuckDB (latest, C++ API) — position index, opening statistics.
    Single-writer mode during import.
- **Concurrency:** `taskflow` is the preferred task graph library for
  CPU-intensive parallel pipelines (import workers, position indexing,
  statistical analysis). Alternatives may be considered, but the choice must be
  raised before spec 002 implementation begins. Adopting `taskflow` requires
  updating both `flake.nix` (with explicit approval) and `vcpkg.json`.
- **GUI:** Qt 6 (spec 004, not yet integrated — do not scaffold Qt code until
  spec 004 is active)

## Critical Implementation Rules

### Language-Specific Rules (C++20)

**Naming conventions (enforced by clang-tidy):**
- All identifiers: `lower_case` — classes, structs, functions, methods,
  variables, enums, enum constants, namespaces
- Private/protected members: `m_` prefix (e.g., `m_db_path`)
- Template parameters: `CamelCase` (e.g., `template <typename ValueType>`)
- Macros: `UPPER_CASE`

**Formatting (enforced by clang-format — run before every commit):**
- Column limit: 80
- Indent: 4 spaces, no tabs
- Pointer/reference alignment: left (`int* p`, not `int *p`)
- Braces: custom style — opening brace on new line for functions, classes,
  namespaces, structs, enums; same line for control flow

**Const correctness:**
- `const` on all variables, parameters, and methods that do not mutate
- Prefer `const auto` and `const auto&` in range-for and local bindings
- Member functions that do not mutate state must be `const`

**No raw owning pointers:**
- Use `std::unique_ptr` or `std::shared_ptr` for heap-owned resources
- Use `std::span`, references, or raw non-owning pointers for views/borrows
- Stack allocation preferred where lifetime is clear

**Error handling:**
- All fallible operations return `tl::expected<T, E>`, never throw
- Do not use `.value()` without checking — use `.and_then` / `.or_else`
  chains or explicit `if (!result)` guards
- Error types should be domain-specific enums or structs, not strings

**Compiler warnings treated as hard failures:**
- clang-tidy runs with nearly all checks enabled (see `.clang-tidy`)
- cppcheck also runs on every build (`dev` preset)
- A build with new warnings is a broken build — fix, do not suppress
- `[[nodiscard]]` is not applied aggressively; do not add it speculatively

**C++20 features in use / guidance:**
- `std::span` for non-owning array/buffer views
- `std::string_view` for non-owning string parameters
- `std::filesystem::path` for all file paths
- Ranges (`std::ranges::`, `std::views::`) are available
- Concepts and requires-clauses: use when they clarify intent
- Coroutines: not in use
- Modules: not in use; use `#pragma once` in all headers

### Testing Rules

- **Framework:** Catch2 v3 (≥ 3.7.1) — v3 API only
  - Include: `#include <catch2/catch_test_macros.hpp>`
  - `Approx()` is removed in v3; use `REQUIRE_THAT(x, WithinRel(y))` or
    `WithinAbs` matchers instead
  - Tag syntax: `[tag]` style, no `[!throws]` — use `REQUIRE_THROWS_AS` instead
- **Coverage requirement:** every public API function must have at least one test
- **Test file location:** `test/source/` — mirror the `source/` structure
- **Test naming:** `TEST_CASE("Description", "[module-tag]")`
- **Sanitizer gate:** tests must pass clean under `dev-sanitize` preset
  (ASan + UBSan) before a story is complete
- **No mocking of internal storage:** test against real SQLite/DuckDB instances
  (in-memory or temp-dir); do not mock the database layer
- **Performance acceptance criteria** are part of the spec — tests that
  assert timing (e.g., 1M game insert < 60 s) are required for those ACs

### Code Quality & Style Rules

**clang-tidy (runs on every `dev` build):**
- Nearly all checks enabled — see `.clang-tidy` for exact config
- Exceptions: `altera-*`, `fuchsia-*` (except `fuchsia-multiple-inheritance`),
  `llvmlibc-*`, `modernize-use-nodiscard`, `misc-non-private-member-variables-in-classes`
- Any new clang-tidy warning is a build failure — fix, never suppress with
  `// NOLINT` unless there is a documented reason
- `bugprone-argument-comment.CommentBoolLiterals`: prefer `enum class` with
  2 values over `bool` parameters

**cppcheck (runs on every `dev` build):**
- Inline suppressions allowed (`--inline-suppr`) but must be justified

**Include ordering (enforced by clang-format):**
1. Standard library headers (`<algorithm>`, `<vector>`, etc.)
2. Third-party library headers (`<fmt/format.h>`, `<catch2/...>`, etc.)
3. Project headers (`"lib.hpp"`, etc.)

**General rules:**
- No `else` after `return` — clang-tidy enforces this
- No old-style casts (`(int)x`) — use `static_cast`, `reinterpret_cast`, etc.
- No implicit narrowing conversions
- Prefer `enum class` over plain `enum`
- `[[nodiscard]]` only where ignoring the return value is always a bug — not
  applied speculatively
- `InsertBraces: true` in clang-format — braces always inserted; never write
  brace-free `if`/`for`/`while` bodies

### Development Workflow Rules

**Specs and branches:**
- Each feature has a spec in `specs/NNN-name/spec.md`
- One spec at a time, one branch per spec
- Do not start a spec whose dependencies are incomplete
- Done = all acceptance criteria in the spec checked off

**Commits:**
- Conventional commits: `feat`, `fix`, `refactor`, `docs`, `test`, `chore`
- clang-format must be run before every commit

**Dependency management:**
- `find_package()` only — never `FetchContent`, `ExternalProject`, or submodules
- Adding any dependency requires updating both `flake.nix` (requires explicit
  approval) and `vcpkg.json`
- In-house libs (`chesslib`, `pgnlib`, `ucilib`) are updated via
  `nix flake update <lib>` then rebuild + test to confirm compatibility

**Spec dependencies (current):**
- `001-database-schema` requires: `chesslib` compact 16-bit move encode/decode
- `002-import-pipeline` requires: `001-database-schema` complete + full
  `chesslib` and `pgnlib` milestone tasks (see `plans/design.md`)
- `003-search` and `004-qt-gui` depend on prior specs — check `plans/design.md`
  before starting

### Critical Don't-Miss Rules

**Storage layer:**
- 16-bit move encoding (CPW convention) via `chesslib::codec` — bits 15-10:
  from square, 9-4: to square, 3-0: flags. Do not invent an alternative encoding.
- `moves_blob` in the `Games` table is a raw `std::span<uint16_t>` stored as
  BLOB — approximately 80 bytes/game average
- Player and event deduplication is required on insert — same name = same id
- Game deduplication must be checked before insert when `skip_duplicates` is set
- SQLite and DuckDB are not transactionally coupled — do not attempt two-phase
  commit across them
- DuckDB is single-writer during import; SQLite uses WAL for concurrent reads

**In-house library boundaries — do not cross these:**
- `chesslib`: all move legality, SAN conversion, Zobrist hashing lives here
- `pgnlib`: text parsing only — it produces `pgn::game` structs with raw SAN
  strings; it does not validate chess rules
- `ucilib`: engine process lifecycle only — it does not interpret positions

**ASan / UBSan hard failures — patterns to avoid:**
- `reinterpret_cast` on misaligned storage
- Union-based type punning
- Signed integer overflow (use unsigned or check bounds)
- Out-of-bounds `std::span` or array access
- Use-after-move (clang-tidy will also catch this)

**CMake:**
- Never write `cmake_minimum_required(VERSION 3.14)` in new files — the
  effective floor is CMake 4.1.2
- Never add a target with `add_library(... SHARED ...)` — in-house libs are
  static; prefer static or object libraries

**Things that will not work:**
- `std::format` — use `fmt::format`
- `std::expected` — use `tl::expected`
- `FetchContent` / `ExternalProject` / submodules — forbidden
- Modifying `flake.nix` without explicit user approval
- Qt code of any kind until spec 004 is active

---

## Usage Guidelines

**For AI agents:** Read this file before implementing any code. Follow all rules
exactly. When in doubt, prefer the more restrictive option. If a rule conflicts
with a spec, the spec's acceptance criteria take precedence — flag the conflict.

**For humans:** Keep this file lean. Update when the stack changes (new dep,
new in-house lib API, new spec dependency). Remove rules that become obvious
over time.

_Last updated: 2026-04-14_
