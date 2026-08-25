# Shared Board-Transition Replay

## Validation Status

**Historical rejected candidate.** The benchmark rejected a full transition
tape as the next optimization. The repository retained sequential builders and
implemented the higher-value format, child-count reuse, buffered-codec, and
scratch-reuse work first. This document remains the rationale and design record
for reconsidering replay sharing only if a new profile supports it.

The 1M-game corpus has about 82 million positions. A 10-byte transition record
therefore creates roughly 0.8-0.9 GiB of tape payload. The original pipeline
writes that payload once and reads it twice, adding approximately 2.4-2.7 GiB
of sequential traffic while retaining the builders' existing spill, merge, and
final-output traffic. This may beat a second board replay on fast storage, but
it must be measured rather than assumed.

The production design is therefore **sequential builders over a
benchmark-selected replay source**. The full transition tape remains one
candidate, not the predetermined implementation.

### Measured 1M Result

The serialized Release comparison on `bench/data/twic-1m.pgn` after the
correctness, bounded-reader, run-local aggregation, and cleanup work measured:

| Path | Wall | Peak RSS | Derived artifact |
|---|---:|---:|---:|
| DuckDB | 60.055 s | 9,552 MiB | 2,039 MiB |
| Exact postings | 95.272 s | 5,962 MiB | 3,196 MiB |
| Postings + shallow tree | 154.805 s | 5,973 MiB | 3,438 MiB combined |

Postings processed 88,221,427 occurrences in 85 spill runs. Its measured phases
were 12.316 s replay, 6.871 s spill, and 15.698 s merge, with additional import,
checksum, validation, and publication work in the end-to-end number. The tree
spent 23.939 s in replay, 11.930 s merging child frequencies, 6.402 s merging
edges, and 12.393 s writing the index.

This rejected replay sharing as the next optimization. The dominant measured
opportunities were the postings format's per-hash directory/storage amplification
and the tree's duplicate reconstruction of child distinct-game counts. The
subsequent implementation completed those items without adding a transition tape:

1. Version postings to store result/Elo once per game.
2. Replace the flat 32-byte-per-hash directory with block-compressed hash and
   offset/count metadata plus a sparse top-level directory.
3. Write merged posting blocks directly to final staging rather than copying a
   full payload temporary.
4. Expose postings' distinct-game counts as a sorted stream and use them for
   tree transposition frequencies, removing child visit spills and merge.
5. Buffer fixed-width tree spill and final-index codecs.

## Decision

Derived-index rebuilds that need board transitions use either independent
direct passes or a disk-backed replay tape selected by benchmark. They never use
a live callback that retains both builders.

When a tape is selected, it is materialized from a read-consistent SQLite
snapshot. Consumers stream it one at a time. A consumer is fully finalized and
destroyed before the next consumer starts. In particular, the position-postings
and opening-tree builders must never exist concurrently.

This deliberately does not restore a callback such as:

```cpp
store.for_each_replay_game([&](replay_game_record const& game) {
    postings.accumulate(game);
    opening_tree.accumulate(game);
});
```

Even when that callback has one SQLite cursor, both builders retain spill
buffers, run lists, merge state, and allocator arenas at the same time. It
shares CPU work but raises peak RSS by the overlap of both working sets.

## Candidate Replay Sources

Benchmark these candidates in this order:

1. **Two direct SQLite passes.** This is the low-complexity baseline: compact
   2-byte moves are read twice and replayed twice, with no staging artifact.
2. **Compact move tape.** Read SQLite once and store metadata plus encoded moves
   only. Each builder replays the board. This isolates the cost of the second
   SQLite traversal without paying to store hashes.
3. **Postings-first hybrid transition tape.** During the single SQLite pass,
   replay each game once, feed the postings builder, and write metadata, moves,
   and hashes for the opening-tree builder. Finalize and destroy postings, then
   construct the tree builder and read the tape once. This is the preferred
   transition-tape candidate because it performs one tape write and one tape
   read rather than two reads.
4. **Full transition tape consumed twice.** Materialize without either builder,
   then replay the tape into each builder sequentially. This has the cleanest
   phase isolation and highest staging I/O. Retain it only if measurements show
   a material benefit or operational simplicity justifies its cost.

Adopt a tape only if repeated isolated 1M runs improve dual-build wall time by
at least 10% or meet a separately defined RSS requirement without unacceptable
temporary-disk growth. If results are within run variance, retain two direct
SQLite passes.

