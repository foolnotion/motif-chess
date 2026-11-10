#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtl/phmap.hpp>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

// Forward declaration — callers own the connection; game_store is non-owning.
struct sqlite3;
struct sqlite3_stmt;

namespace motif::db
{

class database_manager;
class game_writer;

// Minimal data needed by replay-based derived indexes. Keeping this separate
// from game avoids loading players, events, provenance, and tags per row.
struct replay_game_record
{
    game_id id {};
    std::string result;
    std::optional<std::int32_t> white_elo;
    std::optional<std::int32_t> black_elo;
    std::vector<std::uint16_t> moves;
};

// The index-relevant metadata of one canonical game (no moves).
struct canonical_metadata_row
{
    game_id id {};
    std::uint64_t content_revision {};
    std::string result;
    std::optional<std::int32_t> white_elo;
    std::optional<std::int32_t> black_elo;
};

// The three fields of a game the derived indexes embed. A change to any of them
// is the only canonical metadata change that advances the mutation sequence.
struct indexed_game_metadata
{
    std::string result;
    std::optional<std::int32_t> white_elo;
    std::optional<std::int32_t> black_elo;

    auto operator==(indexed_game_metadata const&) const -> bool = default;
};

// A validated, resolved metadata patch: everything a manager needs to decide
// what the patch costs (before/after image, whether the indexes are affected)
// and what the store needs to apply it later. Producing one writes nothing.
struct prepared_metadata_patch
{
    game_id id {};
    game_patch patch;
    std::uint64_t content_revision {};
    indexed_game_metadata before;
    indexed_game_metadata after;
    // The result or a rating really differs from the stored value: the only
    // case that advances the mutation sequence and touches the indexes. A
    // patch that restates the stored values is a canonical no-op, but note
    // that two different result strings are never a no-op even when the
    // indexes encode them identically (draw versus unfinished).
    bool indexed_change {false};
    // Some field other than result/ratings is supplied (names, event, site,
    // date, eco, notes, provenance).
    bool other_fields {false};

    // Nothing to write: not an indexed change and no other field supplied.
    [[nodiscard]] auto is_noop() const noexcept -> bool { return !indexed_change && !other_fields; }
};

class game_store
{
  public:
    explicit game_store(sqlite3* conn) noexcept;
    ~game_store() noexcept;

    game_store(game_store const&) = delete;
    auto operator=(game_store const&) -> game_store& = delete;
    game_store(game_store&& other) noexcept;
    auto operator=(game_store&& other) noexcept -> game_store&;

    // Create the tables and supporting indexes (the shared definition in
    // schema::create_tables, including the bundle_meta ID allocator).
    // Must be called once per connection before any other method. Unlike
    // schema::initialize it does not set WAL mode or PRAGMA user_version.
    // Duplicate key: (white_id, black_id, coalesced event_id, coalesced date,
    // result, move_hash, identity_collision).
    auto create_schema() -> result<void>;

    // Insert a game.  Returns the new game row id on success.
    // Returns error_code::duplicate when the identity key already exists.
    // Callers decide whether to treat a duplicate as fatal.
    auto insert(game const& src_game) -> result<game_id>;

    // The connection's single game_writer (see game_writer.hpp for why there
    // is exactly one). insert() and the transaction calls below forward to it,
    // and database_manager::writer() returns this same object, so batches and
    // standalone inserts share one id allocator. At most one game_store may
    // exist per connection.
    [[nodiscard]] auto writer() noexcept -> game_writer&;

    // Transaction control for batched import paths. Safe to use around multiple
    // insert() calls; insert() will join an already-open transaction.
    auto begin_transaction() -> result<void>;
    auto commit_transaction() -> result<void>;
    auto rollback_transaction() noexcept -> void;

