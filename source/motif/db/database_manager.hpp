#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/game_writer.hpp"
#include "motif/db/manifest.hpp"
#include "motif/db/opening_tree_composite.hpp"
#include "motif/db/opening_tree_index.hpp"
#include "motif/db/position_postings.hpp"
#include "motif/db/resource_governor.hpp"
#include "motif/db/types.hpp"

struct sqlite3;

namespace motif::db
{
namespace fault
{
// Defined in fault_injection.hpp, which no production header includes.
enum class artifact_kind : std::uint8_t;
}  // namespace fault

// Owns the lifecycle of a named chess database bundle:
//   <dir>/games.db                  -- SQLite WAL: canonical games, the mutation
//                                       sequence, and the published snapshot row
//   <dir>/positions.postings.<id>   -- immutable exact position occurrences
//   <dir>/opening_tree.idx.<id>     -- immutable shallow continuation aggregate
//                                       (the tree checkpoint; format v6, v5 read)
//   <dir>/positions.metadata.<id>   -- immutable per-game metadata after-image
//                                       runs / one sparse checkpoint (a chain
//                                       layered over the postings metadata)
//   <dir>/opening_tree.patch.<id>   -- immutable signed tree-delta runs, merged
//                                       over the checkpoint at lookup
//   <dir>/manifest.json             -- rewritable cache of the snapshot row
//
// Exact postings are the sole durable position index. SQLite remains the
// canonical store for game/header truth AND for which derived artifacts are
// published; derived indexes are rebuilt from it and never duplicate that
// truth. Position queries are available only while the published snapshot's
// covered mutation sequence equals SQLite's current mutation sequence (and no
// canonical batch is pending); this replaces the former game-count equality
// checks, which a delete-then-insert could fool.
//
// Publication order is build -> fsync -> rename -> fsync directory -> SQLite
// COMMIT (FULL synchronous) -> install in memory -> manifest.json mirror. The
// COMMIT is the only publication point. A result or rating correction
// (patch_game_metadata) is the one mutation that keeps the indexes valid: its
// canonical UPDATE, its single sequence advance and the new snapshot row (with a
// metadata after-image run and, when a tree exists, a signed tree-delta run)
// share that one COMMIT. Insert, removal and dedup still invalidate the indexes
// until the next rebuild (PR 7/8 make them incremental); metadata the indexes do
// not embed does not invalidate them.
//
// A bundle from before postings existed (games.db plus a legacy
// positions.duckdb and no postings) is a one-release migration case: open()
// rebuilds postings directly from canonical SQLite. It never reads or
// deletes positions.duckdb -- that file, if present, is simply ignored and
// left untouched on disk. If the rebuild fails, open() fails too (fail
// closed); the attempt is safe to retry on a later open() because nothing is
// referenced until a snapshot commit succeeds.
//
class bundle_lock;

// The families of immutable derived artifact a composite snapshot chain adds to a
// bundle (the postings segment and the base tree checkpoint rebuild through
// rebuild_position_postings / rebuild_opening_tree_index).
enum class derived_artifact_family : std::uint8_t
{
    postings_metadata,  // positions.metadata.<id>     (run or sparse checkpoint)
    tree_checkpoint,  // opening_tree.idx.<id>       (v6 fold output)
    tree_patch,  // opening_tree.patch.<id>     (signed tree-delta run)
};

// A reserved, exclusively created staging file for a not-yet-published artifact.
struct staged_derived_artifact
{
    std::string id;
    std::string filename;
    std::filesystem::path staging_path;
    std::filesystem::path final_path;
};

// The next snapshot's complete payload plus the staged artifacts it references
// for the first time. Everything else the payload references must already be
// published and unchanged.
struct snapshot_chain_publication
{
    snapshot_payload payload;
    std::vector<staged_derived_artifact> staged;
};

// Where the derived indexes stand relative to canonical SQLite.
struct derived_index_report
{
    // mutation_state.mutation_seq right now.
    std::uint64_t mutation_seq {};
    // Sequence the installed snapshot covers; empty when unbuilt.
    std::optional<std::uint64_t> covered_mutation_seq;
    // True while a writer-owned batch holds an uncommitted index-relevant change.
    bool pending_canonical_change {false};
    // Exact position queries are available.
    bool postings_usable {false};
    // The opening tree is current as well; unusable trees fall back to postings.
    bool tree_usable {false};
    // Generation of the installed snapshot (advances on every publication).
    std::uint64_t publication_generation {};
};

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
    // exists at that path. The schema, the unbuilt snapshot row and the
    // mutation sequence commit together; then the empty postings snapshot
    // publishes (generation 1 at sequence 0). Once that commit succeeds a
    // manifest.json failure cannot undo the bundle.
    static auto create(std::filesystem::path const& dir, std::string const& name) -> result<database_manager>;

