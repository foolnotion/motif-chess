# Conventions

Single source of truth for durable project rules. Every AI agent working on this project reads this file.
Cross-references architecture.md for full rationale; this file is optimized for surviving context switches.

---

## Upstream ownership

**Bogdan owns the upstream chess libraries and the foolnotion Nix overlay.**

`chesslib`, `pgnlib`, and `ucilib` are maintained upstream by Bogdan. The foolnotion overlay is how
they reach Nix. When a packaging problem surfaces (symbol not found, package unavailable, CMake config
missing), **surface it to Bogdan and wait for an upstream fix.** Never work around it with downstream
CMake hacks (`find_dependency` workarounds, manual include paths, bundled copies). Workarounds that
mask packaging problems waste effort and often make things worse.

---

## DuckDB — C API only

**DuckDB's C++ API is incompatible with Clang 21.**

`profiling_utils.hpp` instantiates `unique_ptr<ActiveTimer>` with an incomplete type; Clang 21 rejects
this even with `SYSTEM` includes. The C++ API (`duckdb::DuckDB`, `duckdb::Connection`,
`duckdb::Appender`) is **banned** in this project.

All DuckDB code uses the pure C API:

```cpp
#include <duckdb.h>
// duckdb_database, duckdb_connection, duckdb_appender — always
```

This applies to all modules for the lifetime of the project unless the upstream incompatibility is
resolved and explicitly approved.

> **Note:** The architecture document's DuckDB section describes the C++ API surface; that section is
> outdated and will be corrected. Trust this file, not the arch doc, on DuckDB API choice.

---

## `game_store::insert` duplicate policy

`game_store::insert()` **always** returns `error_code::duplicate` on a duplicate game. There is no
`insert_options`/`skip_duplicates` flag — it was removed during Story 1.3 review.

Callers (especially the Epic 2 import worker) must handle `error_code::duplicate` explicitly:

```cpp
auto result = store.insert(game);
if (!result && result.error() == error_code::duplicate) {
    logger->warn("duplicate game skipped: {}", game_id);
    // continue — not fatal
}
```

Treating a duplicate as a fatal error is a bug.

---

## Naming

- **All identifiers**: `lower_snake_case` — variables, functions, struct members, Qt signals, logger names.
- **Template parameters only**: `CamelCase` (e.g., `template <typename ResultType>`).
- **No `k_` prefix** for constants. Use `constexpr auto max_retry_count = 3;` not `constexpr auto k_max_retry_count = 3;`.
- **Namespaces** mirror CMake targets: `motif::db`, `motif::import`, `motif::search`, `motif::engine`, `motif::app`.
- **No `using namespace`** in any header, ever.

---

## Formatting and console output

Use `fmt` for string formatting and console output:

- `fmt::format(...)` for string construction.
- `fmt::print(stdout, ...)` for normal console output.
- `fmt::print(stderr, ...)` for error output.

Avoid `std::format`, `std::to_string`, `std::ostringstream`, `std::cout`, and `std::cerr` in new or
touched code. Iostreams are acceptable only when a library API or file/stream abstraction requires
them, such as `std::ifstream`/`std::ofstream` for file IO.

---

## SQL

SQL belongs in **raw string literals**:

```cpp
// Correct
constexpr auto insert_game_sql = R"sql(
    INSERT INTO game (zobrist_hash, result) VALUES (?, ?)
)sql";

// Wrong — never do this
constexpr char const* insert_game_sql = "INSERT INTO game ...";
```

SQLite table and column names: singular `lower_snake_case` nouns (`game`, `player`, `zobrist_hash`).

---

## Headers

- `#pragma once` in every header — no traditional include guards.
- Include order (clang-format enforces): standard library → third-party → project headers.
- Include what you use directly; no transitive includes.
- Full path always: `#include "motif/db/types.hpp"` — never `#include "types.hpp"`.

---

## Error handling

Every public API function returns `tl::expected<T, motif::<module>::error_code>`.

```cpp
auto result = store.find(id);
// Monadic chains preferred over if (!result) guards
result.and_then([](auto& game) { ... })
      .or_else([](auto err) { ... });
```

- `result.value()` without a prior check is forbidden (clang-tidy catches this).
- `error_code` enums are module-scoped: `motif::db::error_code`, `motif::import::error_code`, etc.
- No exceptions across module boundaries.

---

## Module boundaries

| Module | Must never include |
|---|---|
| `motif_db` | Any Qt header |
| `motif_import` | Any Qt header; SQLite or DuckDB headers directly |
| `motif_search` | Any Qt header; SQLite or DuckDB headers directly |
| `motif_engine` | Any Qt header |

`motif_import` and `motif_search` access storage exclusively through `motif_db` APIs.
Shared domain types (`game`, `player`, `event`, `position`) live in `motif_db/types.hpp`.

---

## Packaging workflow

- **`flake.nix`**: requires explicit approval from Bogdan before modification.
- **`vcpkg.json`**: updated at end-of-epic for Windows alignment only. Never modify mid-story.
- Adding a dependency means updating **both** files (one per platform), never just one.

When a dependency is absent from Nix and needed: open an issue, tell Bogdan, wait. Do not improvise.

---

## Read docs before grepping headers

When integrating a library API (DuckDB, SQLite, spdlog, glaze, taskflow, pgnlib, chesslib):
**read the library's documentation first.** Grepping headers to reverse-engineer an API has already
caused a mid-implementation discovery (Clang 21 / DuckDB C++ API incompatibility) that could have
been caught during planning. If official docs are unavailable, read the library's `README` and
`examples/` before any header.

---

## Testing

- Framework: Catch2 v3.
- Test naming: `TEST_CASE("game_store: insert deduplicates players", "[motif-db]")`.
- Storage tests use real in-memory SQLite (`:memory:`) and DuckDB instances — **no mocks for the storage layer**.
- Every public API function must have tests.
- Never run performance tests in parallel with other performance tests or other heavy test workloads; serialized execution is required so measurements are not polluted and perf gates do not fail spuriously.
- Sanitizer gate: `cmake --preset=dev-sanitize && ctest --test-dir build/dev-sanitize` must be clean before a story is marked done.

---

## Checklist before marking a story done

1. All acceptance criteria checked off in the story file.
2. `cmake --preset=dev && cmake --build build/dev` — clean build, zero warnings.
3. `ctest --test-dir build/dev` — all tests pass.
4. `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize` — zero ASan/UBSan violations.
5. `clang-format` applied to all changed files.
6. Zero new clang-tidy or cppcheck warnings.
