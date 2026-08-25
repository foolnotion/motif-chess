# Compact Position Postings

## Status

**Implemented and validated on the 1M corpus.** This document preserves the
version-4 baseline and the version-5 decision that followed it. The accepted
implementation subsequently advanced to format version 6, reduced the artifact
to 1,035 MiB, and incorporated the measured opening-tree follow-ups recorded in
`docs/optimization-journey.md`. Statements below about the "current" version-4
implementation and "next" work are historical planning context, not the present
repository state.

This plan replaced position-postings format version 4 with compact version 5.
It did not change SQLite, position-query semantics, opening-tree semantics, or
the independent builder passes, and it did not add a replay tape.

### Measured Result

Three serialized Release runs on `bench/data/twic-1m.pgn` produced:

| Metric | Version 4 baseline | Version 5 range | Result |
|---|---:|---:|---|
| End-to-end postings wall | 95.272 s | 67.409-67.962 s | 28.7-29.2% lower |
| Process peak RSS | 5,962 MiB | 1,254-1,474 MiB | 75.3-79.0% lower |
| Artifact size | 3,196 MiB | 1,206 MiB | 62.3% lower |
| Spill runs | 85 | 85 | unchanged |

All three version-5 builds indexed 991,278 games, 88,221,427 occurrences, and
68,907,007 distinct hashes. The artifact sections were stable at 420,327,977
posting bytes, 9,912,780 metadata bytes, 828,439,768 compressed-directory
bytes, and 5,921,696 sparse-directory bytes. The measured staging and spill
high-water estimate was 2,368 MiB and no full payload temporary was created.

The implementation passes the normal and ASan/UBSan test suites, including
clang-tidy and cppcheck. The artifact, wall-time, and RSS gates pass. The
popular-hash occurrence-query p99 and isolated retained-reader RSS gates remain
unmeasured; the recorded tree-navigation query latency is not a substitute for
those postings-specific measurements.

## Decision

Version the position-postings artifact and make four related changes in one
format migration:

1. Store result and Elo metadata once per game instead of once per occurrence.
2. Group each hash's occurrences by game and delta-code game IDs and plies.
3. Replace the flat 32-byte entry for every hash with compressed directory
   blocks addressed by a small fixed-width sparse directory.
4. Write merged posting blocks directly to the final staging artifact. Do not
   create and copy a complete `.payload.tmp` file.

These changes belong together. Grouped postings need byte offsets and lengths,
which require a new directory. A directory placed after the payload allows the
builder to stream posting blocks without retaining every directory entry in
memory or copying the payload during final assembly.

Keep the existing bounded sorted-run construction for the first version-5
implementation. Partitioning and radix sorting remain a later optimization.
Changing the run algorithm and the persistent format together would make a
performance or correctness regression harder to isolate.

## Evidence

The current version-4 artifact uses:

- a 44-byte header;
- one 32-byte directory entry per distinct hash;
- one 13-byte payload record per unique `(hash, game_id, ply)` occurrence.

Seven of each payload record's 13 bytes repeat result and Elo metadata. At
88,221,427 occurrences, those repeated fields consume about 589 MiB. The full
payload consumes about 1.07 GiB.

The recorded 3,196 MiB artifact minus its fixed-width occurrence payload
implies that the flat directory occupies about 2.1 GiB. This is an estimate
from rounded benchmark output, not a measured directory count. The builder
retains that directory as `std::vector<directory_entry>` until final assembly,
so it is also the largest code-supported explanation for much of the 5,962 MiB
process peak.

The current merge writes the complete occurrence payload to `.payload.tmp`,
then reads and copies it into the final temporary artifact. The manager later
opens the artifact and performs another full-file checksum pass. Version 5 must
remove the payload copy first; checksum fusion is a separate publication
optimization after the format is stable.

## Required Semantics

The new format must preserve these contracts exactly:

- An occurrence is `(zobrist_hash, game_id, ply)`.
- Exact duplicate occurrences are collapsed.
- Repeated visits by one game at different plies are preserved.
- Occurrences for one hash are ordered by ascending game ID, then ascending
  ply.
- `occurrences(hash, limit, offset)` paginates occurrences, not games.
- A zero limit remains unbounded.
- Distinct-game IDs are ascending and contain each game once.
- Occurrence count, distinct-game count, minimum ply, and maximum ply retain
  their current meanings.
