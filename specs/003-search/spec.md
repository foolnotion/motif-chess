# 003 — Search

## Overview

Implement metadata search (player, event, date, ECO, result, ELO range),
position search (by Zobrist hash), opening explorer queries, and combined
searches with paginated results.

## Search Types

### Metadata Search (SQLite)

    struct MetadataQuery {
        std::optional<std::string> player_name;    // white or black
        std::optional<std::string> white;
        std::optional<std::string> black;
        std::optional<std::string> event;
        std::optional<std::string> eco_prefix;     // "B90" or "B9"
        std::optional<std::string> result;         // "1-0", "0-1", "1/2-1/2"
        std::optional<std::pair<int,int>> elo_range;
        std::optional<std::pair<std::string,std::string>> date_range;
        int offset{0};
        int limit{100};
    };

### Position Search (DuckDB)

Returns all games reaching a given position at any ply.

### Opening Explorer (DuckDB)

    struct MoveStats {
        std::string move_san;
        int white_wins, draws, black_wins, total_games;
        int avg_elo;
        double white_pct, draw_pct, black_pct;
    };

Returns per-move statistics from a given position.

### Combined Search

Position search results (game IDs from DuckDB) intersected with metadata
filters (SQLite) via IN clause or temp table join.

## Public API

    namespace search {

    struct SearchResult {
        std::vector<database::GameRef> games;
        int64_t total_count;    // total matches before pagination
        bool has_more;
    };

    class SearchEngine {
    public:
        SearchEngine(database::GameStore& store,
                     database::PositionIndex& index);

        auto search_metadata(MetadataQuery const& q) -> SearchResult;
        auto search_position(uint64_t zobrist) -> SearchResult;
        auto search_combined(MetadataQuery const& q, uint64_t zobrist)
            -> SearchResult;
        auto opening_stats(uint64_t zobrist)
            -> std::vector<database::MoveStats>;
    };

    }

## Performance Requirements

- Metadata search: < 100 ms for 1M game database.
- Position search: < 10 ms for 1M game database.
- Opening stats: < 10 ms.
- Combined search: < 200 ms.

## Prerequisites

- Populated database (spec 001 schema + spec 002 imported data).

## Dependencies

- database::GameStore (spec 001)
- database::PositionIndex (spec 001)

## Acceptance Criteria

- [ ] Player name search returns correct results (case-insensitive,
      partial match).
- [ ] ECO prefix search: "B9" matches B90, B91, ..., B99.
- [ ] ELO range search: both endpoints inclusive.
- [ ] Date range search works with partial dates (e.g. "2023").
- [ ] Position search returns all games reaching that position.
- [ ] Opening stats match manual count of results.
- [ ] Combined search correctly intersects position and metadata.
- [ ] Pagination: offset=100, limit=50 returns games 101-150.
- [ ] total_count reflects full result set, not just current page.
- [ ] All performance targets met on 1M game database.
- [ ] Empty query returns error, not full table scan.
- [ ] clang-tidy reports no new warnings.
