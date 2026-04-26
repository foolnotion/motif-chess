# Story 4.1: Application Shell, Config & .pgn File Handler

Status: ready-for-dev

<!-- Ultimate context engine analysis completed - comprehensive developer guide created -->

## Story

As a user,
I want the application to start in under 3 seconds, remember my recent databases and preferences, and register as my system's `.pgn` file handler,
so that motif-chess feels like a native, well-integrated desktop tool from first launch.

## Acceptance Criteria

1. **Given** the application is launched for the first time
   **When** the main window appears
   **Then** startup time from process start to usable window is under 3 seconds
   **And** `~/.config/motif-chess/config.json` is created with defaults for database directory, empty recent database list, default engine paths, and default UI preferences.

2. **Given** the user has previously opened databases
   **When** the application starts
   **Then** the recent databases list is populated from `config.json`
   **And** config is never stored inside database bundle files.

3. **Given** the application is launched with one or more `.pgn` file paths as command-line arguments
   **When** the main window appears
   **Then** those paths are captured as a queued import list for later import UI handling
   **And** missing or non-regular paths are reported through the app shell without crashing.

4. **Given** the application is installed on Linux
   **When** the desktop entry is registered
   **Then** double-clicking a `.pgn` file in a file manager opens motif-chess with that file path passed to the application
   **And** the desktop file advertises `application/x-chess-pgn`.

5. **Given** the implementation is reviewed
   **When** includes and target links are inspected
   **Then** all Qt code is confined to `motif_app`
   **And** no Qt headers appear in `motif_db`, `motif_import`, `motif_search`, `motif_engine`, or `motif_http`.

6. **Given** all changes are implemented
   **When** `cmake --preset=dev && cmake --build build/dev && ctest --test-dir build/dev` is run
   **Then** all tests pass with zero new clang-tidy or cppcheck warnings.

7. **Given** all changes are implemented
   **When** `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize && ctest --test-dir build/dev-sanitize` is run
   **Then** all tests pass with zero ASan/UBSan violations.

## Tasks / Subtasks

- [ ] Task 1: Resolve Qt dependency gate before code edits (AC: 5, 6, 7)
  - [ ] Confirm whether Qt 6 is already available to CMake in the active Nix shell.
  - [ ] If Qt 6 is not already available, stop and ask Bogdan for explicit approval before modifying `flake.nix` or `vcpkg.json`.
  - [ ] Resolve the manifest policy explicitly with Bogdan before edits: project rules require dependency changes to stay aligned across Nix/vcpkg, while `vcpkg.json` is normally updated only at end-of-epic.
  - [ ] For this story, prefer the smallest Qt surface: `Qt6::Core`, `Qt6::Gui`, and `Qt6::Widgets` from Qt base. Do not add Qt Quick, QML, or Charts until a later story actually needs them.

- [ ] Task 2: Add the `motif_app` target and minimal desktop executable (AC: 1, 5, 6)
  - [ ] Create `source/motif/app/`.
  - [ ] Add `source/motif/app/CMakeLists.txt` defining the app target and linking Qt only in this module.
  - [ ] Add `source/motif/app/main.cpp` with `QApplication`, project metadata, startup timing, command-line path capture, and a minimal `QMainWindow`.
  - [ ] Add `add_subdirectory(source/motif/app)` in the root `CMakeLists.txt`.
  - [ ] Keep the existing `motif_http_server` executable unchanged.

- [ ] Task 3: Implement app config JSON load/create/save (AC: 1, 2)
  - [ ] Define an `app_config` struct in `motif::app` with `database_directory`, `recent_databases`, `engine_paths`, and `ui_preferences`.
  - [ ] Store config at `${XDG_CONFIG_HOME:-$HOME/.config}/motif-chess/config.json`.
  - [ ] Use glaze JSON serialization, not `QSettings`, because the product contract requires a JSON file at an exact path.
  - [ ] Create parent directories on first launch.
  - [ ] On missing config, write defaults and continue.
  - [ ] On malformed config, return a typed app error and show a non-crashing shell-level error state; do not overwrite the malformed file silently.

- [ ] Task 4: Capture `.pgn` launch arguments for queued import (AC: 3)
  - [ ] Parse command-line positional arguments after Qt has consumed its own options.
  - [ ] Accept one or more paths ending in `.pgn` case-insensitively.
  - [ ] Resolve each path to an absolute path when possible.
  - [ ] Store valid paths in an in-memory queued import list owned by the app shell.
  - [ ] Surface invalid paths in the shell status area or startup diagnostics without throwing.
  - [ ] Do not start imports in this story; actual import UI/progress belongs to Story 4.4.

- [ ] Task 5: Add Linux desktop entry packaging artifacts (AC: 4)
  - [ ] Add a desktop entry file under an appropriate packaging/resources path, e.g. `packaging/linux/motif-chess.desktop`.
  - [ ] Include `Type=Application`, `Name=motif-chess`, `Exec=motif_chess %F`, `MimeType=application/x-chess-pgn;`, `Terminal=false`, and relevant categories.
  - [ ] Add CMake install rules for the desktop entry if install rules are active.
  - [ ] Document the local registration smoke test: install/copy the desktop file, run `update-desktop-database`, then use `xdg-mime query default application/x-chess-pgn`.

