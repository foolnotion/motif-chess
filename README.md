# motif-chess

A local-first chess position database and analysis server. Import PGN collections, search positions by Zobrist hash, explore opening statistics, and run UCI engine analysis — all through a REST/SSE HTTP API.

## Features

- **PGN import** — parallel pipeline with checkpoint/resume; progress streamed via SSE
- **Position search** — find every game containing a position by Zobrist hash
- **Opening statistics** — move frequencies, win rates, Elo averages per continuation
- **UCI engine analysis** — register any UCI engine, stream depth/score/PV in real time
- **Personal games** — add, edit, and delete games by PGN; full CRUD API
- **OpenAPI spec** — `docs/api/openapi.yaml`

## Building

### Prerequisites

- **Nix** (Linux/macOS) — all dependencies are pinned in `flake.nix`
- **vcpkg** (Windows) — dependencies listed in `vcpkg.json`
- CMake ≥ 3.25, Clang 21

### Nix (recommended)

```sh
nix develop          # enter the dev shell with all deps on PATH
cmake --preset=dev
cmake --build build/dev
```

### vcpkg (Windows / no Nix)

```sh
cmake --preset=vcpkg -D VCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build/vcpkg --config Release
```

### Run the tests

```sh
ctest --test-dir build/dev
```

### Sanitizer build

```sh
cmake --preset=dev-sanitize
cmake --build build/dev-sanitize
ctest --test-dir build/dev-sanitize
```

The binary is produced at `build/dev/bin/motif_http_server`.

## Docker

Published images live at `quay.io/motif-chess/motif-server`.

### Pull a released image

```sh
docker pull quay.io/motif-chess/motif-server:v0.1.0
```

### Run the server in Docker

```sh
mkdir -p "$HOME/chess/my-db"

docker run --rm \
  -p 8080:8080 \
  -v "$HOME/chess/my-db:/data" \
  quay.io/motif-chess/motif-server:v0.1.0 \
  motif_http_server --db /data --host 0.0.0.0 --port 8080
```

Notes:

- mount a host directory so the SQLite and DuckDB files persist across container restarts
- use `--host 0.0.0.0` inside the container so the port mapping is reachable from the host
- replace `v0.1.0` with `latest` if you explicitly want the newest published image

### Build the Docker image locally with Nix

```sh
nix build .#docker --out-link docker-image
docker load < docker-image
mkdir -p "$PWD/tmp-db"

docker run --rm \
  -p 8080:8080 \
  -v "$PWD/tmp-db:/data" \
  motif-chess:latest \
  motif_http_server --db /data --host 0.0.0.0 --port 8080
```

## Usage

### Starting the server

```sh
motif_http_server --db <path/to/database> [--host <host>] [--port <port>] [--cors-origin <origin>]
```

| Flag | Default | Description |
|---|---|---|
| `--db <path>` | — | Path to the database directory (created if it does not exist) |
| `--host <host>` | `127.0.0.1` | Bind address |
| `--port <port>` | `8080` | Listen port |
| `--cors-origin <origin>` | *(none — wildcard `*`)* | Allowed CORS origin; repeat to allow multiple |

All flags can also be set via environment variables: `MOTIF_DB_PATH`, `MOTIF_HTTP_HOST`, `MOTIF_HTTP_PORT`, `MOTIF_HTTP_CORS_ORIGINS` (comma-separated).

```sh
# Minimal — creates a new database at ~/chess/my-db
motif_http_server --db ~/chess/my-db

# Custom port, locked-down CORS for a local frontend
motif_http_server --db ~/chess/my-db --port 9000 \
  --cors-origin http://localhost:5173
```

### Key endpoints

| Method | Path | Description |
|---|---|---|
| `GET` | `/health` | Health check |
| `GET` | `/api/positions/legal-moves?fen=` | Legal moves for a position |
| `POST` | `/api/positions/apply-move` | Apply a UCI move and return the resulting FEN |
| `GET` | `/api/positions/hash?fen=` | Zobrist hash for a FEN position |
| `GET` | `/api/positions/:hash` | Games containing a position |
| `POST` | `/api/positions/rebuild` | Rebuild the DuckDB position store from SQLite |
| `GET` | `/api/openings/:hash/stats` | Opening continuation statistics plus `total_games` |
| `POST` | `/api/imports` | Start a PGN import by filesystem path |
| `POST` | `/api/imports/upload` | Start a PGN import by file upload |
| `GET` | `/api/imports/:id/progress` | SSE stream of import progress |
| `DELETE` | `/api/imports/:id` | Request cancellation of an in-progress import |
| `GET` | `/api/games` | List games (filterable by player, result) |
| `GET` | `/api/games/count` | Total number of games in the database |
| `GET` | `/api/games/:id` | Full game record |
| `GET` | `/api/games/:id/positions` | FEN + hash sequence for a game |
| `GET` | `/api/games/:id/pgn` | Game as PGN text |
| `POST` | `/api/games` | Add a personal game by PGN |
| `PATCH` | `/api/games/:id` | Update metadata of a personal game |
| `DELETE` | `/api/games/:id` | Delete a personal game |
| `POST` | `/api/engine/engines` | Register a UCI engine |
| `GET` | `/api/engine/engines` | List registered engines |
| `POST` | `/api/engine/analyses` | Start an analysis session |
| `GET` | `/api/engine/analyses/:id/stream` | SSE stream of analysis events |
| `DELETE` | `/api/engine/analyses/:id` | Stop an analysis session |

See [`docs/api/openapi.yaml`](docs/api/openapi.yaml) for the full contract.

### Example workflow

```sh
# 1. Import a PGN file and stream progress
curl -s -X POST http://localhost:8080/api/imports \
  -H 'Content-Type: application/json' \
  -d '{"path":"/home/user/games.pgn"}' | jq .
# → {"import_id":"a1b2c3..."}

curl -N http://localhost:8080/api/imports/a1b2c3.../progress
# data: {"games_processed":1000,...,"phase":"ingesting"}
# event: complete
# data: {"committed":998,"skipped":2,"errors":0,"elapsed_ms":4321}

# 2. Get the Zobrist hash for a position
HASH=$(curl -s "http://localhost:8080/api/positions/hash?fen=rnbqkbnr%2Fpppppppp%2F8%2F8%2F4P3%2F8%2FPPPP1PPP%2FRNBQKBNR+b+KQkq+e3+0+1" | jq -r .hash)

# 3. Explore opening statistics
curl -s "http://localhost:8080/api/openings/$HASH/stats" | jq .

# 4. Analyze with a UCI engine
curl -s -X POST http://localhost:8080/api/engine/engines \
  -H 'Content-Type: application/json' \
  -d '{"name":"stockfish","path":"/usr/bin/stockfish"}'

curl -s -X POST http://localhost:8080/api/engine/analyses \
  -H 'Content-Type: application/json' \
  -d '{"fen":"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1","depth":20}' | jq .
# → {"analysis_id":"..."}

curl -N http://localhost:8080/api/engine/analyses/<analysis_id>/stream
# event: info
# data: {"depth":12,"multipv":1,"score":{"type":"cp","value":-31},"pv_uci":["e7e5"],"pv_san":["e5"],...}
# event: complete
# data: {"best_move_uci":"e7e5"}
```

## License

See [LICENSING](LICENSING.md).
