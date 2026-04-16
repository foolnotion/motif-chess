# Story 1.1: Project Scaffold Restructure

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

## Story

As a developer,
I want the project build system restructured into per-module static libraries with per-module test executables,
so that each module can be developed, built, and tested independently without touching unrelated code.

## Acceptance Criteria

1. **Given** the current root `CMakeLists.txt` declares `VERSION 3.14` and contains a monolithic template library  
   **When** the scaffold restructure story is complete  
   **Then** `cmake_minimum_required` is set to the minimum version required by features actually in use  
   _(Audit `cmake/` helpers; `CMakePresets.json` uses preset schema `"version": 2` which requires CMake ≥ 3.19)_

2. **And** `source/lib.cpp`, `source/lib.hpp`, `source/main.cpp` are removed

3. **And** the following module directories exist, each with a stub `CMakeLists.txt` defining an empty static library target:
   - `source/motif/db/`
   - `source/motif/import/`
   - `source/motif/search/`
   - `source/motif/engine/`

4. **And** the root `CMakeLists.txt` uses `add_subdirectory` for each module instead of the monolithic `add_library(motif_lib OBJECT ...)` block

5. **And** `test/CMakeLists.txt` defines three per-module test executables with `catch_discover_tests`:
   - `motif_db_test`
   - `motif_import_test`
   - `motif_search_test`

6. **And** `cmake --preset=dev && cmake --build build/dev` succeeds with zero clang-tidy and zero cppcheck warnings

7. **And** `ctest --test-dir build/dev` passes (no tests yet, but the harness runs)

## Tasks / Subtasks