- Result and Elo values returned for an occurrence come from that occurrence's
  game metadata.
- Missing white and black Elo remain independently representable.
- Game ID zero is invalid.
- The index remains immutable, rebuildable, and generation-qualified.
- Unknown or malformed versions fail closed with `error_code::schema_mismatch`.

SQLite remains the source of truth. The metadata table is a query accelerator,
not a second independently mutable game-header store.

## Version 5 Layout

Use explicit little-endian fixed-width fields and unsigned LEB128 varints. Do
not serialize C++ structs. All offsets are absolute byte offsets from the start
of the file so each section can be validated independently.

```text
fixed header
posting blocks, ordered by hash
game metadata table, ordered by game ID
compressed directory blocks, ordered by first hash
sparse directory, ordered by first hash
```

Placing both directories after the posting payload lets the merge write posting
blocks immediately. The writer patches the fixed header only after every
section has closed successfully.

### Header

The header contains:

| Field | Type | Purpose |
|---|---|---|
| magic | 8 bytes | Identifies `MOTIFPO2` |
| version | u32 | Must equal 5 |
| header size | u32 | Permits checked extension |
| indexed game count | u64 | Source game count |
| occurrence count | u64 | Unique occurrences after merge |
| distinct hash count | u64 | Number of posting blocks |
| metadata offset | u64 | Start of game metadata |
| metadata count | u64 | Number of metadata records |
| directory offset | u64 | Start of compressed directory blocks |
| directory byte length | u64 | Bounds directory decoding |
| sparse offset | u64 | Start of sparse directory |
| sparse count | u64 | Number of directory blocks |

The manager manifest continues to bind the artifact to `source_generation` and
to carry the whole-file checksum. Do not duplicate source generation in the
artifact unless a later bundle-format decision requires standalone provenance.

The reader validates section order, non-overlap, arithmetic overflow, file
bounds, counts, and exact EOF before allocating from any persisted count.

### Game Metadata

Write one fixed-width record for each indexed game that emitted at least one
position:

| Field | Type | Purpose |
|---|---|---|
| game ID | u32 | Metadata lookup key |
| result code | u8 | `0` black win, `1` draw/other, `2` white win |
| flags | u8 | White and black Elo presence bits; all others zero |
| white Elo | i16 bits | Meaningful only when present |
| black Elo | i16 bits | Meaningful only when present |

Records are strictly ascending by game ID. Use the explicit three-value result
encoding instead of converting `255` back to `int8_t` for a black win.

Do not assume game IDs are contiguous. The reader loads metadata into a compact
sorted vector and resolves IDs by binary search initially. A dense ID-indexed
table is permitted only after measured ID density and retained RSS justify it.

The expected cost is 10 bytes per indexed game, about 10 MiB for one million
games. This replaces approximately 589 MiB of repeated version-4 fields on the
measured corpus.

### Posting Block

Each distinct hash has one posting block. The directory supplies the block's
byte offset, byte length, occurrence count, distinct-game count, minimum ply,
and maximum ply. The block body contains groups in ascending game-ID order:

```text
repeated distinct_game_count times:
  game_id_delta: uleb128
  ply_count: uleb128
  first_ply: uleb128
  repeated ply_count - 1 times:
    ply_delta: uleb128
```

The first game ID is encoded as a delta from zero. Each later game ID delta
must be positive. `ply_count` must be positive. The first ply may be zero; each
later ply delta must be positive because exact duplicates were removed during
merge.

The sum of all `ply_count` values must equal the directory's occurrence count.
The decoded first and last plies must reproduce its minimum and maximum ply.
The decoder rejects non-canonical, overflowing, or overlong varints.

Example: one hash occurring in game 10 at plies 0 and 4, and in game 14 at ply
9, is encoded schematically as:

```text
10, 2, 0, 4, 4, 1, 9
```

The second `4` is the game-ID delta from 10 to 14. The first `4` is the ply
delta from 0 to 4. The persisted stream distinguishes them by field position.

### Compressed Directory Block

Use blocks with at most 256 hash entries. A block starts with fixed-width
framing:

| Field | Type | Purpose |
|---|---|---|
| first hash | u64 | Absolute key and sparse lookup key |
| first posting offset | u64 | Absolute offset of the first posting block |
| entry count | u16 | `1..256` |

The first entry encodes its metadata without hash or offset deltas. Later
entries encode positive hash and posting-offset deltas. Entry fields are:

