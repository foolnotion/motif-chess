# 001 — Database Schema and CRUD

## Overview

Implement the two-layer storage system: SQLite for game metadata and move
blobs, DuckDB for position indexing and opening statistics. Provide CRUD
operations for games, players, and events.

## Background

Most open-source chess databases use a single storage backend, which forces
a compromise between metadata queries and position lookups. Motif splits
these concerns: SQLite handles structured metadata with mature indexing,
while DuckDB handles columnar analytics over positions and statistics.

## SQLite Schema

    CREATE TABLE Players (
        id       INTEGER PRIMARY KEY,
        name     TEXT NOT NULL,
        elo      INTEGER,
        title    TEXT,
        country  TEXT
    );

    CREATE TABLE Events (
        id    INTEGER PRIMARY KEY,
        name  TEXT NOT NULL,
        site  TEXT,
        date  TEXT
    );

    CREATE TABLE Games (
        id          INTEGER PRIMARY KEY,
        white_id    INTEGER REFERENCES Players(id),
        black_id    INTEGER REFERENCES Players(id),
        event_id    INTEGER REFERENCES Events(id),
        date        TEXT,
        result      TEXT,
        eco         TEXT,
        ply_count   INTEGER,
        moves_blob  BLOB
    );

    CREATE TABLE Tags (
        game_id  INTEGER REFERENCES Games(id),
        key      TEXT,
        value    TEXT
    );

    CREATE INDEX idx_games_white  ON Games(white_id);
    CREATE INDEX idx_games_black  ON Games(black_id);
    CREATE INDEX idx_games_eco    ON Games(eco);
    CREATE INDEX idx_games_date   ON Games(date);
    CREATE INDEX idx_games_event  ON Games(event_id);
    CREATE INDEX idx_players_name ON Players(name);

## DuckDB Schema

    CREATE TABLE PositionIndex (
        zobrist_hash  UBIGINT,
        game_id       INTEGER,
        ply           SMALLINT
    );

    CREATE TABLE OpeningStats (
        zobrist_hash  UBIGINT,
        move_san      VARCHAR,
        white_wins    INTEGER,
        draws         INTEGER,
        black_wins    INTEGER,
        avg_elo       SMALLINT,
        total_games   INTEGER
    );

## Move Encoding

16-bit per move (CPW convention, implemented in `chesslib::codec`):

    Bits 15-10: from square  (0-63, valid_index remapped from 0x88)
    Bits  9- 4: to square    (0-63, valid_index remapped from 0x88)
    Bits  3- 0: flags

    Flags:
      0000  quiet                1000  knight promotion
      0001  double pawn push     1001  bishop promotion
      0010  kingside castle      1010  rook promotion
      0011  queenside castle     1011  queen promotion
      0100  capture              1100  knight promotion + capture
      0101  en passant           1101  bishop promotion + capture
      0110  (reserved)           1110  rook promotion + capture
      0111  (reserved)           1111  queen promotion + capture

    Decode helpers:
      flags & 0x8  -> promotion
      flags & 0x4  -> capture
      flags & 0x3  -> promo piece (0=N, 1=B, 2=R, 3=Q)

Stored as moves_blob in the Games table. ~80 bytes/game average.

## Public API

    namespace database {

    struct GameMetadata {
        std::string white, black, event, site, date, result, eco;
        int white_elo{0}, black_elo{0};
        std::vector<std::pair<std::string, std::string>> extra_tags;
    };

    struct GameRef {
        int64_t game_id;
        std::string white, black, result, date, eco;
        int white_elo, black_elo;
    };

    struct MoveStats {
        std::string move_san;
        int white_wins, draws, black_wins, total_games;
        int avg_elo;
    };

    class GameStore {
    public:
        explicit GameStore(std::filesystem::path const& db_dir);

        auto insert_game(GameMetadata const& meta,
                         std::span<uint16_t const> encoded_moves)
            -> int64_t;

        auto get_game(int64_t game_id)
            -> tl::expected<Game, db_error>;

        auto delete_game(int64_t game_id) -> bool;

        auto game_exists(GameMetadata const& meta,
                         std::span<uint16_t const> moves) -> bool;
    };

    class PositionIndex {
    public:
        explicit PositionIndex(std::filesystem::path const& db_path);

        void record_position(uint64_t zobrist,
                             int64_t game_id, int ply);

        void update_opening_stats(uint64_t zobrist,
                                  std::string_view move_san,
                                  pgn::result result, int avg_elo);

        auto search_position(uint64_t zobrist)
            -> std::vector<GameRef>;

        auto get_stats(uint64_t zobrist)
            -> std::vector<MoveStats>;
    };

    }

## Configuration

- SQLite opens in WAL mode for concurrent read/write during import.
- DuckDB opens in single-writer mode (batched inserts during import).
- Both databases live in the same directory, passed to constructors.

## Prerequisites

- chesslib: 16-bit move encode/decode functions (see design doc).

## Dependencies

- SQLite (provided by Nix / vcpkg)
- DuckDB (provided by Nix / vcpkg)
- tl::expected (provided by Nix / vcpkg)

## Acceptance Criteria

- [ ] Insert and retrieve a game round-trip: metadata and moves match.
- [ ] Player and event deduplication: inserting same player name twice
      returns the same player_id.
- [ ] Game deduplication: inserting the same game twice is detected.
- [ ] 1M games insert in under 60 s (SQLite, WAL mode, batched).
- [ ] DuckDB position index insert for 1M games x 40 avg ply under 60 s.
- [ ] Database files are inspectable with sqlite3 and duckdb CLI tools.
- [ ] Delete game removes all associated data (tags, positions, stats).
- [ ] Concurrent read during write does not block or corrupt.
- [ ] Schema creation is idempotent (CREATE IF NOT EXISTS).
- [ ] clang-tidy reports no new warnings.
