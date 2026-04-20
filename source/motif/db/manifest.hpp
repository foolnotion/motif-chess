// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "motif/db/error.hpp"

namespace motif::db {

struct db_manifest {
    std::string   name;
    std::uint32_t schema_version{1};
    std::uint64_t game_count{0};
    std::string   created_at; // ISO 8601, e.g. "2026-04-18T14:30:00Z"
};

// Create a new manifest with current UTC timestamp and game_count = 0.
auto make_manifest(std::string const& name) -> db_manifest;

auto write_manifest(std::filesystem::path const& path,
                    db_manifest const& manifest) -> result<void>;

auto read_manifest(std::filesystem::path const& path) -> result<db_manifest>;

} // namespace motif::db
