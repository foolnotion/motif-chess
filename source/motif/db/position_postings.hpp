#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <gtl/phmap.hpp>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

class game_store;

struct position_postings_summary
{
    std::uint64_t occurrence_count {};
    std::uint32_t distinct_game_count {};
    std::uint16_t min_ply {};
    std::uint16_t max_ply {};
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
    [[nodiscard]] auto distinct_game_ids(zobrist_hash hash) const -> result<std::vector<game_id>>;
    [[nodiscard]] auto summary(zobrist_hash hash) const -> result<std::optional<position_postings_summary>>;
    [[nodiscard]] auto indexed_game_count() const noexcept -> std::uint64_t;

    // Streams every distinct hash in strictly ascending order together with
    // its summary, decoding only compressed directory blocks (never posting
    // payloads). The callback must not retain any reader-owned view.
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
            if (auto const comparison = hash <=> other.hash; comparison != 0) {
                return comparison;
            }
            if (auto const comparison = game_key <=> other.game_key; comparison != 0) {
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
    };

    [[nodiscard]] auto spill_current_buffer() -> result<void>;
    [[nodiscard]] auto merge_runs() -> result<void>;

    std::filesystem::path path_;
    std::size_t spill_threshold_ {default_spill_threshold};
    std::vector<record> records_;
    gtl::flat_hash_map<std::uint32_t, game_metadata> game_metadata_;
    std::vector<std::filesystem::path> spill_paths_;
    std::uint64_t indexed_game_count_ {};
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
