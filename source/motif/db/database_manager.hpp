#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/game_writer.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/opening_tree_index.hpp"
#include "motif/db/position_postings.hpp"
#include "motif/db/types.hpp"

struct sqlite3;

namespace motif::db
{

// Owns the lifecycle of a named chess database bundle:
//   <dir>/games.db          — SQLite WAL (game metadata and moves)
//   <dir>/positions.postings.N — immutable exact position occurrences
//   <dir>/opening_tree.idx.N   — immutable shallow continuation aggregate
//   <dir>/manifest.json      — glaze-serialized bundle metadata
//
// Exact postings are the sole durable position index. SQLite remains the
// canonical store for game/header truth; derived indexes are rebuilt from it
// and never duplicate that truth.
//
// A bundle from before postings existed (games.db plus a legacy
// positions.duckdb and no postings) is a one-release migration case: open()
// rebuilds postings directly from canonical SQLite. It never reads or
// deletes positions.duckdb -- that file, if present, is simply ignored and
// left untouched on disk. If the rebuild fails, open() fails too (fail
// closed); the attempt is safe to retry on a later open() because it only
// ever writes to a staging file that has never been published (see
// rebuild_position_postings()).
//
class bundle_lock;

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
    static auto create(std::filesystem::path const& dir, std::string const& name) -> result<database_manager>;

    // Open an existing bundle. Fails with not_found if games.db or
    // manifest.json are absent; fails with schema_mismatch if PRAGMA
    // user_version does not match schema::k_version. Rebuilds postings from
    // canonical SQLite when they are missing, stale, or predate postings
    // (see the class-level comment on the one-release migration case).
    static auto open(std::filesystem::path const& dir) -> result<database_manager>;

    // Create an ephemeral database backed by a private temporary directory
    // on disk. Used for short-lived, in-process use (tests, ephemeral
    // previews). Uses the same on-disk create() path as a persistent bundle
    // -- including its games.db, manifest.json, and derived-index files --
    // so it gets the identical staged-build/validate/atomic-publish
    // discipline for free, rather than a parallel in-memory implementation.
    // The temporary directory is removed on close(). dir() returns that
    // private directory; callers must not treat it as a persistent,
    // user-facing bundle location.
    static auto create_scratch() -> result<database_manager>;

    // Read-only access to the SQLite game store. Canonical writes are exposed
    // as manager operations so they cannot bypass derived-index invalidation.
    [[nodiscard]] auto store() const noexcept -> game_store const&;

    // Insert a game, persistently invalidating derived indexes before the
    // canonical SQLite mutation.
    auto insert_game(game const& src_game) -> result<game_id>;

    // Access the SQLite bulk writer used by import and other batched writes.
    // Callers must hold lock_generation() and call prepare_canonical_mutation()
    // before the first mutation in the locked batch.
    [[nodiscard]] auto writer() noexcept -> game_writer&;

    // Exact occurrence lookup from the immutable postings sidecar. Fails
    // closed with error_code::io_failure when postings are stale or absent.
    auto query_position_matches(zobrist_hash hash, std::size_t limit = 0, std::size_t offset = 0) const
        -> result<std::vector<position_match>>;

    // One ascending game ID per game reaching hash. Fails closed when
    // postings are stale or absent.
    auto position_game_ids(zobrist_hash hash) const -> result<std::vector<game_id>>;

    // Directory-level exact occurrence metadata from valid postings.
    auto position_summary(zobrist_hash hash) const -> result<std::optional<position_postings_summary>>;

    // Access the bundle manifest (read-only; updated internally on close).
    [[nodiscard]] auto manifest() const noexcept -> db_manifest const&;

    // Directory containing the database bundle files.
    [[nodiscard]] auto dir() const noexcept -> std::filesystem::path const&;

    // Set provenance on a just-inserted game. This does not affect derived
    // indexes because provenance is absent from their persisted records.
    auto set_manual_game_provenance(game_id game_key, std::optional<std::string> const& source_label, std::string const& review_status)
        -> result<void>;