```text
hash_delta: uleb128                 # omitted for the first entry
posting_offset_delta: uleb128       # omitted for the first entry
posting_byte_length: uleb128
occurrence_count: uleb128
distinct_game_count: uleb128
min_ply: uleb128
max_ply: uleb128
```

The writer buffers at most 256 logical entries, encodes one directory block,
and appends it after posting construction. It does not retain a vector with one
entry per hash.

Because directory data follows the metadata table, the builder may retain
compressed directory blocks in a directory spool while it writes posting
blocks. The spool is expected to be far smaller than the version-4 directory.
It is an allowed bounded temporary artifact and is removed on every success or
failure path. The implementation may eliminate the spool if a simpler single
file layout preserves direct posting writes and bounded RSS.

### Sparse Directory

Write one fixed-width entry per compressed directory block:

| Field | Type | Purpose |
|---|---|---|
| first hash | u64 | First key in the block |
| block offset | u64 | Absolute file offset |
| block byte length | u32 | Checked read bound |
| entry count | u16 | Checked decode bound |

The sparse entries are strictly ascending by first hash. The reader keeps only
this sparse directory and the game metadata table in memory. For 256 hashes per
block, the sparse entry count is roughly 1/256 of the old directory count.

A point lookup performs one binary search in memory, one compressed-directory
block read, and one posting-block read. It must not issue one seek per binary
search comparison as version 4 does.

## Builder Pipeline

Retain sorted spill records with the current `(hash, game_id, ply)` key and the
current one-million-record default threshold.

1. Capture the source game count and initialize metrics.
2. Replay games through the minimal SQLite cursor. Record game metadata once
   and emit `(hash, game_id, ply)` records as today.
3. Sort and spill bounded runs. Preserve exact duplicate suppression during
   the global merge.
4. Open one final staging file and write a placeholder version-5 header.
5. K-way merge runs. For each hash, group records by game, encode one posting
   block directly to staging, and accumulate one logical directory entry.
6. Flush each 256-entry directory block to the bounded directory spool. Record
   its first hash, spool offset, byte length, and entry count.
7. Remove consumed spill runs after all posting blocks are complete.
8. Write the sorted game metadata table to staging.
9. Copy the compact directory spool once to staging while translating spool
   offsets into absolute directory offsets for sparse entries.
10. Append the sparse directory, patch the header, flush, close, validate, and
    rename staging to the generation-qualified unpublished artifact.

The directory spool is not equivalent to the removed payload temporary. Its
size scales with compressed per-hash metadata, not with all occurrences. Record
its measured size and peak temporary-disk contribution.

The empty-index path uses the same version-5 writer and produces empty,
well-bounded sections. Do not keep a second hand-written header path.

## Reader Pipeline

`open()` validates the header and all section ranges, then scans:

1. the metadata table, checking strict game-ID order, result codes, flags, and
   metadata count;
2. the sparse directory, checking strict hash order, block bounds, entry
   counts, non-overlap, and complete coverage of the compressed-directory
   region;
3. each compressed directory block, checking hash order, posting-block order,
   posting bounds, aggregate invariants, and total hash/occurrence counts.

`open()` does not decode every posting block. The manifest checksum protects
the complete payload for manager-owned artifacts. Query-time block decoding
still validates local framing and aggregate counts before returning data.

`summary(hash)` reads and decodes one directory block. `distinct_game_ids(hash)`
decodes only game groups and skips ply values after validation. `occurrences()`
decodes the complete selected block because offset and limit count occurrences,
then attaches metadata to returned matches.

The accepted version-5 implementation decoded one hash's complete posting block
before pagination. The later review fix now retains only the requested window
while validating the complete encoded block; a block-internal skip index remains
unnecessary until profiling justifies the additional format complexity.

## Internal Summary Stream

Define a sorted streaming API while the directory abstraction is introduced:

```cpp
for_each_summary(visitor(zobrist_hash, position_postings_summary))
```

The callback receives hashes in strictly ascending order and may not retain a
reader-owned view. The function streams compressed directory blocks and does
not decode postings or allocate one object per hash.

The version-5 summary stream enabled the subsequent opening-tree optimization:
the manager-backed builder now consumes postings `distinct_game_count` values
instead of reconstructing the same full-depth child frequencies. The standalone
tree builder remains the independent replay oracle.

