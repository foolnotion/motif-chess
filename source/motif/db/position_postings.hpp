#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <gtl/phmap.hpp>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

class game_store;

// On-disk format version of a postings artifact; recorded in the published
// snapshot descriptor so an artifact this build cannot read is never trusted.
inline constexpr std::uint32_t position_postings_format_version = 6;

struct position_postings_summary
{
    std::uint64_t occurrence_count {};
    std::uint32_t distinct_game_count {};
    std::uint16_t min_ply {};
    std::uint16_t max_ply {};
};

// One game's complete metadata after-image: the exact values the postings
// reader reports for every occurrence of the game. PR 6 supports exactly one
// content revision per game (revision 1); the key is already (game, revision) so
// later PRs add replacement documents without a format change.
//
// `retired` is the tombstone of a removed game: its document stays in the
// immutable segment (postings are never rewritten) but no query may see it. A
// retired after-image is canonical only with result 0 and no ratings; those
// fields carry no information and are ignored by every reader.
struct position_metadata_after_image
{
    game_id id {};
    std::uint64_t content_revision {1};
    std::int8_t result {};  // -1 black win, 0 draw/other, 1 white win
    std::optional<std::int16_t> white_elo;
    std::optional<std::int16_t> black_elo;
    bool retired {false};

    auto operator==(position_metadata_after_image const&) const -> bool = default;
};

// A verified, owned replacement for one metadata slot. The index stays valid as
// long as the reader's metadata table is not resized, which no operation does
// after open().
struct position_metadata_slot
{
    std::size_t index {};
    position_metadata_after_image image;
};

// What the extrema of one opening-tree edge are over the live games that reach
// it: `root` at ply p directly followed by `child` at ply p + 1 within one game,
// p counting only while p <= max_root_ply (or at any ply when any_root_ply).
// The edge (root, move, child) of a game is exactly such a pair, so this is
// the quantity the tree stores per edge, recomputed from the postings alone.
struct position_edge_scan
{
    zobrist_hash root {};
    zobrist_hash child {};
    // A game the scan must ignore (the one being removed); retired games are always ignored.
    game_id excluded {};
    std::uint16_t max_root_ply {};
    bool any_root_ply {false};
    // No live game with a higher id reaches the edge, when the caller knows it; lets a
    // scan for the largest game id stop joining at that id (the rest of each block is still validated).
    std::optional<game_id> sample_ceiling;
    // Which extrema the caller needs. The scan ends as soon as those are settled, and
    // leaves the other fields of the result unspecified. A wanted minimum ply is settled
    // once a game reaches min_ply_floor (no game can be lower than it).
    bool want_min_ply {false};
    std::uint16_t min_ply_floor {};
    bool want_sample_min {false};
    bool want_sample_max {false};
};

struct position_edge_extrema
{
    std::uint16_t min_root_ply {};
    game_id sample_min {};
    game_id sample_max {};
};

struct position_postings_build_metrics
{
    std::uint64_t game_count {};
    std::uint64_t input_occurrence_count {};
    std::uint64_t occurrence_count {};
    std::uint64_t distinct_hash_count {};
    std::uint64_t spill_run_count {};
    std::uint64_t spill_bytes {};
    std::uint64_t posting_bytes {};
    std::uint64_t metadata_bytes {};
    std::uint64_t directory_bytes {};
    std::uint64_t sparse_directory_bytes {};
    std::uint64_t directory_hash_encoding_bytes {};
    std::uint64_t directory_block_header_bytes {};
    std::uint64_t directory_bitmap_bytes {};
    std::uint64_t directory_posting_length_bytes {};
    std::uint64_t directory_occurrence_count_bytes {};
    std::uint64_t directory_distinct_game_count_bytes {};
    std::uint64_t directory_min_ply_bytes {};
    std::uint64_t directory_max_ply_bytes {};
    std::uint64_t directory_equal_occurrence_game_count {};
    std::uint64_t directory_hash_delta_over_32_bits {};
    std::uint64_t directory_hash_delta_over_40_bits {};
    std::uint64_t directory_hash_delta_over_48_bits {};
    std::uint64_t peak_temp_bytes {};
    std::uint64_t final_artifact_bytes {};
    std::chrono::milliseconds replay_elapsed {};
    std::chrono::milliseconds spill_elapsed {};
    std::chrono::milliseconds merge_elapsed {};
    std::chrono::milliseconds metadata_write_elapsed {};
    std::chrono::milliseconds directory_write_elapsed {};
    std::chrono::milliseconds final_write_elapsed {};
};

