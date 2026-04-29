# Story 7.1: Database Workspace - Create, Open, Recent & Scratch

Status: ready-for-dev

## Story

As a user,
I want motif-chess to start with a first-class database workspace where I can create, open, reopen, or use a scratch database,
so that every game list, import, search, and analysis workflow has an explicit active database context.

## Acceptance Criteria

1. **Given** the application is launched for the first time
   **When** the main window appears
   **Then** startup time from process start to usable window is under 3 seconds
   **And** a database workspace/start screen is shown before game list, import, search, or analysis workflows.

2. **Given** no app config exists
   **When** the application starts
   **Then** `${XDG_CONFIG_HOME:-$HOME/.config}/motif-chess/config.json` is created with defaults for database directory, empty recent database list, default engine paths, and UI preferences
   **And** config is stored per-user, never inside database bundle files.

3. **Given** the database workspace is visible
   **When** the user creates a named database at a chosen directory
   **Then** `motif::db::database_manager::create` creates a portable bundle containing `games.db`, `positions.duckdb`, and `manifest.json`
   **And** the new database becomes the active database
   **And** the active database name and path are visible in the shell.

4. **Given** the database workspace is visible
   **When** the user opens an existing database bundle
   **Then** `motif::db::database_manager::open` validates and opens the bundle
   **And** invalid, missing, schema-mismatched, or corrupt bundles are reported through the shell without crashing
   **And** failed opens do not replace the previous active database.

5. **Given** the user has previously opened persistent databases
   **When** the application starts or the workspace is shown
   **Then** recent databases are populated from `config.json`
   **And** missing recent paths are shown as unavailable and can be removed
   **And** successfully opened databases move to the front of the recent list without duplicates.

6. **Given** the database workspace is visible
   **When** the user selects the scratch database option
   **Then** `motif::db::scratch_base::instance()` becomes the active database context
   **And** the shell clearly labels it as temporary
   **And** scratch is never persisted to disk and never added to recent databases.

7. **Given** no active database exists
   **When** the user attempts to browse games, import PGN, search positions, or start analysis
   **Then** the shell prompts the user to create, open, or use scratch first
   **And** backend workflows are not started without an active database context.

8. **Given** the application is launched with one or more `.pgn` file paths as command-line arguments
   **When** the main window appears
   **Then** those paths are captured as a queued import list
   **And** if no active database exists, the workspace asks the user to choose/create/use scratch before import can proceed
   **And** missing or non-regular paths are reported through the shell without crashing.

9. **Given** the application is installed on Linux
   **When** the desktop entry is registered
   **Then** double-clicking a `.pgn` file in a file manager opens motif-chess with that file path passed to the application
   **And** the desktop file advertises `application/x-chess-pgn`.

10. **Given** the implementation is reviewed
    **When** includes and target links are inspected
    **Then** all Qt code is confined to `motif_app`
    **And** no Qt headers appear in `motif_db`, `motif_import`, `motif_search`, `motif_engine`, or `motif_http`.

11. **Given** all changes are implemented
    **When** `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev` is run
    **Then** all tests pass with zero new clang-tidy or cppcheck warnings.

12. **Given** all changes are implemented
    **When** `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize` is run
    **Then** all tests pass with zero ASan/UBSan violations.

## Tasks / Subtasks

- [ ] Task 1: Resolve Qt dependency gate before code edits (AC: 10, 11, 12)
  - [ ] Confirm whether Qt 6 is already available to CMake in the active Nix shell.
  - [ ] If Qt 6 is not already available, stop and ask Bogdan for explicit approval before modifying `flake.nix` or `vcpkg.json`.
  - [ ] Resolve the manifest policy explicitly with Bogdan before edits: project rules require dependency changes to stay aligned across Nix/vcpkg, while `vcpkg.json` is normally updated only at end-of-epic.
  - [ ] Prefer the smallest Qt surface for this story: `Qt6::Core`, `Qt6::Gui`, and `Qt6::Widgets`. Do not add Qt Quick, QML, or Charts.