## Publication And Compatibility

Position postings are derived data. Do not convert version-4 files in place.

- New builds write version 5 only.
- A version-4 reader may remain temporarily if existing bundles are a shipped
  compatibility requirement. Otherwise, an old version is unavailable and
  the manager uses the existing correct fallback until rebuild.
- Never delete a manifest-referenced version-4 artifact before a version-5
  artifact has validated and its manifest has published.
- A failed build removes spills, directory spools, and unpublished staging
  files. It leaves the previous manifest and reader usable.
- Keep generation-qualified filenames and atomic manifest publication.
- Do not add format version to the manifest in this change. The artifact
  header is authoritative, and changing both schemas adds no query benefit.

Before implementation, confirm whether any released bundle must remain readable
without an explicit rebuild. If not, omit the version-4 compatibility reader.

The existing build obtains `count_games()` separately from the replay cursor.
Version 5 must count replayed games and reject a mismatch before publication.
A later orchestration change should bind count and replay to one explicit
SQLite read snapshot; the format rewrite must not claim snapshot atomicity that
the current API does not provide.

## Failure Handling

Reject the artifact before allocation or seek when any of these conditions is
detected:

- header, section, or block arithmetic overflows;
- sections overlap, appear out of order, or exceed file size;
- metadata IDs, sparse hashes, or decoded hashes are not strictly ascending;
- result codes or metadata flags are invalid;
- a directory block has zero or more than 256 entries;
- varints overflow, exceed their maximum encoded length, or are non-canonical;
- game-ID or ply deltas violate strict order;
- a posting block exceeds its directory bounds;
- decoded occurrence, distinct-game, or ply summaries disagree with the
  directory entry;
- decoded totals disagree with the header;
- trailing bytes remain after the sparse directory.

Use `error_code::schema_mismatch` for malformed persistent structure and
`error_code::io_failure` for read, write, flush, close, rename, or filesystem
failures. Preserve `error_code::invalid_argument` for invalid builder input.

## Metrics

Correct the benchmark before comparing version 4 and version 5. Report
non-overlapping phases:

- SQLite scan and move decode;
- board replay and record accumulation;
- run sort;
- run write;
- k-way merge and duplicate removal;
- posting-block encoding and write;
- metadata write;
- directory-block encoding and spool write;
- directory and sparse-directory final write;
- validation/open;
- checksum and publication.

Also report:

- replayed game count;
- input and unique occurrence counts;
- distinct hash count;
- spill count and spill bytes;
- posting bytes;
- metadata bytes;
- compressed-directory bytes;
- sparse-directory bytes;
- peak temporary bytes;
- final artifact bytes;
- phase-local current RSS and process peak RSS.

The current benchmark does not print `final_write_elapsed` even though the
builder records it. Add it to the comparison output. Run each performance case
in a fresh process and serialize heavy cases.

## Tests

### Semantic Equivalence

- Compare version-5 occurrences, distinct game IDs, and summaries with the
  existing DuckDB oracle field-for-field.
- Cover transpositions, repeated positions, terminal positions, all result
  values, independently missing Elo values, and non-contiguous game IDs.
- Force duplicate tuples into different spill runs and verify one output.
- Exercise pagination at zero, one, the exact count, beyond the count, and
  `std::numeric_limits<std::size_t>::max()`.
- Verify directory-block boundaries at 255, 256, and 257 hashes.
- Verify the sorted summary stream against point `summary()` calls.

### Codec And Corruption

- Round-trip fixed-width fields and varints at zero, one, each byte boundary,
  and each supported maximum.
- Reject bad magic, old and unknown versions, truncated headers, overflowing
  offsets, overlapping sections, and trailing bytes.
- Reject malformed metadata order, duplicate IDs, invalid result codes, and
  unknown flag bits.
- Reject malformed sparse ordering, directory bounds, entry counts, hash
  deltas, posting offsets, and block lengths.
- Reject overlong, overflowing, and non-canonical varints.
- Reject posting count, distinct-game count, minimum-ply, and maximum-ply
  mismatches.
- Reject a posting that references missing metadata.

### Lifecycle

- Build and reopen an empty index.
- With tiny spill thresholds, verify cleanup of every `.spill*`, directory
  spool, `.tmp`, and unpublished generation after injected failures.