// Immutable exact occurrence index (version-6 compact format). Construction
// bounds record memory by spilling sorted runs and streams grouped posting
// blocks directly to the final staging artifact; its blocked directory uses
// 40-bit hash-delta lows with sparse high-part exceptions and derives posting
// offsets from block lengths. Lookup
// keeps only a sparse directory and per-game metadata table in memory and
// reads one compressed directory block plus one posting block per query.
class position_postings
{
  public:
    static constexpr std::size_t default_spill_threshold {1U << 20U};

    explicit position_postings(std::filesystem::path path, std::size_t spill_threshold = default_spill_threshold);
    // Removes any owned .spill*/.dirspool runs left behind by a build that
    // never reached a successful finalize() (e.g. append()/replay failed
    // before finalize() was even called).
    ~position_postings();
    position_postings(position_postings const&) = delete;
    auto operator=(position_postings const&) -> position_postings& = delete;
    position_postings(position_postings&& other) noexcept;
    auto operator=(position_postings&& other) noexcept -> position_postings&;

    [[nodiscard]] static auto build(game_store const& store,
                                    std::filesystem::path path,
                                    std::size_t spill_threshold = default_spill_threshold,
                                    position_postings_build_metrics* metrics = nullptr) -> result<void>;
    [[nodiscard]] auto append(std::span<position_row const> rows) -> result<void>;
    [[nodiscard]] auto finalize() -> result<void>;
    [[nodiscard]] auto open() -> result<void>;
    [[nodiscard]] auto occurrences(zobrist_hash hash, std::size_t limit = 0, std::size_t offset = 0) const
        -> result<std::vector<position_match>>;
    // Returns the first (lowest-ply) occurrence for each requested game ID.
    // requested_game_ids need not be sorted; output follows canonical game-ID
    // order and omits IDs that do not reach hash.
    [[nodiscard]] auto first_occurrences(zobrist_hash hash, std::span<game_id const> requested_game_ids) const
        -> result<std::vector<position_match>>;
    // The ascending ids of the distinct LIVE games that reach hash (limit == 0 means no limit).
    [[nodiscard]] auto distinct_game_ids(zobrist_hash hash) const -> result<std::vector<game_id>>;
    // A page of that list: the live games [offset, offset + limit) in ascending id order, so retired
    // games count towards neither. The whole immutable posting block is decoded and validated, and every
    // game it names must have metadata, but only the page is retained (limit == 0 means no limit).
    [[nodiscard]] auto distinct_game_ids(zobrist_hash hash, std::size_t limit, std::size_t offset) const -> result<std::vector<game_id>>;
    // Exact over the LIVE games: retired (removed) documents are excluded from
    // every count and from the ply envelope, so the result equals a fresh
    // build of the surviving games. nullopt when no live game reaches hash.
    // Costs one directory block read while nothing is retired; once a document
    // is retired the hash's posting block is decoded as well, and the answer is
    // remembered (a fixed number of hashes, about a megabyte, dropped whenever the retired
    // set changes) so a hot position pays that decode once, not per query.
    [[nodiscard]] auto summary(zobrist_hash hash) const -> result<std::optional<position_postings_summary>>;
    // The exact extrema of one tree edge over the live games (see position_edge_scan):
    // two posting blocks are read whole and joined by game id, ignoring retired
    // documents, so it costs the two blocks' bytes and no allocation beyond them.
    // nullopt when no live game other than `excluded` reaches the edge. The join may end as
    // soon as the wanted extrema are settled, but both blocks are always validated to the end
    // (framing, the entry's occurrence count and ply envelope), so a corrupt block is an error
    // whatever the scan found.
    [[nodiscard]] auto edge_extrema(position_edge_scan const& scan) const -> result<std::optional<position_edge_extrema>>;
    // Live games: the artifact's documents minus the retired ones.
    [[nodiscard]] auto indexed_game_count() const noexcept -> std::uint64_t;

    // Documents the metadata overlay has retired (removed games).
    [[nodiscard]] auto retired_game_count() const noexcept -> std::uint64_t { return retired_count_; }