    // Open an existing bundle. Fails with not_found if games.db is absent and
    // with schema_mismatch for a schema or snapshot payload newer than this
    // build supports. SQLite is authoritative: manifest.json is not needed,
    // and a missing, stale, corrupt, ahead or foreign copy is ignored and
    // rewritten. A corrupt published_snapshot row fails closed (io_failure).
    // Rebuilds postings from canonical SQLite when the published snapshot does
    // not cover the current mutation sequence or its artifacts are missing or
    // corrupt. A bundle from before v7 is migrated in place (its derived
    // artifacts are not adopted) and rebuilt once. A legacy manifest.json
    // whose schema_version is newer than supported is rejected before any
    // migration.
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
    // as manager operations or through writer(); every one of them advances
    // the mutation sequence itself, so none can leave a stale snapshot usable.
    [[nodiscard]] auto store() const noexcept -> game_store const&;

    // Insert a game. The mutation sequence advances in the same SQLite
    // transaction as the row.
    auto insert_game(game const& src_game) -> result<game_id>;

    // Access the SQLite bulk writer used by import and other batched writes.
    // This is the same object game_store and insert_game() write through (one
    // writer, hence one id allocator, per connection), so a batch opened here
    // is seen by every insert on this manager. Callers must hold
    // lock_generation() across the whole batch. A batch advances the mutation
    // sequence once at commit; until then derived_index_status() reports
    // pending_canonical_change and position queries fail.
    [[nodiscard]] auto writer() noexcept -> game_writer&;

    // Exact occurrence lookup from the immutable postings sidecar. Fails
    // closed with error_code::io_failure unless the published snapshot covers
    // the current canonical mutation sequence.
    auto query_position_matches(zobrist_hash hash, std::size_t limit = 0, std::size_t offset = 0) const
        -> result<std::vector<position_match>>;
    // First (lowest-ply) occurrence for only the requested game IDs. This
    // avoids materializing a position's complete occurrence set when joining
    // one metadata page to its matched plies.
    auto query_position_first_matches(zobrist_hash hash, std::span<game_id const> game_ids) const -> result<std::vector<position_match>>;

    // One ascending game ID per game reaching hash. Fails closed when
    // postings are stale or absent.
    auto position_game_ids(zobrist_hash hash) const -> result<std::vector<game_id>>;

    // Directory-level exact occurrence metadata from valid postings. A stale
    // or unbuilt index is an io_failure, never an empty result: a successful
    // nullopt means the hash is absent from a valid, current snapshot.
    auto position_summary(zobrist_hash hash) const -> result<std::optional<position_postings_summary>>;

    // The installed published snapshot: the typed form of SQLite's
    // published_snapshot row (manifest.json is merely a cache of it). It keeps
    // describing the LAST PUBLISHED artifacts after a canonical mutation; use
    // derived_index_status() to learn whether they still cover the bundle.
    [[nodiscard]] auto manifest() const noexcept -> db_manifest const&;

    // Directory containing the database bundle files.
    [[nodiscard]] auto dir() const noexcept -> std::filesystem::path const&;

    // Set provenance on a just-inserted game. This does not affect derived
    // indexes because provenance is absent from their persisted records.
    auto set_manual_game_provenance(game_id game_key, std::optional<std::string> const& source_label, std::string const& review_status)
        -> result<void>;

