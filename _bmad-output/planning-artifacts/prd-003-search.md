---
stepsCompleted: ['step-01-init', 'step-02-discovery', 'step-03-success',
  'step-04-journeys', 'step-05-domain', 'step-09-functional',
  'step-10-nonfunctional', 'step-12-complete']
inputDocuments:
  - 'specs/003-search/spec.md'
  - '_bmad-output/planning-artifacts/prd.md'
  - '_bmad-output/project-context.md'
  - 'source/motif/search/opening_stats.hpp'
  - 'source/motif/search/opening_tree.hpp'
  - 'source/motif/search/position_search.hpp'
workflowType: 'prd'
completedAt: '2026-05-06'
---

# Product Requirements Document — motif-chess: Search & Filtered Opening Explorer (spec 003)

**Author:** Bogdan
**Date:** 2026-05-06

## Executive Summary

Spec 003 delivers the search and filtering layer that makes the motif-chess
database useful for two concrete preparation workflows: opponent research and
opening study. Both workflows converge on the same core capability — a
**filtered opening explorer** where statistics reflect only the games matching
the current query, not the entire database.

The central insight driving this design: aggregate win/draw/loss rates are
often misleading. A position may score poorly overall because it is difficult
to play correctly below 2400 Elo, yet score excellently at the top level. A
flat aggregate hides this entirely. Exposing win/draw/loss as a **continuous
distribution over Elo** — smoothed in the visualization layer — reveals
whether a position is objectively sound, practically demanding, or simply
unpopular at lower levels. This is information no current chess tool surfaces.

The secondary deliverable is a **filterable game list**: a paginated,
browsable view of games matching the current filter, used primarily to get a
feel for an opponent's playing style before a round.

The filter state is shared between the game list and the opening explorer,
with a **sync toggle** allowing them to be coupled or used independently.

## Success Criteria

### User Success

- A player can look up an opponent by name, see their games as White or
  Black, browse the game list for style clues, and navigate an opening
  explorer that reflects only that opponent's repertoire — all within 30
  seconds of opening the panel.
- A player studying an opening can apply ELO and result filters and
  immediately see how the continuation statistics change, including the
  continuous ELO-score distribution that reveals which moves are
  objectively strong vs. practically demanding.
- The ELO-weighted ranking surfaces preparation-relevant moves that raw
  frequency counts would bury — a rare move that only grandmasters play and
  score well with is ranked above a common move that loses at top level.

### Technical Success

- Filtered opening stats queries complete in under 200ms for a 4M-game
  corpus at any position with any active filter combination.
- Filtered game list queries with pagination complete in under 100ms.
- The per-continuation ELO distribution data (used for continuous
  visualization) is returned in a single query per position, not N queries
  per continuation.
- All filter-aware query paths are exercised by tests against a real
  SQLite + DuckDB instance (no mocking).
- Zero new clang-tidy or cppcheck warnings.
- All code clean under ASan + UBSan.

## User Journeys

### Journey 1: Opponent Preparation (Primary)

**The situation:** Bogdan has just seen the pairings for round 4. He is
playing White against a player he has not faced before. He has 45 minutes
before the round starts.

He opens motif-chess and navigates to the Search panel. He types the
opponent's name into the player filter and sets color to Black. The game
list populates: 87 games found. He scans through — the opponent plays the
Sicilian Najdorf almost exclusively against 1.e4. He clicks into the
opening explorer: filtered to this opponent's Black games, it shows the
branching point where the opponent almost always plays 6...e5 rather than
6...e6. He has a prepared line against 6...e6 but not 6...e5.

He switches off the sync toggle, keeps the opening explorer filtered to the
opponent's games, and uses the game list independently to find their most
recent games. He picks three to review the endgame technique. He has enough
to make a decision about his opening choice.

**Capabilities required:** Player name filter, color filter, game list,
filtered opening explorer, sync toggle.

---

### Journey 2: Opening Study (Primary)

**The situation:** Bogdan is preparing the Advance French (1.e4 e6 2.d4 d5
3.e5). He navigates to the position after 3.e5 using the board. He opens
the opening explorer for this position against the full reference database.