    // Retrieve a game by id.  Returns error_code::not_found if absent.
    auto get(game_id game_key) -> result<game>;
    auto get(game_id game_key) const -> result<game>;
    // Precondition: game_ids contains no duplicates (duplicate entries are
    // silently dropped by the unordered_map; deduplicate before calling).
    auto get_game_contexts(std::vector<game_id> const& game_ids) -> result<gtl::flat_hash_map<game_id, game_context>>;
    auto get_game_contexts(std::vector<game_id> const& game_ids) const -> result<gtl::flat_hash_map<game_id, game_context>>;
    auto find_games(search_filter const& filter) const -> result<game_list_result>;
    // Returns all IDs from game_ids that satisfy the metadata filter; no pagination cap.
    auto find_game_ids_with_filter(std::vector<game_id> const& game_ids, search_filter const& filter) const -> result<std::vector<game_id>>;
    auto count_games() const -> result<std::int64_t>;
    // The canonical mutation sequence (mutation_state.mutation_seq): how many
    // index-relevant canonical transactions this bundle has committed. Read
    // from the table on every call (no cache), so it is correct across
    // rollbacks. See game_writer.hpp for exactly which operations advance it.
    // A writer-owned batch defers its advance to commit, so while
    // has_pending_canonical_change() is true this value is one behind the rows
    // visible on this connection.
    auto mutation_seq() const -> result<std::uint64_t>;
    // The durable game-ID allocator (bundle_meta.next_game_id): strictly greater
    // than every game ID ever committed, and written in the same transaction as
    // the rows it covers. Read from the table on every call.
    auto next_game_id() const -> result<std::int64_t>;
    // In-process counter of every canonical change a reader might have cached,
    // metadata and provenance included (see game_writer::data_revision()). Not
    // the index-coverage key: mutation_seq() keeps its meaning.
    [[nodiscard]] auto data_revision() const noexcept -> std::uint64_t;
    [[nodiscard]] auto has_pending_canonical_change() const noexcept -> bool;
    // All game IDs, ascending, unfiltered. For full-corpus streaming
    // consumers (index builders, rebuilds) -- not paginated like find_games.
    auto all_game_ids() const -> result<std::vector<game_id>>;
    // Visit every game once in ascending ID order using one SQLite cursor.
    // The visitor may stop iteration by returning an error.
    auto for_each_replay_game(std::function<result<void>(replay_game_record const&)> const& visitor) const -> result<void>;
    // Visit (id, content_revision, result, white_elo, black_elo) of every game in
    // ascending ID order without loading moves: the narrow scan behind
    // metadata-chain repair.
    auto for_each_metadata_row(std::function<result<void>(canonical_metadata_row const&)> const& visitor) const -> result<void>;

    // Delete the game row and all associated game_tag rows.
    // Player and event rows are preserved.
    // Returns error_code::not_found if the id does not exist.
    auto remove(game_id game_key) -> result<void>;

    // Retrieve only the provenance for a game (cheaper than full get()).
    // Returns error_code::not_found if the id does not exist.
    auto get_provenance(game_id game_key) const -> result<game_provenance>;

    // Standalone metadata patch: prepare, then apply in a transaction of its own
    // (or an operation savepoint when it joins a caller-owned one). Applies the
    // editability policy of game_patch: result and ratings on any game, every
    // other field on manual games only.
    // Returns error_code::not_found if the id does not exist.
    // Returns error_code::not_editable if an imported game's patch supplies a
    //   field other than result/ratings (nothing is applied).
    // Returns error_code::invalid_argument for a malformed result or a rating
    //   outside the signed 16-bit domain.
    // Returns error_code::duplicate if the patch would create an identity conflict.
    // The mutation sequence advances once, and only when the result or a rating
    // really changes; a patch that restates stored values is a no-op. This is the
    // store-level contract for users without a derived-index manager; the
    // database_manager publishes the matching derived state atomically instead.
    auto patch_metadata(game_id game_key, game_patch const& patch) -> result<void>;

    // Mark a just-inserted game as manually-added (source_type = 'manual').
    // Used after import_worker::process() to flip the default 'imported' provenance.
    // Returns error_code::not_found if the id does not exist.
    auto set_manual_provenance(game_id game_key, std::optional<std::string> const& source_label, std::string const& review_status)
        -> result<void>;

    // Release player, event, and tag id caches.  Call after a bulk import to
    // reclaim memory; subsequent inserts will re-populate the caches on demand.
    void clear_insert_caches() noexcept;

  private:
    friend class database_manager;

    sqlite3* db_;
    mutable std::mutex position_game_ids_mutex_;
    gtl::flat_hash_map<std::string, std::int64_t> player_id_cache_;
    gtl::flat_hash_map<std::string, std::int64_t> event_id_cache_;
    gtl::flat_hash_map<std::string, std::int64_t> tag_id_cache_;

    sqlite3_stmt* select_player_stmt_ {nullptr};
    sqlite3_stmt* insert_player_stmt_ {nullptr};
    sqlite3_stmt* select_event_stmt_ {nullptr};
    sqlite3_stmt* insert_event_stmt_ {nullptr};
    sqlite3_stmt* select_tag_stmt_ {nullptr};
    sqlite3_stmt* insert_tag_stmt_ {nullptr};
    sqlite3_stmt* insert_game_tag_stmt_ {nullptr};

    std::unique_ptr<game_writer> writer_;

