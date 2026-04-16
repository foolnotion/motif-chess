# 002 — Import Pipeline

## Overview

Build a multi-threaded pipeline that reads PGN files, parses them via
pgnlib, replays moves through chesslib (computing Zobrist hashes and
encoding moves), and batch-inserts into both SQLite and DuckDB.

## Background

A useful chess database needs millions of games. Importing from PGN is the
primary data ingestion path. The pipeline must be fast (1M games < 120 s),
resilient (malformed games skipped), and safe (interrupted import must not
corrupt the database).

## Pipeline Architecture

    PGN file(s)
        |  mmap or buffered read
        v
    PGN Parser (pgnlib)
        |  -> pgn::game objects
        |  game queue (bounded)
        v
    Worker Pool (N threads)
        |  Per game:
        |    1. parse_san per move (chesslib)
        |    2. Zobrist hash per ply (chesslib)
        |    3. Encode moves to 16-bit (chesslib)
        |    4. Extract metadata
        |  result queue (bounded)
        v
    DB Writer (1 thread)
        - Batch insert SQLite (games + metadata)
        - Batch insert DuckDB (positions + stats)
        - Transaction per batch (1000 games)

## Threading Model

- Parser thread(s): 1 per PGN file (or 1 per chunk for large files).
  Produces pgn::game objects into a bounded queue.
- Worker pool: N threads (default: hardware_concurrency - 2). Each worker
  takes a pgn::game, replays it through a thread-local board, and produces
  encoded output.
- Writer thread: 1 dedicated thread. Consumes encoded games from the
  result queue. Batches inserts using transactions (commit every 1000).

## Error Handling

- Malformed PGN: parser skips the game, logs error with line number
  and file name.
- Invalid SAN during replay: worker logs error with game tags and move
  number, skips the game.
- Database error: writer logs error, attempts to continue.
- All errors collected in a thread-safe log, available after import.

## Resumability

- Before committing each batch, the writer records the byte offset
  in the PGN file.
- On restart, import resumes from the last committed offset.
- Stored in a _import_progress table in SQLite.

## Public API

    namespace importer {

    struct ImportOptions {
        int worker_threads{0};       // 0 = auto-detect
        int batch_size{1000};        // games per transaction
        bool skip_duplicates{true};
        bool verbose{false};
    };

    struct ImportResult {
        int64_t games_imported;
        int64_t games_skipped_malformed;
        int64_t games_skipped_duplicate;
        double elapsed_seconds;
        std::vector<std::string> errors;
    };

    auto import_pgn(std::filesystem::path const& pgn_path,
                    database::GameStore& store,
                    database::PositionIndex& index,
                    ImportOptions const& opts = {})
        -> ImportResult;

    }

## Prerequisites

- chesslib: parse_san, to_san, Zobrist hashing, 16-bit move encoding.
- pgnlib: parse_file returning vector<pgn::game>.

## Dependencies

- pgnlib (flake input)
- chesslib (flake input)
- database::GameStore (spec 001)
- database::PositionIndex (spec 001)
- taskflow (provided by Nix / vcpkg) — optional, for pipeline orchestration

## Acceptance Criteria

- [ ] Import 1M games in under 120 s on reference hardware.
- [ ] All imported positions produce correct Zobrist hashes (spot-check
      1000 random games against standalone chesslib replay).
- [ ] Duplicate games detected and skipped when skip_duplicates is true.
- [ ] Malformed PGN games skipped with error logged; no crash.
- [ ] Interrupted import (kill -9) does not corrupt database.
- [ ] Resumed import does not re-import already committed games.
- [ ] Import of 0-byte file returns immediately with 0 games.
- [ ] Memory usage stays under 1 GB for 1M game import.
- [ ] Worker thread count defaults to hardware_concurrency - 2.
- [ ] ImportResult accurately reports all counts and errors.
- [ ] clang-tidy reports no new warnings.