    // Metadata overlay. The artifact's embedded per-game metadata is the
    // baseline; the certified chain of after-images is applied on top.
    //
    // apply_metadata_overlay: open-time materialization of a whole chain
    // (records sorted ascending and unique by id). All-or-nothing: every record
    // is validated (known document, revision 1, result in range) before any
    // slot is assigned.
    [[nodiscard]] auto apply_metadata_overlay(std::span<position_metadata_after_image const> sorted_records) -> result<void>;
    // prepare/install: the per-edit pair. prepare verifies the document and
    // locates its slot (it may fail, and changes nothing); install then assigns
    // scalar fields only -- no search, allocation, I/O or failure -- so it can
    // run after the durable commit. Callers serialize both against readers.
    [[nodiscard]] auto prepare_metadata_update(position_metadata_after_image const& image) const -> result<position_metadata_slot>;
    void install_metadata_update(position_metadata_slot const& slot) noexcept;
    // The effective metadata of one live game (baseline plus anything installed);
    // nullopt for an unknown or retired document.
    [[nodiscard]] auto metadata_for(game_id game) const -> std::optional<position_metadata_after_image>;
    // Every live game's effective metadata in ascending id order (retired
    // documents are skipped).
    void for_each_metadata(std::function<void(position_metadata_after_image const&)> const& visitor) const;

    // Streams every distinct hash with at least one live game in strictly
    // ascending order together with its live summary. While nothing is retired
    // it decodes only compressed directory blocks (never posting payloads); once
    // a document is retired each hash's posting block is decoded to exclude it.
    // The callback must not retain any reader-owned view.
    [[nodiscard]] auto for_each_summary(std::function<result<void>(zobrist_hash, position_postings_summary const&)> const& visitor) const
        -> result<void>;

    // Reader-owned sparse directory entry: one per compressed directory block.
    struct sparse_entry
    {
        zobrist_hash first_hash {};
        std::uint64_t block_offset {};
        std::uint32_t block_byte_length {};
        std::uint16_t entry_count {};
    };

  private:
    struct record
    {
        zobrist_hash hash {};
        game_id game_key {};
        std::uint16_t ply {};

        constexpr auto operator<=>(record const& other) const noexcept -> std::strong_ordering
        {
            if (auto const comparison = hash <=> other.hash; std::is_neq(comparison)) {
                return comparison;
            }
            if (auto const comparison = game_key <=> other.game_key; std::is_neq(comparison)) {
                return comparison;
            }
            return ply <=> other.ply;
        }

        constexpr auto operator==(record const& other) const noexcept -> bool
        {
            return hash == other.hash && game_key == other.game_key && ply == other.ply;
        }
    };

    struct game_metadata
    {
        std::int8_t result {};
        std::optional<std::int16_t> white_elo;
        std::optional<std::int16_t> black_elo;
        bool is_set {false};
    };

    // Reader-owned game metadata table entry (sorted ascending by game id).
    struct metadata_record
    {
        game_id id {};
        std::int8_t result {};
        std::optional<std::int16_t> white_elo;
        std::optional<std::int16_t> black_elo;
        bool retired {false};
    };

    // True when `id` names a retired document of `table`. `cursor` is a lower
    // bound the caller advances monotonically while walking ascending ids, so a
    // whole posting block costs one binary search per game from the last hit.
    [[nodiscard]] static auto probe_retired(std::vector<metadata_record> const& table,
                                            std::vector<metadata_record>::const_iterator& cursor,
                                            game_id game_key) noexcept -> bool;

    struct live_summary_cache;

    [[nodiscard]] auto spill_current_buffer() -> result<void>;
    [[nodiscard]] auto merge_runs() -> result<void>;

    std::filesystem::path path_;
    std::size_t spill_threshold_ {default_spill_threshold};
    std::vector<record> records_;
    gtl::flat_hash_map<std::uint32_t, game_metadata> game_metadata_;
    std::vector<std::filesystem::path> spill_paths_;
    std::uint64_t indexed_game_count_ {};
    std::uint64_t retired_count_ {};
    // Bumped whenever the retired set changes; a cache entry from another epoch is stale.
    std::uint64_t retired_epoch_ {};
    std::unique_ptr<live_summary_cache> live_summaries_;
    position_postings_build_metrics metrics_;
    bool is_open_ {false};

    // Reader state, populated by open().
    std::vector<metadata_record> metadata_;
    std::vector<sparse_entry> sparse_directory_;
    std::uint64_t metadata_offset_ {};
    std::uint64_t directory_offset_ {};
    std::uint64_t directory_byte_length_ {};
    std::uint64_t sparse_offset_ {};
    std::uint64_t distinct_hash_count_ {};
    std::uint64_t occurrence_count_ {};
};

}  // namespace motif::db
