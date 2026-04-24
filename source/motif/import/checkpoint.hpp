// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "motif/import/error.hpp"

namespace motif::import
{

struct import_checkpoint
{
    std::string source_path;
    std::size_t byte_offset {};
    std::int64_t games_committed {};
    std::int64_t last_game_id {};
};

// Path to <db_dir>/import.checkpoint.json
[[nodiscard]] auto checkpoint_path(std::filesystem::path const& db_dir) -> std::filesystem::path;

// Serialize cp to <db_dir>/import.checkpoint.json via glaze.
[[nodiscard]] auto write_checkpoint(std::filesystem::path const& db_dir, import_checkpoint const& checkpoint) -> result<void>;

// Read and deserialize checkpoint file. Returns not_found if absent.
[[nodiscard]] auto read_checkpoint(std::filesystem::path const& db_dir) -> result<import_checkpoint>;

// Remove checkpoint file. Silently succeeds if already absent.
auto delete_checkpoint(std::filesystem::path const& db_dir) noexcept -> void;

}  // namespace motif::import