    // Patch game metadata. Result, white_elo and black_elo may be corrected on
    // any game, imported ones included (a rating is set or cleared through
    // rating_patch); every other field is editable only on a manually added
    // game, and an imported game's patch that supplies one is rejected as a
    // whole (error_code::not_editable) without applying anything. Corrections
    // overwrite the canonical value: no original is kept and there is no undo.
    //
    // A patch that changes the result or a rating is ONE operation. Under the
    // generation lock it prepares (validates, reads the before-image and, only
    // when a tree exists, the game's moves; no other game is replayed),
    // durably installs the small unreferenced artifacts it needs (a metadata
    // after-image run and, with a tree, a signed tree-delta run -- or the
    // checkpoint that folds a chain that would otherwise pass its hard cap),
    // then commits ONE FULL-synchronous SQLite transaction holding the
    // canonical UPDATE(s), the collision slot, the single mutation-sequence
    // advance and the new published_snapshot row. Success means that COMMIT
    // (reconciled when SQLite reports an ambiguous one) and the in-memory
    // install happened; the data revision advanced, the manifest mirror was
    // attempted and superseded artifacts were collected. Queries stay available
    // and equal a rebuild of the edited bundle; the postings occurrence file
    // and the tree checkpoint are untouched unless a hard cap forced a fold.
    //
    // Fails before any UPDATE with index_stale when the indexes do not
    // currently cover the bundle (stale baseline: run a rebuild first), and
    // with invalid_argument inside an open canonical transaction. A failure
    // of the edit itself (including one that leaves the indexes unusable, such
    // as an unknown COMMIT outcome) is io_failure, never index_stale.
    // error_code::duplicate means the new identity would equal
    // another game's. A patch that restates stored values is a success that
    // changes nothing; draw versus unfinished is a real change even though
    // the indexes encode both identically. A patch that changes no indexed
    // field keeps the NORMAL canonical-only path (no artifacts, no FULL).
    //
    // The cost of a fold (cap crossing) is a checkpoint-sized synchronous write
    // under the lock; it is not a rebuild or a replay, but it is not subsecond
    // on a very large tree.
    auto patch_game_metadata(game_id game_key, game_patch const& patch) -> result<void>;

    // What the last patch_game_metadata call did to the derived state.
    struct patch_report
    {
        // Nothing was written (the patch restated stored values).
        bool canonical_noop {false};
        // Only non-indexed canonical fields changed (NORMAL commit).
        bool canonical_only {false};
        // One FULL transaction published the edit with its derived state.
        bool published {false};
        // The tree chain / metadata chain was folded into a new checkpoint.
        bool tree_folded {false};
        bool metadata_folded {false};
        // Chain lengths after the call.
        std::size_t tree_runs {};
        std::size_t metadata_runs {};
        // Games whose moves were replayed for a nonzero tree contribution,
        // and their plies.
        std::uint64_t replayed_games {};
        std::uint64_t replayed_plies {};
    };

    [[nodiscard]] auto last_patch_report() const noexcept -> patch_report const& { return last_patch_report_; }

    // Admission thresholds for the tree-run and metadata-run chains. The defaults
    // are the design's initial tunables (soft 8 runs / 4 MiB, hard 16 runs /
    // 16 MiB, 64 MiB decoded); tests pass small values to cross the caps.
    //
    // The hard run count and hard decoded size are clamped to what open() accepts
    // (max_patch_chain_runs and max_chain_resident_bytes), so no limit can admit
    // a chain that a reopen would reject and demote to a rebuild.
    void set_chain_limits(patch_chain_limits limits) noexcept;

    [[nodiscard]] auto chain_limits() const noexcept -> patch_chain_limits const& { return chain_limits_; }

    // Delete a game from the SQLite game store; the mutation sequence advances
    // in the same transaction, so a same-count delete + insert can never leave
    // the old snapshot looking valid.
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
    // games and publish it: build into a uniquely named staging file, validate,
    // fsync, rename (never replacing), fsync the directory, then COMMIT the new
    // published_snapshot row at FULL synchronous. The sequence captured at the
    // start is the coverage published. The SQLite COMMIT is the publication
    // point; the manifest.json mirror and cleanup that follow cannot fail it.
    // Persistent bundles and scratch databases alike. Fails with
    // invalid_argument while a canonical batch is open or pending.
    auto rebuild_position_postings(position_postings_build_metrics* metrics = nullptr) -> result<void>;

    // Build and publish the optional shallow unfiltered continuation index at
    // the current coverage. Requires usable postings (invalid_argument
    // otherwise); changes only the tree descriptor.
    auto rebuild_opening_tree_index(opening_tree_index_build_metrics* metrics = nullptr) -> result<void>;

    // Where the derived indexes stand relative to canonical SQLite, computed
    // from the mutation_state singleton and the installed readers (never from
    // the presence of manifest.json or of a descriptor).
    [[nodiscard]] auto derived_index_status() const -> result<derived_index_report>;

    // In-process counter that advances on every canonical change a cached copy
    // could have captured: inserts, removals, dedup, and metadata/provenance
    // patches that deliberately do NOT advance the mutation sequence. A cache
    // of canonical data is valid only while this value is unchanged AND
    // derived_index_status() still reports usable postings; read both under
    // lock_generation(). 0 on a closed manager.
    [[nodiscard]] auto data_revision() const noexcept -> std::uint64_t;
    [[nodiscard]] auto lock_generation() const -> std::unique_lock<std::recursive_mutex>;

