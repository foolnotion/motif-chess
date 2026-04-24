// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <duckdb.h>

#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/position_store.hpp"

struct sqlite3;

namespace motif::db
{

// Owns the lifecycle of a named chess database bundle:
//   <dir>/games.db        — SQLite WAL (game metadata and moves)
//   <dir>/positions.duckdb — DuckDB position index
//   <dir>/manifest.json   — glaze-serialized bundle metadata
//
// Obtain instances via the create() or open() factory methods.
class database_manager
{
  public:
    ~database_manager();

    database_manager(database_manager const&) = delete;
    auto operator=(database_manager const&) -> database_manager& = delete;
    database_manager(database_manager&& other) noexcept;
    auto operator=(database_manager&& other) noexcept -> database_manager&;

    // Create a new bundle at dir. Fails with io_failure if games.db already
    // exists at that path.
    static auto create(std::filesystem::path const& dir,
                       std::string const& name) -> result<database_manager>;

    // Open an existing bundle. Fails with not_found if games.db or
    // manifest.json are absent; fails with schema_mismatch if PRAGMA
    // user_version does not match schema::k_version.
    static auto open(std::filesystem::path const& dir)
        -> result<database_manager>;

    // Access the SQLite game store for CRUD operations.
    [[nodiscard]] auto store() noexcept -> game_store&;
    [[nodiscard]] auto store() const noexcept -> game_store const&;

    // Access the DuckDB position store.
    [[nodiscard]] auto positions() noexcept -> position_store&;
    [[nodiscard]] auto positions() const noexcept -> position_store const&;

    // Access the bundle manifest (read-only; updated internally on close).
    [[nodiscard]] auto manifest() const noexcept -> db_manifest const&;

    // Directory containing the database bundle files.
    [[nodiscard]] auto dir() const noexcept -> std::filesystem::path const&;

    // Drop and repopulate the DuckDB position table from all games in SQLite.
    auto rebuild_position_store(bool sort_by_zobrist = true) -> result<void>;

    // Release all connections and clear internal state.
    // Safe to call multiple times.
    void close() noexcept;

  private:
    database_manager() = default;

    sqlite3* conn_ {nullptr};
    std::optional<game_store> store_;
    db_manifest manifest_;
    std::filesystem::path dir_;
    duckdb_database duck_db_ {nullptr};
    duckdb_connection duck_con_ {nullptr};
    std::optional<position_store> positions_;
};

}  // namespace motif::db
