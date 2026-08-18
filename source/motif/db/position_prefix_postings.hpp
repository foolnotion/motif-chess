#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

// Prototype sidecar for exact deep-position candidate selection. It stores
// game IDs by a Zobrist-hash prefix; callers must replay candidates and verify
// the complete hash before including them in a query result.
//
// append() buffers records in memory only up to spill_threshold records; once
// exceeded, the buffer is sorted and spilled to a temporary run file on disk
// and cleared, bounding peak memory against corpus size. finalize() merges any
// spilled runs (plus a final in-memory run) via a streaming k-way merge.
class position_prefix_postings
{
  public:
    static constexpr std::size_t default_spill_threshold {1U << 20U};

    position_prefix_postings(std::filesystem::path path, std::uint8_t prefix_bits, std::size_t spill_threshold = default_spill_threshold);

    [[nodiscard]] auto append(std::span<position_row const> rows) -> result<void>;
    [[nodiscard]] auto finalize() -> result<void>;
    [[nodiscard]] auto open() -> result<void>;
    [[nodiscard]] auto candidates(zobrist_hash hash) const -> result<std::vector<game_id>>;

  private:
    struct record
    {
        std::uint32_t prefix {};
        game_id game_key {};

        constexpr auto operator<=>(record const&) const noexcept = default;
    };

    [[nodiscard]] auto prefix(zobrist_hash hash) const noexcept -> std::uint32_t;
    [[nodiscard]] auto spill_current_buffer() -> result<void>;
    [[nodiscard]] auto finalize_in_memory() -> result<void>;
    [[nodiscard]] auto finalize_external_merge() -> result<void>;

    std::filesystem::path path_;
    std::uint8_t prefix_bits_ {};
    std::size_t spill_threshold_ {default_spill_threshold};
    std::vector<record> records_;
    std::vector<std::filesystem::path> spill_paths_;
    std::vector<std::uint64_t> offsets_;
    std::vector<game_id> postings_;
};

}  // namespace motif::db