## Replay Tape

The tape is an ephemeral binary staging artifact in the bundle directory. It
is not a published index and is removed after the rebuild succeeds or fails.
Writing beside the final artifacts keeps temporary files on the same volume and
avoids cross-device rename behavior.

Use a fixed-width, little-endian format. Do not serialize C++ structs directly.

### Header

| Field | Type | Purpose |
|---|---|---|
| magic | 8 bytes | Identifies `MOTRPLY1` format |
| version | u32 | Reject incompatible readers |
| source revision | u64 | Binds the tape to one transactional SQLite revision |
| source game count | u64 | Detects truncated or mismatched streams |
| record count | u64 | Written after streaming completes |

### Game Record

| Field | Type | Purpose |
|---|---|---|
| game id | u32 | Canonical SQLite game ID |
| result | i8 | Normalized `-1`, `0`, or `1` |
| metadata flags | u8 | White/black Elo presence bits |
| white Elo | i16 | Value is meaningful when its presence bit is set |
| black Elo | i16 | Value is meaningful when its presence bit is set |
| move count | u32 | Bounds the variable payload |
| initial hash | u64 | Makes the starting position explicit |
| transitions | repeated | One record per ply |

A transition is `encoded_move: u16` followed by `position_hash: u64`, where
`position_hash` is the hash after applying that move. The writer replays each
encoded move using the same trusted-input contract as the existing builders and
rejects games that exceed the existing ply limit. Checked move validation
requires a separate chess API change because `apply_encoded_move` currently
cannot report an error. The reader validates the count against remaining file
size, checks game IDs are strictly ascending, and rejects trailing bytes.

The tape includes a payload checksum. Framing and ordering checks cannot detect
a validly framed but corrupted move or hash. Consumers trust stored hashes, so
payload integrity is required.

The hashes make position-postings construction a copy/format operation rather
than a second board replay. The encoded moves retain exactly what the
opening-tree builder needs for continuation keys and root-ply semantics. The
metadata carries its result and Elo aggregation inputs. This is approximately
10 bytes per ply plus a small per-game header, bounded on disk rather than in
RSS.

## Preferred Staged Pipeline

`database_manager` owns the orchestration because it owns the SQLite
connection, manifest generation, and published derived artifacts.

1. Acquire exclusive rebuild ownership. Establish the SQLite read snapshot with
   a read inside the transaction, then obtain its game count and source
   revision. A manifest-only generation is not an atomic SQLite snapshot ID.
2. Construct only the position-postings builder. Traverse SQLite exactly once
   with the minimal replay query. For each row, decode its move blob, apply
   moves to one board, feed hashes directly to postings, and append one game
   record to `board-transitions.replay.tmp` for the later tree phase.
3. Validate the final game count, checksum and close the tape, finalize postings
   to a generation-qualified staging filename, then destroy the postings
   builder. End the SQLite read transaction only after the source facts needed
   for publication have been captured.
4. Construct the opening-tree builder, stream the tape once, finalize to a
   generation-qualified staging filename, then destroy the builder.
5. Open each staged artifact with a temporary validation reader and destroy that
   reader before the next phase. Do not install long-lived readers while any
   builder remains alive.
6. Verify the SQLite source revision has not changed. Atomically publish one
   manifest referencing both generation-qualified artifacts, then install the
   complete in-memory reader generation under the manager's synchronization
   policy.
7. Remove the replay tape and garbage-collect artifacts from older unreferenced
   generations only after successful manifest publication.

The existing DuckDB position-table rebuild is a separate transaction-sensitive
operation. It may consume the tape in a later migration, but it must not be
combined with either spill-heavy index builder. Until then it retains its
current standalone rebuild path. The tape is introduced for the two derived
index builders specifically; it is not an excuse to run the DuckDB writer,
postings builder, and opening-tree builder together.

If only one derived index is requested, use its direct `game_store` builder.
The tape path is eligible only when two or more board-transition consumers are
requested in one rebuild operation.

## API Shape

Add an internal `board_transition_replay` module with two roles:

- `materialize(game_store const&, replay_tape_path, replay_snapshot)` writes
  the tape from the single SQLite cursor.
- `for_each_game(replay_tape_path, visitor)` opens the tape and invokes the
  visitor with a non-retained `board_transition_game` view.

