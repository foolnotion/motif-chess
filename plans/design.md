## Prerequisites (External Libraries)

Motif-chess depends on three external libraries, all authored in-house and
consumed as Nix flake inputs. Each must reach the specified milestone before
dependent motif-chess specs can begin.

### chesslib (sourcehut:bogdanb/chesslib)

Chess core: 0x88 mailbox board, move generation, Zobrist hashing.

| Task                                         | Needed By        |
|----------------------------------------------|------------------|
| ply/count: u8 -> u16                         | 002-import       |
| Encapsulate pieces/colors (private + accessors) | 002-import    |
| Make do_move/swap private                    | 002-import       |
| Fix const correctness in move_generator      | 002-import       |
| Add scope_guard utility (king restore)       | 002-import       |
| parse_san(string_view) -> expected<move>     | 002-import       |
| to_san(move) -> string                       | 002-import       |
| Compact 16-bit move encode/decode            | 001-database     |
| Incremental Zobrist update                   | 002-import       |
| Verify Zobrist side-to-move XOR              | 002-import       |

Minimum version: all tasks above complete, tagged, and pinned in flake.lock.

### pgnlib (sourcehut:bogdanb/pgnlib)

PGN parser built on foonathan::lexy. Produces pgn::game structs with SAN
strings — no chess logic dependency.

| Task                                         | Needed By        |
|----------------------------------------------|------------------|
| lexy grammar: tags, SAN tokens, NAGs         | 002-import       |
| Recursive variation support (depth >= 10)    | 002-import       |
| Comment handling ({})                        | 002-import       |
| Result tokens (1-0, 0-1, 1/2-1/2, *)        | 002-import       |
| Null move support (--)                       | 002-import       |
| %clk line comment handling                   | 002-import       |
| UTF-8 player names                           | 002-import       |
| Malformed game recovery (skip + log)         | 002-import       |
| parse_file() and parse_string() public API   | 002-import       |
| 100K games in under 10 s                     | 002-import       |

Minimum version: all tasks above complete, tagged, and pinned in flake.lock.

Does not depend on chesslib. SAN validation happens in motif-chess's import
pipeline, not in pgnlib.

### ucilib (sourcehut:bogdanb/ucilib)

UCI engine wrapper built on reproc++. Manages engine subprocess lifecycle
and delivers analysis via callbacks.

| Task                                         | Needed By        |
|----------------------------------------------|------------------|
| Launch/quit engine subprocess                | 004-qt-gui       |
| UCI handshake (uci -> uciok)                 | 004-qt-gui       |
| isready/readyok with timeout                 | 004-qt-gui       |
| setOption, setPosition, go*, stop            | 004-qt-gui       |
| Info line parsing (depth, score, PV)         | 004-qt-gui       |
| Multi-PV support                             | 004-qt-gui       |
| Engine crash isolation (no app crash)        | 004-qt-gui       |
| onInfo / onBestMove callbacks                | 004-qt-gui       |

Minimum version: all tasks above complete, tagged, and pinned in flake.lock.

Does not depend on chesslib or pgnlib. FEN validation is the caller's
responsibility.

### Dependency Graph

    pgnlib ──────────────────────┐
      (text only, no chess dep)  │
                                 ▼
    chesslib ──────────────► motif-chess
      (board, SAN, zobrist)      ▲
                                 │
    ucilib ──────────────────────┘
      (reproc++, no chess dep)

### Flake Inputs

    chesslib.url = "sourcehut:bogdanb/chesslib";
    pgnlib.url   = "sourcehut:bogdanb/pgnlib";
    ucilib.url   = "sourcehut:bogdanb/ucilib";

All three follow nixpkgs from the motif-chess flake:

    chesslib.inputs.nixpkgs.follows = "nixpkgs";
    pgnlib.inputs.nixpkgs.follows   = "nixpkgs";
    ucilib.inputs.nixpkgs.follows   = "nixpkgs";

### Updating After Changes

    nix flake update chesslib   # after chesslib changes land
    nix flake update pgnlib     # after pgnlib changes land
    nix flake update ucilib     # after ucilib changes land

Then rebuild and run tests to confirm compatibility.
