# Product Overview

## Goal

Motif Chess is a local-first chess database and analysis application. It helps a single user import PGN collections, search positions, explore opening statistics, and run engine-assisted analysis on local data.

## Current Capabilities

- Import PGN collections from disk or browser upload
- Monitor or resume imports through SSE progress updates
- Search positions by Zobrist hash
- Query opening continuation statistics from any position
- Add, edit, export, and delete personal games
- Register UCI engines and stream live analysis
- Run locally from source or Docker

## Intended User

The current product is optimized for a single local user:

- a player importing personal or reference PGN collections
- an analyst exploring opening trends from the current board position
- a developer or advanced user running the backend locally

Authentication, cloud sync, and multi-user access are intentionally out of scope for the current version.

## Frontend Shape

The current web UI is organized around six panels:

- board workspace
- opening explorer
- game list
- notation
- engine analysis
- import and backend status

The frontend consumes the HTTP API defined in `docs/api/openapi.yaml`.

## Non-Functional Priorities

- local-first durability
- clear, testable API boundaries
- fast position and opening-stat queries
- straightforward deployment and release workflow
- maintainable code with automated tests