He applies an ELO filter: min 2400 for both colors. The continuation
statistics update — the move frequency rankings shift. He notices that 3...c5
has a higher ELO-weighted score than 3...Nf6, even though 3...Nf6 is more
common. He hovers over the ELO distribution for 3...c5: the curve shows
Black scoring well above 50% in games between 2400–2700, and even better
above 2700. In lower-rated games the score is near 50/50. This tells him
3...c5 is the theoretically more demanding but objectively stronger try —
worth preparing.

He removes the ELO filter and adds a result filter (Black wins only) to see
which continuations Black most commonly uses when they win. The opening
explorer and game list both update because sync is on.

**Capabilities required:** Position-based entry into opening explorer,
ELO range filter, result filter, ELO-weighted continuation ranking,
continuous ELO distribution visualization data, sync toggle.

## Domain-Specific Requirements

### Filter Model

The filter is a first-class value in the search layer. It is defined as:

```
struct search_filter {
    std::optional<std::string> player_name;   // white, black, or either
    std::optional<player_color> player_color; // white | black | either
    std::optional<int> min_elo;               // both sides >= min_elo
    std::optional<int> max_elo;               // both sides <= max_elo
    std::optional<std::string> result;        // "1-0" | "0-1" | "1/2-1/2"
    std::optional<std::string> eco_prefix;    // prefix match, e.g. "B9"
    int offset {0};
    int limit  {100};
};
```

An empty filter (all fields nullopt) is valid and equivalent to "all games."
The backend must not treat an empty filter as an error.

### ELO-Weighted Continuation Score

The weighted score for a continuation is a scalar ranking signal that gives
more weight to outcomes in higher-rated games. The formula is to be finalized
during implementation, but the intent is:

- A win in a 2700-average game contributes more to the score than a win in
  a 1500-average game.
- The score is used only for **ranking** continuations relative to each
  other within a position — it is not displayed as a percentage or absolute
  value to the user.
- The existing frequency-based sort remains available as an alternative sort
  key; the user can toggle between "by frequency" and "by ELO-weighted score."

A reasonable starting formula: for each game g reaching this position and
playing move m, contribution = result_value(g) × avg_elo(g), where
result_value is +1 (side to move wins), 0 (draw), −1 (side to move loses).
Score = Σ contributions / Σ avg_elo(g). This is a weighted average outcome
normalized by Elo. Evaluate and adjust during implementation.

### Continuous ELO Distribution

For each continuation at a position, the backend returns a sequence of
(avg_elo_bucket, white_wins, draws, black_wins, game_count) tuples at a
fixed bucket width (target: 25 Elo). The Qt visualization layer applies
smoothing (moving average or kernel) and renders the result as overlapping
win/draw/loss curves over the Elo axis.

The backend must return enough data points across the Elo range of actual
games to make smoothing meaningful — empty buckets are included as zeros so
the frontend knows the Elo range spans without gaps.

### Sync Toggle Semantics

When sync is ON: setting a filter in either the game list panel or the
opening explorer panel propagates the filter to the other. Both panels
reflect the same filter state.

When sync is OFF: each panel maintains its own independent filter state.
The toggle persists for the session but is not saved to user configuration.

### Relationship to Existing Opening Explorer

The filtered opening explorer is not a separate component — it is the
**existing opening explorer** (`opening_tree`, `opening_stats`) with
filter parameters threaded through. The backend API for opening stats and
tree traversal gains an optional `search_filter` parameter. When the filter
is empty, behavior is identical to today. This is an extension, not a
replacement.

### Player Name Search Semantics

- Case-insensitive, partial match (prefix or substring — to be decided
  during implementation, but must handle "Carls" matching "Carlsen, Magnus").
- Matches against both White and Black player fields.
- When `player_color` is set, restricts to games where the matching player
  was that color.
- Performance: player name lookup hits SQLite (the metadata store), not
  DuckDB. The result is a set of game IDs that is then intersected with the
  position filter in DuckDB when computing opening stats.

## Functional Requirements

### Game Search & List

- **FR01:** The user can filter games by player name (case-insensitive
  partial match) with optional color restriction (White, Black, or either).
- **FR02:** The user can filter games by ELO range (min and/or max average
  game Elo, both endpoints inclusive). Games without Elo data are excluded
  when an Elo filter is active.
- **FR03:** The user can filter games by result ("1-0", "0-1", "1/2-1/2").
- **FR04:** The user can filter games by ECO prefix (e.g., "B9" matches
  B90–B99).
