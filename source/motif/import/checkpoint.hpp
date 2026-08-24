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
    // Minimal source identity captured at write time: size in bytes and last
    // write time (nanoseconds since the filesystem clock epoch). Compared
    // against the source file's current stat at resume time to catch the
    // source being edited or replaced at the same path before resuming --
    // without hashing file content or adding a dependency.
    std::uint64_t source_size {};
    std::int64_t source_mtime_ns {};
    // Stable dependency-free hash of the complete source contents. Size and
    // mtime are cheap diagnostics; this fingerprint is the authoritative
    // same-path replacement check.
    std::uint64_t source_content_hash {};
};

// Minimal, portable identity of a source file: size and last write time
// (nanoseconds since the filesystem clock's epoch). Not meaningful across
// machines or filesystems -- only used to detect in-place mutation or
// replacement of a source file at the same path within a single run.
struct source_stat
{
    std::uint64_t size {};
    std::int64_t mtime_ns {};
};

// Stat source_path for source_stat. Returns io_failure if the file cannot
// be stat'd.
[[nodiscard]] auto stat_source(std::filesystem::path const& source_path) -> result<source_stat>;

// Compute a stable FNV-1a fingerprint over the complete source file.
[[nodiscard]] auto hash_source(std::filesystem::path const& source_path) -> result<std::uint64_t>;

// Path to <db_dir>/import.checkpoint.json
[[nodiscard]] auto checkpoint_path(std::filesystem::path const& db_dir) -> std::filesystem::path;

// Serialize cp to <db_dir>/import.checkpoint.json via glaze, replacing the
// previous checkpoint only after the new file has been fully written.
[[nodiscard]] auto write_checkpoint(std::filesystem::path const& db_dir, import_checkpoint const& checkpoint) -> result<void>;

// Read and deserialize checkpoint file. Returns not_found if absent.
[[nodiscard]] auto read_checkpoint(std::filesystem::path const& db_dir) -> result<import_checkpoint>;

// Remove checkpoint file. Silently succeeds if already absent.
auto delete_checkpoint(std::filesystem::path const& db_dir) noexcept -> void;

}  // namespace motif::import