    auto find_or_insert_player(player const& plr) -> result<std::int64_t>;
    auto find_or_insert_event(event const& evt) -> result<std::int64_t>;
    auto insert_game_tags(game_id game_key, std::vector<std::pair<std::string, std::string>> const& extra_tags) -> result<void>;
    auto prepare_cached_stmt(sqlite3_stmt*& stmt, char const* sql) -> result<sqlite3_stmt*>;
    auto find_games_with_ids(std::vector<game_id> const& game_ids, search_filter const& filter) const -> result<game_list_result>;

    // ── Metadata patching ───────────────────────────────────────────────────
    //
    // One patch implementation serves two owners. Standalone (patch_metadata,
    // above): it prepares, then runs apply + the sequence advance in a
    // transaction of its own. Managed (database_manager): the manager prepares,
    // builds the derived artifacts, and runs the same apply inside the one
    // FULL-synchronous transaction that also advances the sequence once and
    // replaces the published snapshot. The store never commits on the managed
    // path, and the id caches / data revision belong to the outer outcome.
    //
    // Validates the request (four result strings, ratings inside the signed
    // 16-bit domain), reads the before-image, enforces the editability policy
    // (imported games: result and ratings only, else not_editable for the
    // whole patch), and rejects an exact-duplicate identity (duplicate) without
    // writing anything. Never writes.
    auto prepare_metadata_patch(game_id game_key, game_patch const& patch) -> result<prepared_metadata_patch>;
    // The canonical UPDATE(s): re-verifies the before-image, resolves player and
    // event ids, assigns the identity collision slot (a move-hash collision
    // between distinct move lists takes the next slot; an exact duplicate is
    // `duplicate`), and writes every changed column in ONE statement so no
    // intermediate identity can collide. Runs inside the caller's transaction;
    // it does not advance the sequence, commit, or clear caches on failure.
    auto apply_metadata_patch(prepared_metadata_patch const& plan) -> result<void>;
    // The standalone transaction around apply_metadata_patch.
    auto commit_metadata_patch(prepared_metadata_patch const& plan) -> result<void>;
    // The single mutation-sequence advance of an indexed patch, inside the
    // caller's transaction.
    auto advance_mutation_seq() -> result<void>;
    // Outcome hooks of an outer transaction owner: drop the id caches a rolled
    // back transaction may have filled; tell cache holders canonical data changed.
    void discard_id_caches() noexcept;
    void note_data_changed() noexcept;
    // Moves and indexed fields of one game (the replay input of a one-game tree
    // contribution).
    auto get_replay_record(game_id game_key) const -> result<replay_game_record>;

    // Read-only identity resolution of a patch: nullopt when a named player or
    // event does not exist yet (no other game can then share the identity).
    struct identity_row;
    auto read_identity_row(game_id game_key) const -> result<identity_row>;
    auto lookup_player_id(std::string const& name) const -> result<std::optional<std::int64_t>>;
    auto lookup_event_id(std::string const& name) const -> result<std::optional<std::int64_t>>;
    auto identity_duplicate_exists(game_id game_key, identity_row const& identity) const -> result<bool>;
    auto next_identity_collision(game_id game_key, identity_row const& identity) const -> result<std::int64_t>;
    // The identity a patched game would have. `current` is the full game when the
    // patch touches names/event/site (needed to build a new player or event row)
    // and may be null otherwise. With create=false nothing is written and
    // nullopt means "names a player or event that does not exist yet".
    auto resolve_identity(game_id game_key, game_patch const& patch, identity_row const& stored, game const* current, bool create)
        -> result<std::optional<identity_row>>;
    // One patched player name / the patched event applied to `target`: false when
    // the named row does not exist and create is unset (see resolve_identity).
    auto resolve_patched_player(std::optional<std::string> const& name, player const* base, bool create, std::int64_t& target)
        -> result<bool>;
    auto resolve_patched_event(game_patch const& patch, game const& current, bool create, std::optional<std::int64_t>& target)
        -> result<bool>;
    // The full game when the patch names players, event or site; nullopt otherwise.
    auto load_patch_context(game_id game_key, game_patch const& patch) -> result<std::optional<game>>;
    // Refuses (duplicate) a patch whose resolved identity equals another game's.
    auto reject_duplicate_identity(game_id game_key, game_patch const& patch, std::string const& before_result) -> result<void>;
    // The single UPDATE of apply_metadata_patch, built column by column.
    class column_assignments;
    auto collect_identity_assignments(prepared_metadata_patch const& plan, column_assignments& assignments) -> result<void>;
    static void collect_value_assignments(prepared_metadata_patch const& plan, column_assignments& assignments);
    auto run_column_update(game_id game_key, column_assignments const& assignments) -> result<void>;
    auto replace_notes(game_id game_key, std::string const& notes) -> result<void>;
    auto read_indexed_metadata(game_id game_key) const -> result<std::pair<indexed_game_metadata, std::uint64_t>>;
};

}  // namespace motif::db
