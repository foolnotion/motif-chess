#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "motif/db/error.hpp"

namespace motif::db
{

struct derived_index_manifest_entry
{
    std::string filename;
    std::uint64_t source_generation {};
    std::uint64_t game_count {};
    std::uint64_t file_size {};
    std::uint64_t checksum {};
};

struct db_manifest
{
    std::string name;
    std::uint32_t schema_version {1};
    std::uint64_t game_count {0};
    std::string created_at;  // ISO 8601, e.g. "2026-04-18T14:30:00Z"
    // Set to true when a session begins; cleared to false on clean close.
    // A true value on open means the previous session did not close cleanly;
    // the position index will be rebuilt to guarantee consistency.
    bool position_index_dirty {false};
    std::uint64_t source_generation {};
    // Monotonic counter embedded in each freshly built derived-index
    // filename (e.g. "positions.postings.7") so a rebuild never writes over
    // the currently-published artifact -- see publish_derived_index() in
    // database_manager.cpp. Persisted so it survives across sessions and
    // never repeats, even after a rebuild that fails partway through.
    std::uint64_t derived_index_build_seq {};
    std::optional<derived_index_manifest_entry> position_postings;
    std::optional<derived_index_manifest_entry> opening_tree_index;
};

enum class manifest_write_state : std::uint8_t
{
    not_published,
    published,
    published_not_durable,
};

struct manifest_write_result
{
    manifest_write_state state {manifest_write_state::not_published};
    motif::db::error failure {error_code::ok};

    [[nodiscard]] auto has_value() const noexcept -> bool { return state == manifest_write_state::published; }

    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] auto was_published() const noexcept -> bool { return state != manifest_write_state::not_published; }

    [[nodiscard]] auto error() const noexcept -> motif::db::error const& { return failure; }
};

// Create a new manifest with current UTC timestamp and game_count = 0.
auto make_manifest(std::string const& name) -> db_manifest;

auto write_manifest(std::filesystem::path const& path, db_manifest const& manifest) -> manifest_write_result;

auto read_manifest(std::filesystem::path const& path) -> result<db_manifest>;

}  // namespace motif::db