- [ ] Task 6: Add focused tests (AC: 1, 2, 3, 6, 7)
  - [ ] Add `motif_app_test` under `test/source/motif_app/` for config path/default serialization and command-line PGN queue parsing.
  - [ ] Keep tests headless; test pure helpers without constructing real windows where possible.
  - [ ] For config tests, use temporary `XDG_CONFIG_HOME` or an injectable config root so tests never write to the developer's real home directory.
  - [ ] Add a test proving malformed config is reported and not silently replaced.
  - [ ] Add a test proving non-PGN and missing paths are rejected without crashing.

- [ ] Task 7: Validate and record results (AC: 6, 7)
  - [ ] Run `cmake --preset=dev && cmake --build build/dev`.
  - [ ] Run `ctest --test-dir build/dev`.
  - [ ] Run `cmake --preset=dev-sanitize && cmake --build build/dev-sanitize`.
  - [ ] Run `ctest --test-dir build/dev-sanitize`.
  - [ ] Apply clang-format to all touched source files before marking the story done.

## Dev Notes

### Scope Boundary

This story starts Epic 4 Desktop Application. The allowed outcome is a native Qt desktop shell with config persistence and Linux `.pgn` file-association artifacts. Do not implement the chessboard, game list, import dialog, position search panel, or engine panel here; those are later Epic 4 stories.

Epic 4b/4c HTTP work is complete and remains a separate interface boundary. The desktop app should follow the original architecture for now: Qt shell over direct C++ module APIs via worker threads where needed. This story should not make the Qt app consume the HTTP server.

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

```
source/motif/app/
  CMakeLists.txt
  main.cpp
  app_config.hpp
  app_config.cpp
  error.hpp
  pgn_launch_queue.hpp
  pgn_launch_queue.cpp

test/source/motif_app/
  app_config_test.cpp
  pgn_launch_queue_test.cpp

packaging/linux/
  motif-chess.desktop
```

Use this as guidance, not a hard mandate. Keep the implementation small and avoid abstractions that will only be needed by later UI stories.

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
- CMake 3.25 is already in use, so Qt target finalization can be automatic; avoid unnecessary `MANUAL_FINALIZATION`.

Use a minimal `QMainWindow` shell. Later stories can introduce dock widgets, QML, charts, and model/view data panels.

### Linux `.pgn` Association Guidance

Freedesktop desktop entries advertise file support with the `MimeType` key. Use `application/x-chess-pgn;` and an `Exec` line with `%F` so file managers pass one or more selected files to the app.

Do not try to force motif-chess as the user's default handler from inside the application. Default application choice belongs to the desktop environment/user through `xdg-mime` or `mimeapps.list`. This story only needs to install/register a correct desktop entry and document a smoke test.

### Testing Guidance

Most app-shell logic should be testable without showing a window:

- Config path resolution with and without `XDG_CONFIG_HOME`.
- Default config creation in a temp directory.
- Round-trip serialization with glaze.
- Malformed config preservation/reporting.
- PGN launch queue parsing for valid, missing, directory, non-PGN, and mixed-case `.PGN` paths.

Only add Qt GUI tests if the existing test environment can run them reliably headless. If GUI runtime tests require display setup, record that as a deferred testing note and cover the pure logic thoroughly.

### Previous Story Intelligence

- Epic 4b/4c established strict adapter boundaries and avoided Qt in backend modules. Preserve that discipline.
- `motif_http` exists as a separate executable target (`motif_http_server`) and should not be repurposed as the desktop app.
- Recent stories emphasize bounded lifecycle management and non-blocking behavior. Keep startup work tiny; avoid opening large databases or starting imports before the first window appears.
- The OpenAPI spec and HTTP contract are useful for future frontends, but this Qt shell should not route local UI operations through HTTP unless a separate architecture decision is made.

### References

- [Source: `_bmad-output/planning-artifacts/epics.md` — Epic 4 and Story 4.1]
- [Source: `_bmad-output/planning-artifacts/prd.md` — Desktop Application Requirements, Configuration, System Integration]
- [Source: `_bmad-output/planning-artifacts/architecture.md` — Qt boundary, `motif_app`, QThread guidance, project structure]
- [Source: `specs/004-qt-gui/spec.md` — Qt GUI scope and panel roadmap]
- [Source: `CONVENTIONS.md` — dependency approval, fmt, naming, module boundaries, testing checklist]
- [Source: `_bmad-output/project-context.md` — technology stack and agent rules; note that the "do not scaffold Qt until spec 004" guard is now satisfied by starting Epic 4]
- [External: Qt 6 `qt_add_executable` and `qt_standard_project_setup` documentation]
- [External: Qt 6 `QStandardPaths`/`QSettings` documentation for XDG config behavior]
- [External: freedesktop.org Desktop Entry and MIME Applications specifications]

## Dev Agent Record

### Agent Model Used

{{agent_model_name_version}}

### Debug Log References

### Completion Notes List

### File List