    // Release all connections and clear internal state.
    // Safe to call multiple times.
    void close() noexcept;

    // Roll back a bundle produced by create(): close it, then delete only the
    // paths create() recorded in its ownership ledger (games.db and SQLite
    // sidecars, manifest.json, .motif.lock, and the artifact names its
    // derived-index builds reserved). The directory is never scanned, so
    // unrelated files survive even with look-alike names such as
    // positions.postings.keepme. The directory itself is removed only when
    // create() made it and it is empty afterwards. For a manager that did not
    // come from create() this is equivalent to close().
    void discard_created_bundle() noexcept;

  private:
    database_manager() = default;

    sqlite3* conn_ {nullptr};
    std::optional<game_store> store_;
    // The installed snapshot: what SQLite's published_snapshot row held at the
    // last publication or open. The readers below are installed only for
    // artifacts of this snapshot that passed size/checksum/structure checks.
    db_manifest manifest_;
    std::filesystem::path dir_;
    std::optional<position_postings> position_postings_;
    std::optional<composite_opening_tree> composite_tree_;
    // True for instances created by create_scratch(): dir_ points at a
    // private temporary directory that close() removes entirely, rather
    // than a user-facing persistent bundle location.
    bool is_scratch_ {false};
    // True for managers returned by create(): the bundle files in dir_ were
    // written by this process and are safe to delete on rollback.
    bool created_bundle_ {false};
    // True when create() also created dir_ itself (it did not exist before).
    bool created_dir_ {false};
    // Ownership ledger of create(): exactly the paths this manager's create()
    // put on disk (database files, reserved artifact names, manifest cache).
    // Rolling a created bundle back deletes these and nothing else; the
    // directory is never scanned, because it may predate create() and hold
    // unrelated files with look-alike names. Empty for opened bundles.
    std::vector<std::filesystem::path> created_paths_;
    // manifest.json is a cache of manifest_. True when its last rewrite
    // failed; open() and close() retry. It never affects query results.
    bool manifest_cache_repair_pending_ {false};
    std::unique_ptr<bundle_lock> bundle_lock_;
    // Share of the process-wide SQLite page-cache budget; released by close().
    sqlite_cache_lease cache_lease_;
    mutable std::recursive_mutex generation_mutex_;
    // Chain admission thresholds (see set_chain_limits) and the account of the
    // last patch_game_metadata call. Both are guarded by generation_mutex_.
    patch_chain_limits chain_limits_ {};
    patch_report last_patch_report_ {};

    // Caller must hold generation_mutex_. The one shared coverage computation
    // behind every position-dependent API; see derived_index_status().
    [[nodiscard]] auto coverage_locked() const -> result<derived_index_report>;
    // coverage_locked() plus the shared "postings must be usable" gate: fails
    // with io_failure when stale or unbuilt.
    [[nodiscard]] auto require_postings_usable() const -> result<derived_index_report>;
    // Which kind of publication a commit belongs to (selects the fault-injection
    // artifact kind).
    enum class publication_kind : std::uint8_t
    {
        postings,
        tree,
        chain,
        edit,
    };
    struct canonical_edit_steps;
    // Build -> fsync -> rename -> fsync directory happen in the callers; this
    // is the FULL-synchronous SQLite commit of the new snapshot row
    // (build-time sequence must still be current). When `edit` is given the
    // canonical edit runs first inside the same transaction: re-check the
    // sequence, apply, advance once, then replace the snapshot row covering
    // base + 1. Success means COMMIT succeeded: the publication is durable and
    // final. outcome_unknown is set when COMMIT failed in a way that leaves it
    // unknown whether the row was written; the caller must then keep its
    // artifacts.
    auto commit_snapshot(db_manifest const& next,
                         std::uint64_t expected_mutation_seq,
                         publication_kind publication,
                         bool& outcome_unknown,
                         canonical_edit_steps const* edit = nullptr) -> result<void>;
    // Post-commit steps: install the already-prepared snapshot and readers
    // (moves and scalar assignments only; nothing here can fail the
    // publication). A reader argument is installed when provided; the tree
    // reader is dropped when the new snapshot carries no tree. An edit's
    // verified metadata slot is assigned into the postings reader.
    void install_snapshot(db_manifest next,
                          std::optional<position_postings> postings,
                          std::optional<composite_opening_tree> tree,
                          position_metadata_slot const* metadata_slot = nullptr) noexcept;
    // Rewrites manifest.json from manifest_. Failure is a cache-repair issue.
    void mirror_manifest() noexcept;
    // Deletes owned-pattern artifact files the installed snapshot no longer
    // references. Best-effort; a failure only leaks disk space.
    void collect_garbage() noexcept;
    // Adds a path create() put on disk to the ownership ledger. No-op for a
    // manager that did not come from create().
    void note_created_path(std::filesystem::path path);
    // Publishes a snapshot that drops an unusable tree descriptor.
    void clear_unusable_tree() noexcept;

