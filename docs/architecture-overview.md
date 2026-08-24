# Architecture Overview

## High-Level Stack

Motif Chess is a local-first C++ application with multiple presentation adapters.

Backend:

- C++23
- SQLite for durable game metadata and move blobs
- Immutable position postings and opening-tree sidecars for position/statistics queries
- HTTP API with SSE for progress and engine streaming

Desktop presentation:

- Slint is the production presentation direction
- toolkit-neutral workspace and import services own UI-facing behavior
- the existing Qt application remains a working migration baseline until Slint reaches parity

The local HTTP/SSE adapter remains available for experimental web clients and automation. It is not
the production desktop presentation layer.

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

- a presentation adapter derives or requests a position hash
- backend loads matching position occurrences from exact postings
- backend resolves valid game contexts from SQLite
- backend returns per-move aggregated statistics

Engine analysis flow:

- a presentation adapter registers or selects a UCI engine
- backend starts an analysis session for a FEN position
- info, complete, and error events stream over SSE

## Quality Controls

- strong Catch2 backend coverage
- headless tests for toolkit-neutral desktop services
- OpenAPI as the wire-contract source of truth
- pre-commit formatting and `clang-tidy` checks
- Docker image publication via GitHub Actions to Quay

## Known Limitations

- SQLite mutations invalidate derived indexes before changing canonical data; queries fail closed until rebuild
- the production Slint shell currently covers workspace and import lifecycle; game browsing, board,
  notation, and search parity remain migration work
- some historical specs describe Qt as the target GUI; Qt now serves only as the working migration baseline