- Verify publication failure leaves the prior generation queryable.
- Verify a replayed-game/count mismatch prevents publication.
- Verify a version-4 artifact is rebuilt or rejected according to the selected
  compatibility policy; never silently interpret it as version 5.

Every public API, including the new summary stream if public, requires direct
tests under the project convention.

## Benchmark Gates

Use at least three isolated Release runs on `bench/data/twic-1m.pgn`. Record the
corpus checksum, accepted game count, Git revision or worktree identity,
compiler, allocator, storage device, and cold/warm cache state. Compare medians
and ranges; do not accept a result inside observed run variance.

Version 5 passes only if:

| Metric | Gate |
|---|---|
| Correctness | Exact oracle and version-4 semantic equivalence |
| Artifact size | At most 1.6 GiB, 50% of the recorded version-4 artifact |
| Builder peak RSS | At least 35% below the version-4 postings phase peak |
| Temporary disk | No full payload temporary; at least 1.0 GiB lower peak temporary bytes |
| Rebuild wall time | At least 15% lower median than version 4 |
| Warm query p99 | No more than 10% regression for rare, median, and popular hashes |
| Reader RSS | Metadata plus sparse directory remains below 128 MiB at 1M games |

The artifact target is based on the known 589 MiB repeated metadata and the
estimated 2.1 GiB flat directory. If compressed hash deltas do not reach it,
retain the implementation only if measured RSS, wall time, and temporary-disk
wins still justify the format complexity and document the failed estimate.

Measure occurrence queries separately for rare, median, and popular hashes.
For popular hashes, include offsets near the start, middle, and end because
version 5 initially decodes the complete posting block for pagination.

## Delivery Sequence

### Phase 0: Baseline And Instrumentation

- Add the missing phase and byte metrics without changing the version-4
  format.
- Run repeated postings-only Release baselines in fresh processes.
- Record the actual distinct-hash count and directory bytes, replacing the
  estimate in this plan.

Exit gate: all version-4 wall time, RSS, artifact, temporary-byte, and query
measurements have repeated-run variance.

### Phase 1: Version-5 Codec

- Implement checked fixed-width and canonical varint codecs.
- Implement metadata, posting-block, compressed-directory, sparse-directory,
  and header encode/decode tests.
- Keep these helpers internal to `motif_db`.

Exit gate: codec and corruption tests pass without integrating the builder.

### Phase 2: Version-5 Builder And Reader

- Stream posting blocks directly to staging during the existing k-way merge.
- Replace the full in-memory directory with 256-entry block buffering and the
  bounded directory spool.
- Write metadata and sparse sections, patch the header, and implement point
  queries plus the sorted summary stream.
- Preserve manager publication and fallback behavior.

Exit gate: all semantic, corruption, lifecycle, normal, and sanitizer tests
pass.

### Phase 3: Benchmark Decision

- Run the version-4 and version-5 builds in alternating fresh processes.
- Compare all gates and retain raw outputs.
- If the build misses a gate, attribute the miss before adding radix sorting,
  checksumming changes, block skip indexes, or replay sharing.

Exit gate: accept version 5, revise it with one measured follow-up, or retain
version 4. Do not stack speculative optimizations onto an unexplained result.

### Phase 4: Opening-Tree Count Reuse — Completed

After version 5 was accepted, the sorted summary stream replaced the
manager-backed tree builder's duplicate child-visit spill and merge. The
standalone tree builder retains its independent replay counter as an oracle and
fallback. Measurements and follow-on codec/scratch changes are recorded in
`docs/optimization-journey.md`.

## Follow-up Inventory

Completed after the version-5 decision:

- block-internal windowed pagination without a persisted skip index;
- opening-tree reuse of postings counts;
- buffered opening-tree codecs and retained per-game scratch capacity.

Still measurement-gated:

- high-hash partitioning and radix sorting of spill records;
- computing the manifest checksum during final output;
- compact move or transition replay tapes;
- joint publication of postings and opening-tree artifacts.

These items are not prerequisites for the accepted compact-postings format.

## Non-Goals

- No change to canonical SQLite game or move storage.
- No loss of repeated-position occurrences.
- No prefix or probabilistic postings.
- No memory map requirement.
- No new dependency.
- No concurrent postings and opening-tree builders.
- No in-memory directory with one object per distinct hash.
- No compatibility layer unless a released persisted bundle requires it.
- No replay tape until the format and tree-count optimizations have been
  measured.