- [ ] Task 2: Add the `motif_app` target and minimal desktop executable (AC: 1, 10, 11)
  - [ ] Create `source/motif/app/`.
  - [ ] Add `source/motif/app/CMakeLists.txt` defining the app target and linking Qt only in this module.
  - [ ] Add `source/motif/app/main.cpp` with `QApplication`, project metadata, startup timing, command-line path capture, and a minimal `QMainWindow`.
  - [ ] Add `add_subdirectory(source/motif/app)` in the root `CMakeLists.txt`.
  - [ ] Keep the existing `motif_http_server` executable unchanged.

- [ ] Task 3: Implement app config JSON load/create/save (AC: 2, 5)
  - [ ] Define an `app_config` struct in `motif::app` with `database_directory`, `recent_databases`, `engine_paths`, and `ui_preferences`.
  - [ ] Store config at `${XDG_CONFIG_HOME:-$HOME/.config}/motif-chess/config.json`.
  - [ ] Use glaze JSON serialization, not `QSettings`, because the product contract requires a JSON file at an exact path.
  - [ ] Create parent directories on first launch.
  - [ ] On missing config, write defaults and continue.
  - [ ] On malformed config, return a typed app error and show a non-crashing shell-level error state; do not overwrite the malformed file silently.
  - [ ] Persist recent databases as paths plus display metadata needed for the workspace; do not store config inside bundles.

- [ ] Task 4: Implement active database workspace state (AC: 1, 3, 4, 5, 6, 7)
  - [ ] Create an app-layer database workspace/controller that owns the active persistent `database_manager` or a non-owning scratch reference.
  - [ ] Model active database state explicitly: none, persistent bundle, or scratch.
  - [ ] Expose active database display name, path, and temporary/persistent status to the shell.
  - [ ] Ensure failed opens leave the previous active database unchanged.
  - [ ] Keep this app-layer state out of `motif_db`; do not add Qt or UI concepts to backend modules.

- [ ] Task 5: Implement create/open/recent/scratch UI shell (AC: 1, 3, 4, 5, 6, 7)
  - [ ] Add a workspace/start screen with actions for Create Database, Open Database, Recent Databases, and Scratch Database.
  - [ ] Use `database_manager::create(path, name)` for create and `database_manager::open(path)` for open.
  - [ ] Validate selected paths and surface typed errors without throwing or crashing.
  - [ ] Move successfully opened persistent databases to the front of `recent_databases` without duplicates.
  - [ ] Show missing recent database paths as unavailable and allow removing them from config.
  - [ ] Label scratch clearly as temporary and exclude it from recent databases.

- [ ] Task 6: Capture `.pgn` launch arguments for queued import (AC: 8)
  - [ ] Parse command-line positional arguments after Qt has consumed its own options.
  - [ ] Accept one or more paths ending in `.pgn` case-insensitively.
  - [ ] Resolve each path to an absolute path when possible.
  - [ ] Store valid paths in an in-memory queued import list owned by the app shell.
  - [ ] Surface invalid paths in the shell status area or startup diagnostics without throwing.
  - [ ] Do not start imports in this story; actual import execution/progress belongs to Story 4.4.
  - [ ] If queued imports exist and no active database is selected, keep the workspace open and prompt for create/open/scratch first.

- [ ] Task 7: Add Linux desktop entry packaging artifacts (AC: 9)
  - [ ] Add a desktop entry file under an appropriate packaging/resources path, e.g. `packaging/linux/motif-chess.desktop`.
  - [ ] Include `Type=Application`, `Name=motif-chess`, `Exec=motif_chess %F`, `MimeType=application/x-chess-pgn;`, `Terminal=false`, and relevant categories.
  - [ ] Add CMake install rules for the desktop entry if install rules are active.
  - [ ] Document the local registration smoke test: install/copy the desktop file, run `update-desktop-database`, then use `xdg-mime query default application/x-chess-pgn`.

- [ ] Task 8: Add focused tests (AC: 2, 3, 4, 5, 6, 7, 8, 11, 12)
  - [ ] Add `motif_app_test` under `test/source/motif_app/` for config path/default serialization, recent database normalization, active database state, and command-line PGN queue parsing.
  - [ ] Keep tests headless; test pure helpers without constructing real windows where possible.
  - [ ] For config tests, use temporary `XDG_CONFIG_HOME` or an injectable config root so tests never write to the developer's real home directory.
  - [ ] Add tests proving malformed config is reported and not silently replaced.
  - [ ] Add tests proving non-PGN, missing, directory, and mixed-case `.PGN` launch paths behave correctly.
  - [ ] Add tests proving scratch is never persisted to recent databases.
  - [ ] Add tests proving a failed open does not replace the previous active database.

