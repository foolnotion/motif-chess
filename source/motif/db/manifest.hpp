#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "motif/db/error.hpp"

namespace motif::db
{

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
};

// Create a new manifest with current UTC timestamp and game_count = 0.
auto make_manifest(std::string const& name) -> db_manifest;

auto write_manifest(std::filesystem::path const& path, db_manifest const& manifest) -> result<void>;

auto read_manifest(std::filesystem::path const& path) -> result<db_manifest>;

}  // namespace motif::db
