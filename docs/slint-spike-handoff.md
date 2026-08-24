# Slint Spike Decision Handoff

## Purpose

Record the completed evaluation of Slint as a replacement for the Qt
Quick/KDDockWidgets presentation layer. The spike is a fixed, resizable
workspace and intentionally does not implement arbitrary docking.

The decisive evaluation surface is the game table: real database data, large
row counts, selection, keyboard navigation, sort callbacks, column resizing,
context-menu behavior, and scrolling. A chessboard is secondary because its
custom rendering is portable across toolkits.

## Upstream Dependency

Slint is packaged in the foolnotion Nix overlay and has been pushed:

- Repository: `~/src/nur-pkg`
- Runtime-fix commit: `e8447985d5428d55e0fc0715fb41454cfd6ea139`
- Package: `slint` (Slint C++ SDK 1.17.1)

The package builds the upstream C++ SDK from source with vendored Cargo
dependencies. It exports `Slint::Slint`, `Slint::slint-compiler`,
`slint_target_sources(...)`, headers, `libslint_cpp.so`, and
`SlintConfig.cmake`.

It uses the winit/FemtoVG desktop backend and deliberately disables Slint's Qt
backend, interpreter, and live preview. It was verified by both:

```sh
cd ~/src/nur-pkg
nix build --no-link .#slint
```

and a separate CMake/Ninja consumer using `find_package(Slint CONFIG REQUIRED)`,
`slint_target_sources(...)`, and `Slint::Slint` under Clang 21.

## Implemented Spike Integration

The spike updates the `foolnotion` lock, adds Slint only to app-oriented Linux
inputs, and provides a default-OFF root CMake option:

```cmake
option(motif_ENABLE_SLINT_SPIKE "Build the experimental Slint game-browser spike" OFF)
```

The spike subdirectory is gated on that option and does not change
`motif_ENABLE_APP` or current Qt/QML targets.

Upstream vcpkg has no Slint port as of 2026-08-24. `vcpkg.json` therefore
remains unchanged, and Windows support is blocked pending an approved package.

No `FetchContent`, `ExternalProject`, vendored Slint source, or ad hoc CMake
lookup workaround is allowed.

## Spike Architecture

Add a parallel UI target, isolated from the existing Qt app:

```text
source/motif/slint_spike/
  CMakeLists.txt
  main.cpp
  game_browser_presenter.hpp
  game_browser_presenter.cpp
  ui/game_browser.slint
```

Keep Slint types and generated APIs in this target or a narrow adapter only.
Do not include Slint headers in `motif_db`, `motif_import`, `motif_search`, or
`motif_chess`.

Reuse existing toolkit-free code:

- `motif::db::database_manager` and `game_store` to open an existing database
  bundle and call `find_games(search_filter)` / `get(game_id)`.
- `motif::app::game_navigator` for SAN move lists, FEN, and move navigation.
- `motif::db::{game_list_entry, game, game_id, search_filter}`.

Do not reuse Qt adapters:

- `board_model`
- `game_list_model`
- `position_search_model`
- `opening_stats_model`
- `workspace_controller`
- KDDockWidgets view factory or Wayland bridge

For the initial spike, open a bundle passed as `argv[1]` directly with
`database_manager::open()`. This avoids recent-database persistence and import
workflow work. Load at most 500 rows synchronously.

## Required UI

Use a fixed split-pane layout:

- Left: game table/list backed by real `game_list_entry` data.
- Right: selected game metadata, SAN moves, current FEN, previous/next move
  actions, and visible error text.

Implement row selection through a Slint callback into the presenter. Selecting
a game must load it through `game_store::get()`, then populate `game_navigator`.

Do not build arbitrary docking, import UI, settings persistence, full styling,
or a complete board in the first pass. Once the table seam works, a board may
render structured state from C++ instead of parsing FEN in UI code.

## Tests And Decision

Add headless tests for the toolkit-neutral presenter using a real scratch
database. Assert real game rows, selection, SAN, FEN changes after advancing,
and safe error presentation. Do not require a graphical display in CI.

Run the normal build/test gates plus an app-enabled Slint spike build. Keep all
UI and business-logic changes in separate PRs. The current PR numbers are to
be repurposed only after this spike yields a go/no-go decision.

The presenter tests pass against real scratch databases, including navigation
through every ply of a 300-ply game. Human checks found the 500-row table and
20-, 80-, 160-, and 300-ply games very responsive through waypipe with
`SLINT_BACKEND=winit-software`. The packaged executable starts without a
manual `LD_LIBRARY_PATH` after the overlay runtime fix.

**Decision: go.** Slint is the production presentation-layer direction. Keep
the Qt application working while production Slint functionality is delivered
in vertical slices. Do not turn this disposable synchronous spike into the
production shell. The canonical migration contract is `spec-008` in the
`motif-chess-meta` repository, and the accepted decision is `decision-050`.