    // Result of loading the installed snapshot's artifacts into readers.
    struct reader_load_outcome
    {
        // The segment verified but its metadata chain did not: the baseline
        // reader (embedded metadata only) is kept for the narrow repair, and
        // postings are NOT installed.
        bool metadata_chain_invalid {false};
        std::optional<position_postings> baseline;
    };

    // Loads and verifies the installed snapshot's artifacts into readers.
    auto load_readers(std::uint64_t current_mutation_seq) -> reader_load_outcome;

    // The edit that rides inside a snapshot publication (patch_game_metadata):
    // the canonical UPDATE(s) and the one sequence advance run inside the same
    // FULL-synchronous transaction that replaces the published_snapshot row.
    struct canonical_edit_steps
    {
        // The sequence the edit starts from; the snapshot row it replaces
        // covers base + 1.
        std::uint64_t base_mutation_seq {};
        // Canonical UPDATE(s) including the collision slot, in the transaction.
        std::function<result<void>()> apply;
        // The single mutation-sequence advance, in the transaction.
        std::function<result<void>()> advance;
    };

    // What an edit installs after COMMIT, all noexcept: the verified metadata
    // slot (assigned in the postings reader's resident table) and the new tree
    // reader.
    struct prepared_edit
    {
        canonical_edit_steps steps;
        position_metadata_slot slot;
    };

    // Readers a publication installs: either given by the caller (already
    // verified) or loaded from the final artifact names after they are renamed.
    struct prepared_readers
    {
        std::optional<position_postings> postings;
        std::optional<composite_opening_tree> tree;
        bool load_from_disk {false};
        // Runs after the staged files have their final names and the directory
        // is fsynced, for a reader that must be opened from a final path (a
        // freshly folded checkpoint). Its result replaces `tree`.
        std::function<result<std::optional<composite_opening_tree>>()> tree_loader;
    };

    // What a publication reads before it writes anything: the store, the
    // canonical sequence and game count it will stamp, and its generation.
    struct publication_gate;
    // The gates every publication that stamps the CURRENT canonical state shares
    // (a rebuild or a chain publication): a bundle is open, no canonical
    // transaction is open or pending, the sequence and game count are readable,
    // the generation counter has headroom and, when `edit` is given, the
    // sequence it was prepared against is still current. Writes nothing.
    // Caller must hold generation_mutex_.
    [[nodiscard]] auto begin_publication(canonical_edit_steps const* edit = nullptr) -> result<publication_gate>;
    // The edit half of the snapshot transaction, run after BEGIN IMMEDIATE:
    // re-check the sequence, apply the canonical UPDATE(s), advance once. The
    // caller rolls back on failure.
    [[nodiscard]] auto apply_canonical_edit(canonical_edit_steps const& edit,
                                            std::uint64_t expected_mutation_seq,
                                            fault::artifact_kind kind,
                                            std::uint64_t generation) const -> result<void>;
    // Completes `readers` for a publication: loads and verifies them from the
    // final artifact names when asked, then runs the tree loader.
    [[nodiscard]] auto materialize_readers(prepared_readers& readers, db_manifest const& next, std::uint64_t mutation_seq) const
        -> result<void>;
    // The tail of open(): loads the installed snapshot's readers, then repairs,
    // rebuilds or demotes whatever is not current and refreshes manifest.json.
    auto recover_derived_state(std::uint64_t current_mutation_seq, std::filesystem::path const& manifest_path) -> result<void>;
    // The installed postings reader once coverage proved it usable; io_failure
    // when stale or unbuilt. Caller must hold generation_mutex_.
    [[nodiscard]] auto usable_postings() const -> result<position_postings const*>;