- [x] Audit cmake minimum version requirement (AC: #1)
  - [x] Review `cmake/` helpers for features used (e.g. `cmake_path`, `file(CONFIGURE ...)`, generator expressions)
  - [x] Check `CMakePresets.json` — `"version": 2` requires CMake ≥ 3.19; `"version": 3` requires ≥ 3.21
  - [x] Set `cmake_minimum_required` to the highest lower bound found (at minimum 3.19 for presets v2)
  - [x] Update the `cmakeMinimumRequired` field in `CMakePresets.json` to match

- [x] Remove template placeholder files (AC: #2)
  - [x] Delete `source/lib.cpp`
  - [x] Delete `source/lib.hpp`
  - [x] Delete `source/main.cpp`

- [x] Create module directory stubs (AC: #3, #4)
  - [x] Create `source/motif/db/CMakeLists.txt` — empty static lib `motif_db`, includes `source/` as include root
  - [x] Create `source/motif/import/CMakeLists.txt` — empty static lib `motif_import`
  - [x] Create `source/motif/search/CMakeLists.txt` — empty static lib `motif_search`
  - [x] Create `source/motif/engine/CMakeLists.txt` — empty static lib `motif_engine` (Phase 2 stub)
  - [x] Add placeholder `.cpp` files if required by CMake to avoid empty-library warnings (one stub `.cpp` per module is acceptable)

- [x] Rewrite root `CMakeLists.txt` (AC: #4)
  - [x] Remove `add_library(motif_lib OBJECT ...)`, `add_executable(motif_exe ...)`, and install rule for the exe
  - [x] Add `add_subdirectory(source/motif/db)`, `add_subdirectory(source/motif/import)`, `add_subdirectory(source/motif/search)`, `add_subdirectory(source/motif/engine)`
  - [x] Keep `dev-mode.cmake` include and developer mode guard unchanged
  - [x] Retain `find_package(fmt REQUIRED)` — it will be re-linked from `motif_db` once that module is implemented; for now it may remain at root level or be moved into `motif_db/CMakeLists.txt`

- [x] Update `cmake/dev-mode.cmake` (AC: #4, #6)
  - [x] Remove `run-exe` custom target (depends on removed `motif_exe`)

- [x] Rewrite `test/CMakeLists.txt` (AC: #5)
  - [x] Remove `add_executable(motif_test ...)` and associated `motif_lib` linkage
  - [x] Add `motif_db_test` executable — link `motif_db` + `Catch2::Catch2WithMain`; `catch_discover_tests(motif_db_test)`
  - [x] Add `motif_import_test` executable — link `motif_import` + `Catch2::Catch2WithMain`; `catch_discover_tests(motif_import_test)`
  - [x] Add `motif_search_test` executable — link `motif_search` + `Catch2::Catch2WithMain`; `catch_discover_tests(motif_search_test)`
  - [x] Create stub test source files: `test/source/motif_db/placeholder_test.cpp`, `test/source/motif_import/placeholder_test.cpp`, `test/source/motif_search/placeholder_test.cpp` — each contains exactly one `TEST_CASE` so `catch_discover_tests` can find at least one test

- [x] Verify clean build and test run (AC: #6, #7)
  - [x] `cmake --preset=dev` configures without errors
  - [x] `cmake --build build/dev` compiles with zero clang-tidy warnings and zero cppcheck warnings
  - [x] `ctest --test-dir build/dev` exits zero

## Dev Notes

- **CMake minimum version:** The current `cmake_minimum_required(VERSION 3.14)` is wrong — it was the template default, not derived from features in use. `CMakePresets.json` schema `"version": 2` requires CMake ≥ 3.19. Run `cmake --version` to confirm the Nix-provided version (4.1.2 as of this writing); set the floor to whatever features actually require, using 3.19 as the audited lower bound.

- **No `motif_app` yet:** `source/motif/app/` is NOT scaffolded in this story. Qt is deferred to spec 004. Do not create it here.

- **Static libraries only:** All `add_library(...)` calls must use `STATIC`. No `OBJECT` or `SHARED` libraries. See `project-context.md` rule: "Never add a target with `add_library(... SHARED ...)` — in-house libs are static."

- **Include root:** Every module's `CMakeLists.txt` sets its include directory to `${PROJECT_SOURCE_DIR}/source` (not the module subdirectory). This ensures `#include "motif/db/types.hpp"` works correctly from any module. Example:
  ```cmake
  target_include_directories(motif_db PUBLIC
      "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>")
  ```

- **Stub `.cpp` files:** CMake may warn or error if a static library has no source files. Add a minimal stub per module, e.g.:
  ```cpp
  // source/motif/db/motif_db.cpp
  // placeholder — replaced in Story 1.2
  ```
  Alternatively, use `INTERFACE` libraries for empty stubs if preferred — but `STATIC` with one stub `.cpp` is simpler and avoids link-order issues later.

- **Stub test files:** Each test executable needs at least one `.cpp` so CMake can compile it and `catch_discover_tests` can enumerate. One minimal `TEST_CASE` per file is sufficient:
  ```cpp
  #include <catch2/catch_test_macros.hpp>
  TEST_CASE("placeholder", "[motif_db]") { SUCCEED(); }
  ```

- **`fmt` dependency:** The existing root `CMakeLists.txt` links `fmt::fmt` to `motif_lib`. After removing `motif_lib`, either move `find_package(fmt REQUIRED)` and the link to `motif_db/CMakeLists.txt`, or keep it at root and propagate via target properties. `motif_db` will use fmt once implemented (Story 1.2+). For this story, whichever approach keeps the build clean is acceptable.

- **`cmake/install-rules.cmake`:** The install rules reference `motif_exe`. Either update or guard with a check for target existence. Since `motif_exe` is being removed and no replacement binary exists yet, removing the install-rules include from root `CMakeLists.txt` for this story is acceptable.

- **clang-tidy scope:** clang-tidy runs with `--header-filter=^${sourceDir}/` (see `CMakePresets.json`). With empty stubs, there is essentially nothing to lint. The zero-warnings requirement is trivially met here — but confirm it explicitly.

- **No `find_package` for chess libraries yet:** `chesslib`, `pgnlib`, `ucilib`, SQLite, DuckDB, taskflow, glaze are added in later stories (Story 1.2+). Do not add them here.

- **`test/source/` directory structure:** Create subdirectories:
  - `test/source/motif_db/`
  - `test/source/motif_import/`
  - `test/source/motif_search/`
  These mirror the structure from `architecture.md` § "Project Structure & Boundaries".

### Project Structure Notes

**Files to delete:**
- `source/lib.cpp`
- `source/lib.hpp`
- `source/main.cpp`

**Files to create:**
- `source/motif/db/CMakeLists.txt`
- `source/motif/db/motif_db.cpp` (stub)
- `source/motif/import/CMakeLists.txt`
- `source/motif/import/motif_import.cpp` (stub)
- `source/motif/search/CMakeLists.txt`
- `source/motif/search/motif_search.cpp` (stub)
- `source/motif/engine/CMakeLists.txt`
- `source/motif/engine/motif_engine.cpp` (stub)
- `test/source/motif_db/placeholder_test.cpp`
- `test/source/motif_import/placeholder_test.cpp`
- `test/source/motif_search/placeholder_test.cpp`

**Files to modify:**
- `CMakeLists.txt` — remove monolithic lib/exe, add `add_subdirectory` per module, update `cmake_minimum_required`
- `CMakePresets.json` — update `cmakeMinimumRequired` to match
- `cmake/dev-mode.cmake` — remove `run-exe` target
- `test/CMakeLists.txt` — replace monolithic `motif_test` with three per-module executables

**Module dependency graph (this story establishes the scaffold):**
```
motif_app → motif_db, motif_import, motif_search, motif_engine   (spec 004)
motif_import → motif_db
motif_search → motif_db
motif_engine → (ucilib; Phase 2 stub — no deps for now)
motif_db → (SQLite, DuckDB, glaze, chesslib — added in Story 1.2+)
```

In this story, none of the inter-module links exist yet. Each module is a standalone empty static library.

### References

- [Source: `_bmad-output/planning-artifacts/architecture.md` § "Project Structure & Boundaries"] — complete directory tree, module boundary table
- [Source: `_bmad-output/planning-artifacts/architecture.md` § "Module Structure"] — dependency graph, library names
- [Source: `_bmad-output/planning-artifacts/epics.md` § "Story 1.1"] — acceptance criteria (AR01, AR02)
- [Source: `_bmad-output/project-context.md` § "Technology Stack"] — CMake constraints, static-only libs, `CMAKE_CXX_EXTENSIONS=OFF`
- [Source: `_bmad-output/project-context.md` § "Critical Don't-Miss Rules / CMake"] — `cmake_minimum_required` guidance, no SHARED libs
- [Source: `CMakePresets.json`] — preset schema version 2 → requires CMake ≥ 3.19
- [Source: `cmake/dev-mode.cmake`] — `run-exe` target to remove

## Dev Agent Record

### Agent Model Used

claude-sonnet-4-6

### Debug Log References

- `CMakeUserPresets.json` (gitignored, user-local) had `dev-common` inheriting `vcpkg`. On Nix, `VCPKG_ROOT` is unset so the toolchain path resolves to an invalid `/scripts/buildsystems/vcpkg.cmake`. Removed `vcpkg` from `dev-common` inherits in `CMakeUserPresets.json` to fix. Deps come from Nix, not vcpkg.
- Stale `build/dev/CMakeCache.txt` from a previous failed configure preserved the invalid `CMAKE_TOOLCHAIN_FILE`. Cleared `build/dev/` before re-configuring.

### Completion Notes List

- Set `cmake_minimum_required` to 3.19 (audited lower bound from CMakePresets.json schema v2); no cmake/ helpers require beyond 3.19
- Deleted `source/lib.cpp`, `source/lib.hpp`, `source/main.cpp` (template placeholders)
- Created four module directories each with a `STATIC` library target and one stub `.cpp`, include root set to `${PROJECT_SOURCE_DIR}/source`
- Rewrote root `CMakeLists.txt`: removed monolithic `motif_lib OBJECT` + `motif_exe`; added four `add_subdirectory` calls; removed install-rules include (references removed `motif_exe`); retained `find_package(fmt REQUIRED)` at root for later use
- Removed `run-exe` custom target from `cmake/dev-mode.cmake`
- Rewrote `test/CMakeLists.txt` with three per-module executables (`motif_db_test`, `motif_import_test`, `motif_search_test`)
- Created stub test files with one `TEST_CASE` each (using `SUCCEED()`)
- `cmake --preset=dev` + `cmake --build build/dev` → zero warnings; `ctest --test-dir build/dev` → 3/3 tests passed

### File List

**Deleted:**
- `source/lib.cpp`
- `source/lib.hpp`
- `source/main.cpp`

**Created:**
- `source/motif/db/CMakeLists.txt`
- `source/motif/db/motif_db.cpp`
- `source/motif/import/CMakeLists.txt`
- `source/motif/import/motif_import.cpp`
- `source/motif/search/CMakeLists.txt`
- `source/motif/search/motif_search.cpp`
- `source/motif/engine/CMakeLists.txt`
- `source/motif/engine/motif_engine.cpp`
- `test/source/motif_db/placeholder_test.cpp`
- `test/source/motif_import/placeholder_test.cpp`
- `test/source/motif_search/placeholder_test.cpp`

**Modified:**
- `CMakeLists.txt`
- `CMakePresets.json`
- `CMakeUserPresets.json` (gitignored — Nix env: removed vcpkg inheritance)
- `cmake/dev-mode.cmake`
- `test/CMakeLists.txt`

## Change Log

- 2026-04-14: Story implemented. Scaffold restructured from monolithic template to four per-module static libs with per-module test executables. All ACs satisfied; 3/3 placeholder tests pass.