- [ ] Task 9: Validate and record results (AC: 11, 12)
  - [ ] Run `cmake --preset=dev && cmake --build build/dev`.
  - [ ] Run `ctest --test-dir build/dev`.
  - [ ] Run `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize`.
  - [ ] Run `ctest --test-dir build/dev-sanitize`.
  - [ ] Apply clang-format to all touched source files before marking the story done.

## Dev Notes

### Scope Boundary

This story starts Epic 7 Desktop Application by making the database workspace first-class. The allowed outcome is a native Qt desktop shell where the user can create, open, reopen, or use scratch as the active database context. Do not implement the chessboard, game list browser, import execution/progress dialog, position search panel, engine panel, or copy-games-between-databases workflow here; those belong to later stories.

The application may show placeholders or disabled navigation for later workflows, but those workflows must not start without an active database context.

Epic 4b/4c HTTP work is complete and remains a separate interface boundary. The desktop app should follow the original architecture for now: Qt shell over direct C++ module APIs via worker threads where needed. This story should not make the Qt app consume the HTTP server.

### Database Workspace Contract

A motif-chess database is a named, portable directory bundle:

```text
<db-name>/
  games.db
  positions.duckdb
  manifest.json
```

Use `motif::db::database_manager::create` and `motif::db::database_manager::open` as the only persistent bundle lifecycle APIs. Use `motif::db::scratch_base::instance()` for the temporary scratch database. The app layer owns active database selection and recent-database persistence; `motif_db` remains Qt-free and unaware of UI state.

Active database state should be explicit:

- `none`: workspace must prompt before database-backed workflows run.
- `persistent`: owned `database_manager`, visible name/path, eligible for recent databases.
- `scratch`: non-persistent temporary database, visible label, never added to recent databases.

Failed open/create operations must leave the previous active database untouched. This matters when a user tries to open a corrupt or stale recent database while already working in another database.

### Dependency and Approval Guardrail

Current repository state has no `source/motif/app/` and no root `find_package(Qt6 ...)`. `flake.nix` also does not list Qt in `buildInputs`. Project rules require explicit Bogdan approval before modifying `flake.nix`. They also require dependency manifests to remain aligned across Nix/vcpkg, while `vcpkg.json` is normally updated only at end-of-epic; treat that as a human policy gate, not something for the dev agent to infer.

If Qt is unavailable in the active build environment, stop before dependency edits and request approval plus manifest instructions. Do not use FetchContent, ExternalProject, vendored Qt, manual include paths, or downstream CMake hacks.

### Architecture Compliance

- `motif_app` is the only module that may include Qt headers or link Qt libraries.
- Keep `motif_db`, `motif_import`, `motif_search`, `motif_engine`, and `motif_http` Qt-free.
- All identifiers, including Qt signals/slots, use `lower_snake_case`.
- Use `fmt::format`/`fmt::print` for formatting and console output; do not introduce `std::format`, `std::cout`, `std::cerr`, `std::ostringstream`, or `std::to_string`.
- Public fallible helpers should return `tl::expected<T, motif::app::error_code>`.
- Use `#pragma once` in every new header and full include paths such as `"motif/app/app_config.hpp"`.

### Suggested File Structure

```text
source/motif/app/
  CMakeLists.txt
  main.cpp
  app_config.hpp
  app_config.cpp
  database_workspace.hpp
  database_workspace.cpp
  pgn_launch_queue.hpp
  pgn_launch_queue.cpp
  error.hpp

test/source/motif_app/
  app_config_test.cpp
  database_workspace_test.cpp
  pgn_launch_queue_test.cpp

packaging/linux/
  motif-chess.desktop
```

Use this as guidance, not a hard mandate. Keep implementation small and avoid abstractions that only later UI stories need.

### Config Contract

The user-facing config path is exactly `~/.config/motif-chess/config.json` unless `XDG_CONFIG_HOME` is set, in which case use `$XDG_CONFIG_HOME/motif-chess/config.json`. Prefer a small pure C++ config helper over `QSettings`; Qt's native settings format creates `.conf` files under organization/application paths, which does not satisfy the required JSON contract.