    // Working state of one indexed patch, threaded through the phases below
    // (defined with publish_indexed_patch).
    struct indexed_patch_context;
    // Gates, the metadata after-image and its resident slot.
    [[nodiscard]] auto prepare_indexed_patch(prepared_metadata_patch const& plan, indexed_patch_context& ctx) const -> result<void>;
    // The one-game tree contribution, only when a usable tree exists.
    [[nodiscard]] auto build_patch_tree_run(prepared_metadata_patch const& plan, indexed_patch_context& ctx) const -> result<void>;
    // Encodes the candidate runs, classifies each chain (append or fold) and
    // checks free disk space for every candidate.
    [[nodiscard]] auto admit_patch_runs(indexed_patch_context& ctx) const -> result<void>;
    // The candidate tree reader of an append: shares the checkpoint and every run.
    [[nodiscard]] auto extend_patch_tree_reader(indexed_patch_context& ctx) const -> result<void>;
    // Reserves a staging file, writes the artifact into it and seals it.
    [[nodiscard]] auto stage_patch_artifact(indexed_patch_context& ctx,
                                            derived_artifact_family family,
                                            std::function<result<void>(std::filesystem::path const&)> const& write)
        -> result<artifact_descriptor>;
    [[nodiscard]] auto stage_patch_metadata(indexed_patch_context& ctx) -> result<void>;
    [[nodiscard]] auto stage_patch_tree(indexed_patch_context& ctx) -> result<void>;
    [[nodiscard]] auto stage_folded_tree(indexed_patch_context& ctx, tree_descriptor& tree_payload) -> result<void>;

    // The shared body of publish_snapshot_chain, the metadata repair and the
    // edit: rename every staged file (no replace), fsync the directory, prepare
    // readers, FULL-commit (with the edit's canonical UPDATE and sequence
    // advance inside the same transaction when `edit` is given), install, mirror,
    // collect garbage.
    auto publish_prepared(snapshot_payload payload,
                          std::vector<staged_derived_artifact> staged,
                          prepared_readers readers,
                          prepared_edit* edit = nullptr) -> result<void>;
    // The index-relevant half of patch_game_metadata: prepare the derived state,
    // stage its artifacts, and publish them with the edit.
    auto publish_indexed_patch(prepared_metadata_patch const& plan) -> result<void>;
    // Replaces a corrupt metadata chain by one sparse checkpoint derived from
    // canonical SQLite (no chess replay), after proving the baseline membership
    // still matches; fails (leaving the caller to rebuild) when it does not.
    auto repair_metadata_chain(position_postings baseline) -> result<void>;

    // ── Composite snapshot publication (private) ──────────────────────────────
    //
    // The protocol every chain producer shares, in the same order as a rebuild:
    // reserve a staging file, write the artifact into it, seal it (fsync +
    // descriptor), then publish_snapshot_chain() which renames every staged file
    // without replacing anything, fsyncs the directory, verifies and loads the
    // exact reader state the new payload describes, commits the new
    // published_snapshot row at FULL synchronous (the only publication point),
    // installs the readers, mirrors manifest.json and collects garbage.
    //
    // These are private because publish_snapshot_chain() trusts the caller's
    // payload: it verifies each artifact against its descriptor and the chain
    // structure, but not that the runs are the true consequence of canonical
    // SQLite, and it stamps the snapshot as covering the CURRENT mutation
    // sequence. A public caller could therefore claim coverage it never earned.
    // Only the manager's own producers (metadata repair, edit, rebuild) and
    // snapshot_publication_test_access (tests that publish hand-built chains
    // after changing canonical SQLite through a second connection) may call them.
    //
    // Fails with invalid_argument while a canonical transaction is open or
    // pending. Any failure before COMMIT leaves the previous snapshot and every
    // referenced file untouched and removes this attempt's own staged files.
    friend struct snapshot_publication_test_access;
    [[nodiscard]] auto reserve_derived_artifact(derived_artifact_family family) -> result<staged_derived_artifact>;
    // fsyncs the written staging file and returns its descriptor (family format,
    // version, size and FNV-1a-64 of the exact bytes); it does not publish.
    [[nodiscard]] static auto seal_derived_artifact(staged_derived_artifact const& staged, derived_artifact_family family)
        -> result<artifact_descriptor>;
    // Removes a staged (or already renamed, never referenced) artifact.
    static void discard_derived_artifact(staged_derived_artifact const& staged) noexcept;
    [[nodiscard]] auto publish_snapshot_chain(snapshot_chain_publication publication) -> result<void>;
};

}  // namespace motif::db