    // Patch game metadata in SQLite. Only user-added games may be patched;
    // returns error_code::not_editable otherwise. Marks derived indexes
    // stale when the patch affects fields they aggregate (elo, result).
    auto patch_game_metadata(game_id game_key, game_patch const& patch) -> result<void>;

    // Delete a game from the SQLite game store, marking derived indexes
    // stale first so a crash between the two never leaves them looking
    // valid for a game set they no longer describe.
    // Returns error_code::not_found if the game does not exist.
    auto remove_game(game_id game_key) -> result<void>;

    // Delete a manually-added game. Imported games return
    // error_code::not_editable without modifying the store.
    auto remove_user_game(game_id game_key) -> result<void>;

    // Elo distribution per continuation from a given position. When filter
    // contains metadata criteria, first narrows game IDs via SQLite, then
    // aggregates from postings and replay.
    auto query_elo_distribution(zobrist_hash hash, search_filter const& filter, int bucket_width) const
        -> result<std::vector<elo_distribution_row>>;

    // Unfiltered opening-stat aggregate. Uses the shallow continuation index
    // only when exact postings prove the index covers every root occurrence.
    auto query_unfiltered_opening_stats(zobrist_hash hash) const -> result<std::vector<opening_stat_agg_row>>;

    // Filtered continuation aggregate from exact postings and bounded replay.
    auto query_filtered_opening_stats(zobrist_hash hash, std::vector<game_id> const& game_ids) const
        -> result<std::vector<opening_stat_agg_row>>;

    // Bounded continuation rows per exact root occurrence, from postings and
    // canonical moves.
    auto query_tree_slice(zobrist_hash root_hash, std::uint16_t max_depth, std::vector<game_id> const& game_ids = {}) const
        -> result<std::vector<tree_position_row>>;

    // Filtered game list with cross-store position support. When
    // filter.position is set, queries postings for matching game IDs first,
    // then applies metadata filters in SQLite.
    auto find_games(search_filter const& filter) -> result<game_list_result>;

    // Rebuild the immutable exact-occurrence sidecar from canonical SQLite
    // games. Persistent bundles and scratch databases alike.
    auto rebuild_position_postings(position_postings_build_metrics* metrics = nullptr) -> result<void>;

    // Build and load the optional shallow unfiltered continuation index.
    auto rebuild_opening_tree_index(opening_tree_index_build_metrics* metrics = nullptr) -> result<void>;

    // Persistently invalidate immutable derived indexes before a canonical
    // SQLite mutation. If the later mutation fails, rebuilding is unnecessary
    // but safe; the reverse order could leave a same-count stale index valid
    // when manifest persistence fails.
    auto prepare_canonical_mutation() -> result<void>;
    [[nodiscard]] auto lock_generation() const -> std::unique_lock<std::recursive_mutex>;

    // Release all connections and clear internal state.
    // Safe to call multiple times.
    void close() noexcept;

  private:
    database_manager() = default;

    sqlite3* conn_ {nullptr};
    std::optional<game_store> store_;
    std::optional<game_writer> writer_;
    db_manifest manifest_;
    std::filesystem::path dir_;
    std::optional<position_postings> position_postings_;
    std::optional<opening_tree_index> opening_tree_index_;
    // True for instances created by create_scratch(): dir_ points at a
    // private temporary directory that close() removes entirely, rather
    // than a user-facing persistent bundle location.
    bool is_scratch_ {false};
    std::unique_ptr<bundle_lock> bundle_lock_;
    mutable std::recursive_mutex generation_mutex_;
    auto mark_derived_indexes_stale() -> result<void>;
    auto publish_derived_index(std::string const& filename, bool is_postings, std::uint64_t next_build_seq) -> manifest_write_result;
    [[nodiscard]] auto has_valid_derived_index(derived_index_manifest_entry const& entry) const -> bool;
    // Caller must hold generation_mutex_. True when an open, checksum-backed
    // postings generation matches the manifest and indexes every canonical
    // SQLite game -- the same validity rule the per-query dispatch applies.
    [[nodiscard]] auto postings_cover_canonical_games() const -> bool;
};

}  // namespace motif::db