Suggested defaults:

```json
{
  "database_directory": "",
  "recent_databases": [],
  "engine_paths": [],
  "ui_preferences": {
    "board_theme": "classic",
    "piece_set": "standard",
    "prefetch_depth": 5
  }
}
```

Keep `prefetch_depth` aligned with the existing opening-tree default of 5. Do not store config inside any database bundle; database portability depends on config and data remaining separate.

### Qt/CMake Guidance

Use current Qt 6 CMake patterns:

- `find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)`
- `qt_standard_project_setup()` only if it is called before Qt targets in the relevant directory scope.
- `qt_add_executable(motif_chess ...)` for the desktop executable.

CMake 3.25 is already in use, so Qt target finalization can be automatic; avoid unnecessary `MANUAL_FINALIZATION`.

Use a minimal `QMainWindow` shell with a database workspace/start screen. Later stories can introduce dock widgets, QML, charts, and model/view data panels.

### Linux `.pgn` Association Guidance

Freedesktop desktop entries advertise file support with the `MimeType` key. Use `application/x-chess-pgn;` and an `Exec` line with `%F` so file managers pass one or more selected files to the app.

Do not try to force motif-chess as the user's default handler from inside the application. Default application choice belongs to the desktop environment/user through `xdg-mime` or `mimeapps.list`. This story only needs to install/register a correct desktop entry and document a smoke test.

### Testing Guidance

Most app-shell logic should be testable without showing a window:

- Config path resolution with and without `XDG_CONFIG_HOME`.
- Default config creation in a temp directory.
- Round-trip serialization with glaze.
- Malformed config preservation/reporting.
- Recent database ordering, deduplication, unavailable-path handling, and removal.
- Active database state transitions: none to persistent, persistent to failed-open unchanged, persistent to scratch, scratch excluded from recents.
- PGN launch queue parsing for valid, missing, directory, non-PGN, and mixed-case `.PGN` paths.

Only add Qt GUI tests if the existing test environment can run them reliably headless. If GUI runtime tests require display setup, record that as a deferred testing note and cover the pure logic thoroughly.

### Previous Story Intelligence

- Epic 1 already implemented the backend bundle concept through `database_manager`, `manifest`, `position_store`, and `scratch_base`.
- Epic 4b/4c established strict adapter boundaries and avoided Qt in backend modules. Preserve that discipline.
- `motif_http` exists as a separate executable target (`motif_http_server`) and should not be repurposed as the desktop app.
- Recent stories emphasize bounded lifecycle management and non-blocking behavior. Keep startup work tiny; avoid opening large databases or starting imports before the first window appears.
- The OpenAPI spec and HTTP contract are useful for future frontends, but this Qt shell should not route local UI operations through HTTP unless a separate architecture decision is made.

### References

- [Source: `_bmad-output/planning-artifacts/epics.md` - Epic 7 and Story 7.1]
- [Source: `_bmad-output/planning-artifacts/prd.md` - Desktop Application Requirements, Database Management, Configuration, System Integration]
- [Source: `_bmad-output/planning-artifacts/architecture.md` - Database Model, Qt boundary, `motif_app`, QThread guidance, project structure]
- [Source: `_bmad-output/implementation-artifacts/1-4-database-manager-schema-lifecycle-manifest.md` - persistent bundle lifecycle]
- [Source: `_bmad-output/implementation-artifacts/1-5-duckdb-position-schema-scratch-base-rebuild.md` - DuckDB store and scratch base]
- [Source: `specs/004-qt-gui/spec.md` - Qt GUI scope and panel roadmap]
- [Source: `CONVENTIONS.md` - dependency approval, fmt, naming, module boundaries, testing checklist]
- [Source: `_bmad-output/project-context.md` - technology stack and agent rules; note that the "do not scaffold Qt until spec 004" guard is now satisfied by starting Epic 7]
- [External: Qt 6 `qt_add_executable` and `qt_standard_project_setup` documentation]
- [External: freedesktop.org Desktop Entry and MIME Applications specifications]

## Dev Agent Record

### Agent Model Used

{{agent_model_name_version}}

### Debug Log References

### Completion Notes List

### File List
