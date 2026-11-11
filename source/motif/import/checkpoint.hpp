#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "motif/import/error.hpp"

namespace motif::import
{

// Marks an interrupted import of source_path. It is written before an import
// commits its first game and remains until the import fully succeeds. On POSIX,
// file and directory fsync protect this marker across power loss; on Windows,
// the existing directory-sync implementation is best-effort only. It is NOT a
// resume cursor: SQLite commits at synchronous=NORMAL can roll back to an
// earlier WAL prefix after a power failure while this file survives. Nothing in
// the file proves that the batch it describes is still in the database:
// counters and fingerprints can be recreated by unrelated writes after a lost
// batch, so resume never seeks to byte_offset. It re-reads the source from the
// start with checked (deduplicating) inserts: games that survived are skipped as duplicates, lost
// ones are imported, and no game is skipped. byte_offset, games_committed and
// last_game_id only report how far the interrupted run got. Reading ignores
// unknown keys, so files written by builds with more fields still load.
struct import_checkpoint
{
    std::string source_path;
    std::size_t byte_offset {};
    std::int64_t games_committed {};
    std::int64_t last_game_id {};
};

// Path to <db_dir>/import.checkpoint.json
[[nodiscard]] auto checkpoint_path(std::filesystem::path const& db_dir) -> std::filesystem::path;

// Serialize cp to <db_dir>/import.checkpoint.json via glaze, replacing the
// previous checkpoint only after the new file is written and synchronized.
// The parent directory is synchronized on POSIX; Windows directory durability
// is not guaranteed by the current filesystem implementation.
[[nodiscard]] auto write_checkpoint(std::filesystem::path const& db_dir, import_checkpoint const& checkpoint) -> result<void>;

// Read and deserialize checkpoint file. Returns not_found if absent.
[[nodiscard]] auto read_checkpoint(std::filesystem::path const& db_dir) -> result<import_checkpoint>;

// Remove checkpoint file. Silently succeeds if already absent.
auto delete_checkpoint(std::filesystem::path const& db_dir) noexcept -> void;

}  // namespace motif::import