- **FR05:** Filter fields can be combined arbitrarily; all active filters
  are ANDed.
- **FR06:** An empty filter (no fields set) returns all games, paginated.
- **FR07:** The game list is paginated with configurable limit (default 100,
  max 500) and offset.
- **FR08:** The game list returns total_count reflecting the full result set,
  not just the current page.
- **FR09:** The game list displays: White player, Black player, Result,
  White Elo, Black Elo, ECO, Date, Event.
- **FR10:** Selecting a game in the list navigates the board to that game.

### Filtered Opening Explorer

- **FR11:** The opening explorer accepts an optional search_filter and
  computes all statistics (frequency, white_wins, draws, black_wins,
  average_white_elo, average_black_elo) over the filtered game set only.
- **FR12:** When no filter is active, the opening explorer behaves
  identically to the current implementation.
- **FR13:** Each continuation exposes an ELO-weighted score (scalar,
  backend-computed) usable as a sort key in addition to frequency.
- **FR14:** The user can toggle continuation sort between "by frequency"
  and "by ELO-weighted score."
- **FR15:** For each continuation, the backend returns per-bucket ELO
  distribution data: (avg_elo_bucket, white_wins, draws, black_wins,
  game_count) at 25-Elo bucket width, covering the full Elo range of
  matching games (empty buckets included as zeros).
- **FR16:** The opening tree (`opening_tree::open` and `expand`) accept the
  search filter and pass it through to all underlying queries.

### Sync Toggle

- **FR17:** A sync toggle controls whether the game list and opening
  explorer share a single filter state or maintain independent filters.
- **FR18:** When sync is ON, any filter change in either panel immediately
  propagates to the other and triggers a refresh.
- **FR19:** When sync is OFF, panels refresh only when their own filter
  changes.
- **FR20:** The sync toggle defaults to ON at session start.

### HTTP API (existing server)

- **FR21:** `GET /api/games` accepts query parameters corresponding to
  `search_filter` fields (player, color, min_elo, max_elo, result, eco,
  offset, limit) and returns the filtered game list with total_count.
- **FR22:** `GET /api/positions/{hash}/stats` accepts filter query
  parameters and returns filtered opening statistics including the
  ELO-weighted score and ELO distribution buckets per continuation.
- **FR23:** `GET /api/positions/{hash}/tree` accepts filter query parameters
  and returns a filtered opening tree up to prefetch_depth.

## Non-Functional Requirements

### Performance

- **NFR01:** Filtered game list query (any filter combination): < 100ms
  for a 4M-game corpus.
- **NFR02:** Filtered opening stats at any position (including ELO
  distribution buckets): < 200ms for a 4M-game corpus.
- **NFR03:** Filtered opening tree open (prefetch_depth = 3): < 500ms for
  a 4M-game corpus.
- **NFR04:** ELO distribution bucket data is returned in a single DuckDB
  query per position, not one query per continuation.
- **NFR05:** Player name search (SQLite) completes in under 50ms and
  returns a game ID set usable for DuckDB intersection.

### Correctness

- **NFR06:** Filtered stats are exactly equal to manually counting results
  from the matching game set — verified by tests against a known dataset.
- **NFR07:** ELO-weighted scores produce a strict total order (no ties
  broken arbitrarily) — ties broken by frequency, then SAN alphabetically.
- **NFR08:** Empty filter and absent filter produce identical results.

### Code Quality

- **NFR09:** All new public API functions have at least one Catch2 v3 test.
- **NFR10:** Tests run against real SQLite + DuckDB instances (no mocks).
- **NFR11:** Zero new clang-tidy or cppcheck warnings.
- **NFR12:** All code passes ASan + UBSan under `cmake --preset=dev-sanitize`.

## Out of Scope

- **Multi-database support:** Copying search results to a new database
  (noted as a useful future capability; not in this spec).
- **Date range filter:** Useful but lower priority; can be added as a
  follow-on without architectural changes.
- **Full-text search on comments/annotations:** Not needed for MVP.
- **Saved searches / named filters:** Session-only; persistence deferred.
- **Statistical analysis panels** (performance by opening, phase, time
  pressure): Phase 2 per the overall PRD.