`board_transition_game` contains normalized metadata, an initial hash, and
parallel `encoded_moves` / `position_hashes` spans. Its backing storage belongs
to the reader and is valid only during the callback. It is intentionally not
`replay_game_record`: consumers must not accidentally retain a copied vector
for every game.

Both builders receive overloads that consume this type. The postings overload
is needed for the full-tape benchmark candidate; the preferred hybrid feeds
postings directly during tape generation. The opening-tree overload uses stored
hashes when walking roots and children, preserving its per-game edge
deduplication and aggregate semantics without creating a board. Keep standalone
`build(game_store, ...)` overloads for unit use and single-consumer builds. They
must not introduce a dual-builder visitor.

The orchestrator must use scopes, not merely `clear()` calls:

```cpp
{
    auto postings = position_postings_builder {postings_staging_path, options};
    materialize_replay_tape(store, tape_path, postings);
    postings.finalize();
}  // Postings builder state is released before tree construction.

{
    auto tree = opening_tree_index_builder {tree_staging_path, options};
    replay_tape.for_each_game([&](auto const& game) { return tree.accumulate(game); });
    tree.finalize();
}
```

## Failure And Recovery

- A failed tape write, malformed SQLite row, or count mismatch aborts the active
  postings build before tree construction starts.
- A consumer failure removes its generation-qualified unpublished output and
  the tape. Existing manifest-referenced indexes remain untouched.
- A process crash can leave `*.replay` or `*.tmp` files. Startup and rebuild
  cleanup removes only named staging artifacts, never a manifest-published
  index.
- The tape's SQLite source revision and game count are checked before every
  consumer. A same-count metadata mutation must still invalidate publication.
- Stable filenames are not overwritten before manifest publication. Both new
  artifacts use generation-qualified names, and one manifest swap publishes the
  complete generation.
- Process-crash atomicity and power-loss durability are separate guarantees.
  If power-loss durability is required, fsync staged files before rename and
  fsync the containing directory after artifact and manifest renames.

## Subsequent Performance Work

The replay source was not the largest known source of avoidable work. Later
iterations completed several items from this inventory:

- compact postings now store result/Elo once per game and use a compressed
  directory;
- final opening-tree writes use buffered codecs;
- per-game tree scratch retains capacity;
- manager-backed tree construction reuses postings child counts.

High-hash partition/radix construction, checksum fusion, and replay tapes remain
measurement-gated candidates. `docs/optimization-journey.md` records the
implemented sequence and benchmark results.

## RSS Contract And Tests

Correctness tests must cover:

- tape round-trip preserves game ID order, metadata, moves, initial hashes,
  and post-move hashes, including missing Elo and repeated positions;
- both tape-backed builders match the existing SQLite-backed oracle with tiny
  spill thresholds;
- malformed headers, counts, payload lengths, non-ascending IDs, invalid
  moves, and trailing bytes are rejected;
- a dual-artifact rebuild leaves no replay staging files after success or
  failure;
- an orchestration test records builder lifetime events and proves postings is
  destroyed before opening-tree construction;
- publication failure after either artifact build leaves the prior manifest
  generation usable;
- a same-game-count source mutation prevents publication;
- temporary validation readers and long-lived installed readers do not overlap
  either builder's lifetime.

RSS cannot be proven reliably by a normal unit test. Run each candidate in a
fresh subprocess and record baseline RSS, phase-local current RSS, process peak
RSS, final steady-state reader RSS, tape size, total temporary bytes, wall time,
CPU time, and cold/warm storage state. Allocators may retain pages after a
builder is destroyed, so a same-process high-water mark cannot prove independent
phase peaks. Record repeated separate-build and dual-build baselines before
turning measurements into a calibrated gate.

Builder metrics must use non-overlapping phases: SQLite scan/decode, board
transition generation, run sort, run write, run merge, final assembly,
validation/open, checksum/publication, and peak temporary bytes. Current replay
timers include synchronous spill work and therefore cannot identify whether a
tape targets the dominant phase.

## Non-Goals

- No in-memory vector of all replay records or hashes.
- No concurrent consumer threads or tee/fan-out visitor.
- No shared SQLite traversal that constructs both builders.
- No persistent replay cache or manifest entry in the first implementation.
- No change to canonical SQLite game storage or move encoding.
