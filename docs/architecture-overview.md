# Architecture Overview

## High-Level Stack

Motif Chess is split into a backend server and a web frontend.

Backend:

- C++23
- SQLite for durable game metadata and move blobs
- Immutable position postings and opening-tree sidecars for position/statistics queries
- HTTP API with SSE for progress and engine streaming

Frontend:

- React + TypeScript
- panel-based workspace UI
- generated API types from the OpenAPI contract

## Data Model

The storage model uses two persistence layers with distinct roles:

- SQLite stores games, players, events, tags, and provenance
- Exact postings store position occurrences keyed by full Zobrist hash

This split keeps metadata operations simple while making position and opening-stat queries fast enough for interactive exploration.

## Core Request Flows

Import flow:

- parse PGN
- validate and encode moves
- write games to SQLite
- rebuild immutable postings and optional opening-tree aggregates from canonical SQLite
- stream progress via SSE

Opening exploration flow:

- frontend derives or requests a position hash
- backend loads matching position occurrences from exact postings
- backend resolves valid game contexts from SQLite
- backend returns per-move aggregated statistics

Engine analysis flow:

- frontend registers or selects a UCI engine
- backend starts an analysis session for a FEN position
- info, complete, and error events stream over SSE

## Quality Controls

- strong Catch2 backend coverage
- smoke E2E coverage in the web frontend
- OpenAPI as the wire-contract source of truth
- pre-commit formatting and `clang-tidy` checks
- Docker image publication via GitHub Actions to Quay

## Known Limitations

- SQLite mutations invalidate derived indexes before changing canonical data; queries fail closed until rebuild
- the web UI is ahead of documentation, but still behind the backend in overall maturity
- some historical specs describe a Qt GUI, while the active implementation is a web frontend
