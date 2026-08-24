#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "motif/db/database_manager.hpp"

#include <duckdb.h>
#include <gtl/meminfo.hpp>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/position_store.hpp"
#include "motif/db/schema.hpp"
#include "motif/db/types.hpp"

namespace motif::db
{

// ── Helpers
// ───────────────────────────────────────────────────────────────────

namespace
{
constexpr std::size_t rebuild_batch_rows = 50'000;

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto open_sqlite(std::filesystem::path const& db_path) -> result<sqlite3*>
{
    sqlite3* conn = nullptr;
    int const ret = sqlite3_open(db_path.c_str(), &conn);
    if (ret != SQLITE_OK) {
        if (conn != nullptr) {
            sqlite3_close(conn);
        }
        return tl::unexpected {error_code::io_failure};
    }
    // synchronous=FULL: a commit is not durable until fsynced, which is what
    // makes "checkpoint written after commit" a sound ordering under power
    // loss. synchronous=NORMAL under WAL only fsyncs at checkpoint boundaries,
    // so an import.checkpoint.json write (itself fsynced via rename-replace)
    // could persist and be read back on the next resume() while the SQLite
    // commit it describes was still only in the OS page cache and lost in a
    // crash -- silently skipping games on resume. cache_size/mmap_size raise
    // the working-set that can stay resident as games.db grows into the
    // multi-GB range; wal_autocheckpoint raised from the 1000-page default
    // reduces mid-import checkpoint stalls at the cost of a larger WAL file
    // between checkpoints.
    sqlite3_exec(conn, "PRAGMA synchronous = FULL;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "PRAGMA cache_size = -200000;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "PRAGMA mmap_size = 30000000000;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "PRAGMA wal_autocheckpoint = 10000;", nullptr, nullptr, nullptr);
    return conn;
}

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto enable_foreign_keys(sqlite3* conn) -> result<void>
{
    int const ret = sqlite3_exec(conn, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    if (ret != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(conn, "PRAGMA foreign_keys;", -1, &raw, nullptr) != SQLITE_OK) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const guard = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> {raw, sqlite3_finalize};
    if (sqlite3_step(guard.get()) != SQLITE_ROW) {
        return tl::unexpected {error_code::io_failure};
    }
    if (sqlite3_column_int(guard.get(), 0) != 1) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto path_exists(std::filesystem::path const& path) -> result<bool>
{
    std::error_code fs_err;
    auto const exists = std::filesystem::exists(path, fs_err);
    if (fs_err) {
        return tl::unexpected {error_code::io_failure};
    }
    return exists;
}

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto map_result(std::string const& pgn_result) -> std::int8_t
{
    if (pgn_result == "1-0") {
        return 1;
    }
    if (pgn_result == "0-1") {
        return -1;
    }
    return 0;
}

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto narrow_elo(std::optional<std::int32_t> const& elo) -> result<std::optional<std::int16_t>>
{
    if (!elo.has_value()) {
        return std::optional<std::int16_t> {};
    }
    if (*elo < std::numeric_limits<std::int16_t>::min() || *elo > std::numeric_limits<std::int16_t>::max()) {
        return tl::unexpected {error_code::io_failure};
    }
    return std::optional<std::int16_t> {static_cast<std::int16_t>(*elo)};
}

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto build_position_rows(std::span<std::uint16_t const> const moves,
                         motif::db::game_id const game_id,
                         std::int8_t const result_code,
                         std::optional<std::int32_t> const& white_elo,
                         std::optional<std::int32_t> const& black_elo) -> result<std::vector<position_row>>
{
    if (moves.empty()) {
        return std::vector<position_row> {};
    }

    if (moves.size() > std::numeric_limits<std::uint16_t>::max()) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const narrowed_white_elo = narrow_elo(white_elo);
    if (!narrowed_white_elo) {
        return tl::unexpected {narrowed_white_elo.error()};
    }
    auto const narrowed_black_elo = narrow_elo(black_elo);
    if (!narrowed_black_elo) {
        return tl::unexpected {narrowed_black_elo.error()};
    }

    auto board = motif::chess::board {};
    std::vector<position_row> batch;
    batch.reserve(moves.size() + 1);

    // Starting position row (ply = 0) so root-hash queries find data
    batch.push_back(position_row {
        .zobrist_hash = motif::db::zobrist_hash {board.hash()},
        .game_id = game_id,
        .ply = 0,
        .encoded_move = 0,
        .result = result_code,
        .white_elo = *narrowed_white_elo,
        .black_elo = *narrowed_black_elo,
    });

    for (std::size_t i = 0; i < moves.size(); ++i) {
        motif::chess::apply_encoded_move(board, moves[i]);
        batch.push_back(position_row {
            .zobrist_hash = motif::db::zobrist_hash {board.hash()},
            .game_id = game_id,
            .ply = static_cast<std::uint16_t>(i + 1),
            .encoded_move = moves[i],
            .result = result_code,
            .white_elo = *narrowed_white_elo,
            .black_elo = *narrowed_black_elo,
        });
    }

    return batch;
}

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto cleanup_failed_create(std::filesystem::path const& dir,
                           std::filesystem::path const& db_path,
                           std::filesystem::path const& duck_path,
                           std::filesystem::path const& manifest_path,
                           bool created_dir) noexcept -> void
{
    std::error_code fs_err;
    std::filesystem::remove(manifest_path, fs_err);
    fs_err.clear();
    std::filesystem::remove(db_path, fs_err);
    fs_err.clear();
    std::filesystem::remove(db_path.string() + "-wal", fs_err);
    fs_err.clear();
    std::filesystem::remove(db_path.string() + "-shm", fs_err);
    fs_err.clear();
    std::filesystem::remove(duck_path, fs_err);
    if (created_dir) {
        fs_err.clear();
        std::filesystem::remove(dir, fs_err);
    }
}

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto open_duckdb(std::filesystem::path const& duck_path, duckdb_database& out_db, duckdb_connection& out_con) -> result<void>
{
    if (duckdb_open(duck_path.c_str(), &out_db) == DuckDBError) {
        return tl::unexpected {error_code::io_failure};
    }
    if (duckdb_connect(out_db, &out_con) == DuckDBError) {
        duckdb_close(&out_db);
        out_db = nullptr;
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

}  // namespace

// ── Lifecycle
// ─────────────────────────────────────────────────────────────────

database_manager::~database_manager()
{
    close();
}

database_manager::database_manager(database_manager&& other) noexcept
    : conn_ {std::exchange(other.conn_, nullptr)}
    , store_ {std::move(other.store_)}
    , writer_ {std::move(other.writer_)}
    , manifest_ {std::move(other.manifest_)}
    , dir_ {std::move(other.dir_)}
    , duck_db_ {std::exchange(other.duck_db_, nullptr)}
    , duck_con_ {std::exchange(other.duck_con_, nullptr)}
    , positions_ {std::move(other.positions_)}
{
    other.store_.reset();
    other.writer_.reset();
    other.positions_.reset();
}

auto database_manager::operator=(database_manager&& other) noexcept -> database_manager&
{
    if (this != &other) {
        close();
        conn_ = std::exchange(other.conn_, nullptr);
        store_ = std::move(other.store_);
        writer_ = std::move(other.writer_);
        manifest_ = std::move(other.manifest_);
        dir_ = std::move(other.dir_);
        duck_db_ = std::exchange(other.duck_db_, nullptr);
        duck_con_ = std::exchange(other.duck_con_, nullptr);
        positions_ = std::move(other.positions_);
        other.store_.reset();
        other.writer_.reset();
        other.positions_.reset();
    }
    return *this;
}

void database_manager::close() noexcept
{
    // Refresh game_count and mark clean before tearing down connections.
    // Best-effort — errors are swallowed since close() is noexcept.
    if (!dir_.empty() && store_ && conn_ != nullptr) {
        if (auto count_res = store_->count_games(); count_res) {
            manifest_.game_count = static_cast<std::uint64_t>(*count_res);
        }
        manifest_.position_index_dirty = false;
        (void)write_manifest(dir_ / "manifest.json", manifest_);
    }

    positions_.reset();
    writer_.reset();
    if (duck_con_ != nullptr) {
        duckdb_disconnect(&duck_con_);
        duck_con_ = nullptr;
    }
    if (duck_db_ != nullptr) {
        duckdb_close(&duck_db_);
        duck_db_ = nullptr;
    }
    store_.reset();
    if (conn_ != nullptr) {
        sqlite3_close(conn_);
        conn_ = nullptr;
    }
}

// ── Factory methods
// ───────────────────────────────────────────────────────────

auto database_manager::create(std::filesystem::path const& dir, std::string const& name) -> result<database_manager>
{
    auto const db_path = dir / "games.db";
    auto const duck_path = dir / "positions.duckdb";
    auto const manifest_path = dir / "manifest.json";

    auto db_exists = path_exists(db_path);
    if (!db_exists) {
        return tl::unexpected {db_exists.error()};
    }
    if (*db_exists) {
        return tl::unexpected {error_code::io_failure};
    }

    std::error_code fs_err;
    auto const created_dir = std::filesystem::create_directories(dir, fs_err);
    if (fs_err) {
        return tl::unexpected {error_code::io_failure};
    }

    auto conn_res = open_sqlite(db_path);
    if (!conn_res) {
        return tl::unexpected {conn_res.error()};
    }
    sqlite3* conn = *conn_res;

    auto init_res = schema::initialize(conn);
    if (!init_res) {
        sqlite3_close(conn);
        cleanup_failed_create(dir, db_path, duck_path, manifest_path, created_dir);
        return tl::unexpected {init_res.error()};
    }

    auto new_manifest = make_manifest(name);
    auto write_res = write_manifest(manifest_path, new_manifest);
    if (!write_res) {
        sqlite3_close(conn);
        cleanup_failed_create(dir, db_path, duck_path, manifest_path, created_dir);
        return tl::unexpected {write_res.error()};
    }

    database_manager mgr;
    mgr.conn_ = conn;
    mgr.store_.emplace(conn);
    mgr.writer_.emplace(conn);
    mgr.manifest_ = std::move(new_manifest);
    mgr.dir_ = dir;

    auto duck_res = open_duckdb(duck_path, mgr.duck_db_, mgr.duck_con_);
    if (!duck_res) {
        mgr.close();
        cleanup_failed_create(dir, db_path, duck_path, manifest_path, created_dir);
        return tl::unexpected {duck_res.error()};
    }
    mgr.positions_.emplace(mgr.duck_con_);
    if (auto schema_res = mgr.positions_->initialize_schema(); !schema_res) {
        mgr.close();
        cleanup_failed_create(dir, db_path, duck_path, manifest_path, created_dir);
        return tl::unexpected {schema_res.error()};
    }

    // Mark in-use so that a crash before the first close() triggers a rebuild.
    mgr.manifest_.position_index_dirty = true;
    if (auto dirty_write_res = write_manifest(manifest_path, mgr.manifest_); !dirty_write_res) {
        mgr.close();
        cleanup_failed_create(dir, db_path, duck_path, manifest_path, created_dir);
        return tl::unexpected {dirty_write_res.error()};
    }

    return mgr;
}

auto database_manager::open(std::filesystem::path const& dir) -> result<database_manager>
{
    auto const db_path = dir / "games.db";
    auto const manifest_path = dir / "manifest.json";

    auto db_exists = path_exists(db_path);
    if (!db_exists) {
        return tl::unexpected {db_exists.error()};
    }
    if (!*db_exists) {
        return tl::unexpected {error_code::not_found};
    }
    auto manifest_exists = path_exists(manifest_path);
    if (!manifest_exists) {
        return tl::unexpected {manifest_exists.error()};
    }
    if (!*manifest_exists) {
        return tl::unexpected {error_code::not_found};
    }

    auto conn_res = open_sqlite(db_path);
    if (!conn_res) {
        return tl::unexpected {conn_res.error()};
    }
    sqlite3* conn = *conn_res;

    auto fk_res = enable_foreign_keys(conn);
    if (!fk_res) {
        sqlite3_close(conn);
        return tl::unexpected {fk_res.error()};
    }

    auto ver_res = schema::version(conn);
    if (!ver_res) {
        sqlite3_close(conn);
        return tl::unexpected {ver_res.error()};
    }
    if (*ver_res > schema::current_version) {
        sqlite3_close(conn);
        return tl::unexpected {error_code::schema_mismatch};
    }
    if (*ver_res < schema::current_version) {
        auto mig_res = schema::migrate(conn, *ver_res);
        if (!mig_res) {
            sqlite3_close(conn);
            return tl::unexpected {mig_res.error()};
        }
    }

    auto mf_res = read_manifest(manifest_path);
    if (!mf_res) {
        sqlite3_close(conn);
        return tl::unexpected {mf_res.error()};
    }

    database_manager mgr;
    mgr.conn_ = conn;
    mgr.store_.emplace(conn);
    mgr.writer_.emplace(conn);
    mgr.manifest_ = std::move(*mf_res);
    mgr.dir_ = dir;

    auto duck_res = open_duckdb(dir / "positions.duckdb", mgr.duck_db_, mgr.duck_con_);
    if (!duck_res) {
        mgr.close();
        return tl::unexpected {duck_res.error()};
    }
    mgr.positions_.emplace(mgr.duck_con_);
    // initialize_schema is idempotent — creates table if missing on first open
    if (auto schema_res = mgr.positions_->initialize_schema(); !schema_res) {
        mgr.close();
        return tl::unexpected {schema_res.error()};
    }

    // A missing game_identity_lookup index means a prior raw bulk ingest was
    // interrupted before deduplicate() could restore it (see
    // motif::import::import_pipeline). Repair the raw rows before any
    // derived position data is rebuilt or reads are exposed on this handle.
    auto const index_exists = mgr.writer_->identity_index_exists();
    if (!index_exists) {
        mgr.close();
        return tl::unexpected {index_exists.error()};
    }
    if (!*index_exists) {
        if (auto dedup_res = mgr.writer_->deduplicate(); !dedup_res) {
            mgr.close();
            return tl::unexpected {error_code::io_failure};
        }
        // Raw ingest may have left duplicate rows reflected in the position
        // store from an earlier partial rebuild; force a rebuild so it
        // reflects only the deduplicated, canonical games.
        mgr.manifest_.position_index_dirty = true;
    }

    // If the previous session did not close cleanly (crash, SIGKILL, etc.),
    // rebuild the position index from SQLite to guarantee consistency.
    if (mgr.manifest_.position_index_dirty) {
        if (auto rebuild_res = mgr.rebuild_position_store(); !rebuild_res) {
            // close() would clear the dirty flag on disk, which would hide the
            // inconsistency from the next open().  Re-write it as true afterward.
            mgr.close();
            mgr.manifest_.position_index_dirty = true;
            (void)write_manifest(manifest_path, mgr.manifest_);
            return tl::unexpected {rebuild_res.error()};
        }
    }

    // Mark in-use: a crash before close() will leave this set and trigger
    // a rebuild on the next open().
    mgr.manifest_.position_index_dirty = true;
    if (auto write_res = write_manifest(manifest_path, mgr.manifest_); !write_res) {
        mgr.close();
        return tl::unexpected {write_res.error()};
    }

    return mgr;
}

auto database_manager::create_scratch() -> result<database_manager>
{
    sqlite3* conn = nullptr;
    if (sqlite3_open(":memory:", &conn) != SQLITE_OK) {
        if (conn != nullptr) {
            sqlite3_close(conn);
        }
        return tl::unexpected {error_code::io_failure};
    }

    if (auto init_res = schema::initialize(conn); !init_res) {
        sqlite3_close(conn);
        return tl::unexpected {init_res.error()};
    }

    database_manager mgr;
    mgr.conn_ = conn;
    mgr.store_.emplace(conn);
    mgr.writer_.emplace(conn);
    mgr.manifest_ = make_manifest("scratch");
    // dir_ stays empty — close() skips manifest write for in-memory instances

    if (duckdb_open(nullptr, &mgr.duck_db_) == DuckDBError) {
        mgr.close();
        return tl::unexpected {error_code::io_failure};
    }
    if (duckdb_connect(mgr.duck_db_, &mgr.duck_con_) == DuckDBError) {
        mgr.close();
        return tl::unexpected {error_code::io_failure};
    }
    mgr.positions_.emplace(mgr.duck_con_);
    if (auto schema_res = mgr.positions_->initialize_schema(); !schema_res) {
        mgr.close();
        return tl::unexpected {schema_res.error()};
    }

    return mgr;
}

// ── Accessors
// ─────────────────────────────────────────────────────────────────

auto database_manager::store() noexcept -> game_store&
{
    assert(store_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- asserted above
    return *store_;
}

auto database_manager::store() const noexcept -> game_store const&
{
    assert(store_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- asserted above
    return *store_;
}

auto database_manager::writer() noexcept -> game_writer&
{
    assert(writer_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- asserted above
    return *writer_;
}

auto database_manager::manifest() const noexcept -> db_manifest const&
{
    return manifest_;
}

auto database_manager::dir() const noexcept -> std::filesystem::path const&
{
    return dir_;
}

auto database_manager::positions() noexcept -> position_store&
{
    assert(positions_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- asserted above
    return *positions_;
}

auto database_manager::positions() const noexcept -> position_store const&
{
    assert(positions_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- asserted above
    return *positions_;
}

auto database_manager::patch_game_metadata(game_id const game_key, game_patch const& patch) -> result<void>
{
    if (!positions_ || !store_) {
        return tl::unexpected {error_code::io_failure};
    }

    // Validate and narrow elo values before touching any state.
    auto new_white = std::optional<std::int16_t> {};
    if (patch.white_elo) {
        auto res = narrow_elo(patch.white_elo);
        if (!res) {
            return tl::unexpected {res.error()};
        }
        new_white = *res;
    }
    auto new_black = std::optional<std::int16_t> {};
    if (patch.black_elo) {
        auto res = narrow_elo(patch.black_elo);
        if (!res) {
            return tl::unexpected {res.error()};
        }
        new_black = *res;
    }

    if (auto res = store_->patch_metadata(game_key, patch); !res) {
        return res;
    }

    if (new_white || new_black) {
        if (auto update_res = positions_->update_elo_for_game(game_key, new_white, new_black); !update_res) {
            return update_res;
        }
    }

    if (patch.result) {
        if (auto update_res = positions_->update_result_for_game(game_key, map_result(*patch.result)); !update_res) {
            return update_res;
        }
    }
    return {};
}

auto database_manager::remove_game(game_id const game_key) -> result<void>
{
    if (!positions_ || !store_) {
        return tl::unexpected {error_code::io_failure};
    }
    // Position rows are removed first: they can always be restored by
    // rebuild_position_store(), whereas a game deleted from SQLite cannot.
    // The two operations are not atomic across a crash boundary.
    if (auto res = positions_->delete_by_game_id(game_key); !res) {
        return res;
    }
    return store_->remove(game_key);
}

auto database_manager::remove_user_game(game_id const game_key) -> result<void>
{
    if (!positions_ || !store_) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const provenance = store_->get_provenance(game_key);
    if (!provenance) {
        return tl::unexpected {provenance.error()};
    }
    if (provenance->source_type != "manual") {
        return tl::unexpected {error_code::not_editable};
    }

    return remove_game(game_key);
}

auto database_manager::query_elo_distribution(zobrist_hash const hash, search_filter const& filter, int const bucket_width) const
    -> result<std::vector<elo_distribution_row>>
{
    if (!positions_ || !store_) {
        return tl::unexpected {error_code::io_failure};
    }

    if (bucket_width <= 0) {
        return tl::unexpected {error_code::invalid_argument};
    }

    bool const has_metadata = filter.player_name.has_value() || filter.min_elo.has_value() || filter.max_elo.has_value()
        || filter.result.has_value() || filter.eco_prefix.has_value();

    if (!has_metadata) {
        return positions_->query_elo_distribution(hash, bucket_width);
    }

    auto all_ids_res = positions_->distinct_game_ids_by_zobrist(hash);
    if (!all_ids_res) {
        return tl::unexpected {all_ids_res.error()};
    }
    if (all_ids_res->empty()) {
        return std::vector<elo_distribution_row> {};
    }

    auto meta_filter = filter;
    meta_filter.position = std::nullopt;

    auto filtered_ids_res = store_->find_game_ids_with_filter(*all_ids_res, meta_filter);
    if (!filtered_ids_res) {
        return tl::unexpected {filtered_ids_res.error()};
    }
    if (filtered_ids_res->empty()) {
        return std::vector<elo_distribution_row> {};
    }

    return positions_->query_elo_distribution(hash, *filtered_ids_res, bucket_width);
}

auto database_manager::find_games(search_filter const& filter) -> result<game_list_result>
{
    if (!positions_ || !store_) {
        return tl::unexpected {error_code::io_failure};
    }

    if (!filter.position.has_value()) {
        return store_->find_games(filter);
    }

    auto game_ids = positions_->distinct_game_ids_by_zobrist(*filter.position);
    if (!game_ids) {
        return tl::unexpected {game_ids.error()};
    }

    auto metadata_filter = filter;
    metadata_filter.position.reset();
    return store_->find_games_with_ids(*game_ids, metadata_filter);
}

auto database_manager::find_games_by_position(zobrist_hash const hash, std::size_t const limit)
    -> result<std::pair<game_list_result, std::vector<position_match>>>
{
    if (!positions_ || !store_) {
        return tl::unexpected {error_code::io_failure};
    }

    auto ply_matches = positions_->query_min_ply_by_game(hash, limit);
    if (!ply_matches) {
        return tl::unexpected {ply_matches.error()};
    }

    std::vector<game_id> ids;
    ids.reserve(ply_matches->size());
    for (auto const& match : *ply_matches) {
        ids.push_back(match.game_id);
    }

    search_filter filter;
    filter.limit = ids.size();
    auto games = store_->find_games_with_ids(ids, filter);
    if (!games) {
        return tl::unexpected {games.error()};
    }

    auto total = positions_->count_distinct_games_by_zobrist(hash);
    if (!total) {
        return tl::unexpected {total.error()};
    }
    games->total_count = *total;

    return std::make_pair(std::move(*games), std::move(*ply_matches));
}

auto database_manager::sort_positions(position_rebuild_timing* const timing) -> result<void>
{
    if (duck_con_ == nullptr || !positions_) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const connection_lock = positions_->lock_connection();

    duckdb_result tx_res {};
    if (duckdb_query(duck_con_, "BEGIN TRANSACTION", &tx_res) == DuckDBError) {
        duckdb_destroy_result(&tx_res);
        return tl::unexpected {error_code::io_failure};
    }
    duckdb_destroy_result(&tx_res);

    auto rollback = [this]() noexcept -> void
    {
        duckdb_result rollback_res {};
        duckdb_query(duck_con_, "ROLLBACK", &rollback_res);
        duckdb_destroy_result(&rollback_res);
    };

    auto const sort_started = std::chrono::steady_clock::now();
    if (auto sort_res = positions_->sort_by_zobrist(); !sort_res) {
        rollback();
        return tl::unexpected {error_code::io_failure};
    }
    if (timing != nullptr) {
        timing->sort_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sort_started);
    }
    auto const rollup_started = std::chrono::steady_clock::now();
    if (auto rollup_res = positions_->rebuild_opening_stats_rollups(); !rollup_res) {
        rollback();
        return tl::unexpected {rollup_res.error()};
    }
    if (timing != nullptr) {
        timing->rollup_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rollup_started);
    }

    duckdb_result commit_res {};
    if (duckdb_query(duck_con_, "COMMIT", &commit_res) == DuckDBError) {
        duckdb_destroy_result(&commit_res);
        rollback();
        return tl::unexpected {error_code::io_failure};
    }
    duckdb_destroy_result(&commit_res);

    return {};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto database_manager::rebuild_position_store(bool const sort_by_zobrist, position_rebuild_timing* const timing) -> result<void>
{
    if (duck_con_ == nullptr || !positions_) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const connection_lock = positions_->lock_connection();

    auto log = spdlog::get("motif.db");
    auto log_rss = [&](char const* const phase) -> void
    {
        if (log == nullptr) {
            return;
        }
        // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker)
        auto const rss_bytes = gtl::GetProcessMemoryUsed();
        log->info("rebuild_position_store rss {}: {} bytes", phase, rss_bytes);
    };

    log_rss("start");
    duckdb_result tx_res {};
    if (duckdb_query(duck_con_, "BEGIN TRANSACTION", &tx_res) == DuckDBError) {
        duckdb_destroy_result(&tx_res);
        return tl::unexpected {error_code::io_failure};
    }
    duckdb_destroy_result(&tx_res);

    auto rollback = [this]() noexcept -> void
    {
        duckdb_result rollback_res {};
        duckdb_query(duck_con_, "ROLLBACK", &rollback_res);
        duckdb_destroy_result(&rollback_res);
    };

    // Drop and recreate so that any schema changes (new columns) are applied,
    // and so a rebuild always reflects the current SQLite game.result/elo --
    // game_result's insert path uses ON CONFLICT DO NOTHING (see
    // insert_game_results_for_batch), which would otherwise silently keep a
    // stale row from before a patch_game_metadata() edit.
    duckdb_result drop_res {};
    if (duckdb_query(duck_con_, "DROP TABLE IF EXISTS position", &drop_res) == DuckDBError) {
        duckdb_destroy_result(&drop_res);
        rollback();
        return tl::unexpected {error_code::io_failure};
    }
    duckdb_destroy_result(&drop_res);
    if (duckdb_query(duck_con_, "DROP TABLE IF EXISTS game_result", &drop_res) == DuckDBError) {
        duckdb_destroy_result(&drop_res);
        rollback();
        return tl::unexpected {error_code::io_failure};
    }
    duckdb_destroy_result(&drop_res);
    if (auto schema_res = positions_->initialize_schema(); !schema_res) {
        rollback();
        return tl::unexpected {error_code::io_failure};
    }

    // Stream exactly the fields required for replay. game_store::get() also
    // loads event, player, provenance, and tag metadata, none of which is
    // needed to reconstruct positions.
    sqlite3_stmt* raw = nullptr;
    constexpr auto replay_games_sql = R"sql(
        SELECT g.id, g.result, w.elo, b.elo, g.moves
        FROM game AS g
        JOIN player AS w ON w.id = g.white_id
        JOIN player AS b ON b.id = g.black_id
        ORDER BY g.id
    )sql";
    if (sqlite3_prepare_v2(conn_, replay_games_sql, -1, &raw, nullptr) != SQLITE_OK) {
        rollback();
        return tl::unexpected {error_code::io_failure};
    }
    auto const stmt_guard = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> {raw, sqlite3_finalize};

    std::vector<position_row> pending_rows;
    pending_rows.reserve(rebuild_batch_rows);
    auto position_insert_elapsed = std::chrono::steady_clock::duration {};

    auto flush_pending_rows = [&]() -> result<void>
    {
        if (pending_rows.empty()) {
            return {};
        }
        auto const insert_started = std::chrono::steady_clock::now();
        if (auto ins_res = positions_->insert_batch(pending_rows); !ins_res) {
            return tl::unexpected {ins_res.error()};
        }
        position_insert_elapsed += std::chrono::steady_clock::now() - insert_started;
        pending_rows.clear();
        log_rss("after_flush_batch");
        return {};
    };

    auto const replay_started = std::chrono::steady_clock::now();
    auto position_row_build_elapsed = std::chrono::steady_clock::duration {};
    int step_result = sqlite3_step(stmt_guard.get());
    while (step_result == SQLITE_ROW) {
        auto const game_id = motif::db::game_id {static_cast<std::uint32_t>(sqlite3_column_int64(stmt_guard.get(), 0))};
        auto const* result =
            reinterpret_cast<char const*>(sqlite3_column_text(stmt_guard.get(), 1));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        auto const white_elo = sqlite3_column_type(stmt_guard.get(), 2) == SQLITE_NULL
            ? std::optional<std::int32_t> {}
            : std::optional<std::int32_t> {static_cast<std::int32_t>(sqlite3_column_int(stmt_guard.get(), 2))};
        auto const black_elo = sqlite3_column_type(stmt_guard.get(), 3) == SQLITE_NULL
            ? std::optional<std::int32_t> {}
            : std::optional<std::int32_t> {static_cast<std::int32_t>(sqlite3_column_int(stmt_guard.get(), 3))};
        auto const* moves_blob = sqlite3_column_blob(stmt_guard.get(), 4);
        auto const move_bytes = sqlite3_column_bytes(stmt_guard.get(), 4);
        if (result == nullptr || move_bytes < 0 || move_bytes % static_cast<int>(sizeof(std::uint16_t)) != 0) {
            rollback();
            return tl::unexpected {error_code::io_failure};
        }

        std::vector<std::uint16_t> moves(static_cast<std::size_t>(move_bytes) / sizeof(std::uint16_t));
        if (!moves.empty() && moves_blob == nullptr) {
            rollback();
            return tl::unexpected {error_code::io_failure};
        }
        if (!moves.empty()) {
            std::memcpy(moves.data(), moves_blob, static_cast<std::size_t>(move_bytes));
        }

        auto const build_started = std::chrono::steady_clock::now();
        auto batch = build_position_rows(moves, game_id, map_result(result), white_elo, black_elo);
        if (!batch) {
            rollback();
            return tl::unexpected {batch.error()};
        }

        if (!batch->empty()) {
            pending_rows.insert(pending_rows.end(), batch->begin(), batch->end());
        }
        position_row_build_elapsed += std::chrono::steady_clock::now() - build_started;
        if (pending_rows.size() >= rebuild_batch_rows) {
            auto flush_res = flush_pending_rows();
            if (!flush_res) {
                rollback();
                return tl::unexpected {flush_res.error()};
            }
        }
        step_result = sqlite3_step(stmt_guard.get());
    }
    if (step_result != SQLITE_DONE) {
        rollback();
        return tl::unexpected {error_code::io_failure};
    }

    if (auto flush_res = flush_pending_rows(); !flush_res) {
        rollback();
        return tl::unexpected {flush_res.error()};
    }
    if (timing != nullptr) {
        timing->position_replay_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - replay_started);
        timing->position_row_build_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(position_row_build_elapsed);
        timing->position_insert_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(position_insert_elapsed);
    }

    if (sort_by_zobrist) {
        auto const sort_started = std::chrono::steady_clock::now();
        if (auto sort_res = positions_->sort_by_zobrist(); !sort_res) {
            rollback();
            return tl::unexpected {sort_res.error()};
        }
        if (timing != nullptr) {
            timing->sort_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sort_started);
        }
        log_rss("after_sort_by_zobrist");
    }

    auto const rollup_started = std::chrono::steady_clock::now();
    if (auto rollup_res = positions_->rebuild_opening_stats_rollups(); !rollup_res) {
        rollback();
        return tl::unexpected {rollup_res.error()};
    }
    if (timing != nullptr) {
        timing->rollup_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rollup_started);
    }

    duckdb_result commit_res {};
    if (duckdb_query(duck_con_, "COMMIT", &commit_res) == DuckDBError) {
        duckdb_destroy_result(&commit_res);
        rollback();
        return tl::unexpected {error_code::io_failure};
    }
    duckdb_destroy_result(&commit_res);
    log_rss("after_commit");
    return {};
}

}  // namespace motif::db
