# 004 — Qt6 GUI

## Overview

Migrate the user interface from ImGui to Qt6/QML. Build the five core
panels: chessboard, game list, opening explorer, notation panel, and
engine analysis panel.

## Background

ImGui is used during backend development for quick prototyping. The
production UI targets Qt6 with dockable panels, model/view architecture
for large datasets, and real-time engine analysis display.

## UI Components

### 1. Chessboard Widget

- Renders 8x8 board with pieces.
- Drag-and-drop piece movement (legal moves only).
- Highlights: last move, selected square, legal move targets.
- Arrow drawing for engine PV visualization.
- Board flip (white/black perspective).
- Coordinate labels (a-h, 1-8).

### 2. Game List

- QTableView with custom QAbstractTableModel.
- Columns: #, White, Black, Result, ELO(W), ELO(B), ECO, Date, Event.
- Virtual scrolling for 1M+ rows.
- Column sorting (click header).
- Row selection loads game into board and notation panel.
- Context menu: copy PGN, delete game, open in new tab.

### 3. Opening Explorer Panel

- For the current board position, shows:
  - Each move played from this position.
  - Win / Draw / Loss counts and percentages.
  - Horizontal bar chart (green/gray/red).
  - Average ELO and total games.
- Clicking a move plays it on the board and updates the explorer.

### 4. Notation Panel

- Displays moves in standard notation.
- Inline comments, NAGs, recursive variations.
- Click a move to navigate the board to that position.
- Current move highlighted.

### 5. Engine Analysis Panel

- Real-time UCI engine output.
- Columns: Depth, Score (cp or mate), Principal Variation.
- Multi-PV support (configurable 1-5 lines).
- Start/stop analysis buttons.
- Engine selector dropdown.
- Depth limit or time limit controls.

## Layout

    +----------------+--------------------------+
    |                |  Game List               |
    |  Chessboard    |  (sortable, filterable)  |
    |                +--------------------------+
    |                |  Notation Panel          |
    +----------------+  (moves, comments, NAGs) |
    |  Opening       +--------------------------+
    |  Explorer      |  Engine Analysis         |
    |                |  (depth, score, PV)      |
    +----------------+--------------------------+

All panels are QDockWidget — user can rearrange, float, or hide.

## Prerequisites

- ucilib: complete UCI engine wrapper.
- search::SearchEngine (spec 003): metadata and position search.

## Dependencies

- Qt6 base, Qt6 declarative, Qt6 charts (provided by Nix / vcpkg)
- ucilib (flake input)
- search::SearchEngine (spec 003)
- chesslib (flake input)

## Acceptance Criteria

- [ ] Chessboard renders correctly with standard piece set.
- [ ] Drag-and-drop produces legal moves only.
- [ ] Board flips between white and black perspective.
- [ ] Game list displays 1M+ games with smooth scrolling (60 fps).
- [ ] Column sorting works on all columns.
- [ ] Selecting a game loads it into board and notation panel.
- [ ] Opening explorer shows correct stats for current position.
- [ ] Clicking an explorer move plays it on the board.
- [ ] Notation panel shows moves, comments, variations, NAGs.
- [ ] Clicking a notation move navigates the board.
- [ ] Engine analysis panel updates in real-time during analysis.
- [ ] Multi-PV display shows N lines correctly.
- [ ] Start/stop analysis works without UI freeze.
- [ ] All panels are dockable and rearrangeable.
- [ ] Application does not crash on empty database.
- [ ] clang-tidy reports no new warnings on Qt integration code.
