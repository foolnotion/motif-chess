#pragma once

#include <cstdint>

// Fault-injection seam for crash and I/O-failure tests of the derived-index
// publication protocol and of canonical transaction commits. This header is
// deliberately not included by any public production header: only the
// implementation files that own a boundary, and tests, include it.
//
// The seam is one null-by-default atomic function pointer. Every boundary is a
// single relaxed atomic load when no hook is installed, and the boundaries
// occur once per publication or transaction, never per posting or per game, so
// production cost is unmeasurable. No std::function, no filesystem mock.
//
// Contract:
//   * A hook returns fault_action::proceed or fault_action::fail_io.
//   * fail_io at a "before_*" point simulates the guarded operation failing;
//     the publication protocol must then behave exactly as for a real failure.
//   * An "after_*" point that follows a SUCCESSFUL SQLite COMMIT can never turn
//     that publication into "not committed". fail_io there is recorded as a
//     post-commit diagnostic and the publication still succeeds. To model
//     "the process died right after COMMIT" a test hook calls _exit() from a
//     forked child and the parent reopens the bundle.
//   * Hooks must not reenter database_manager APIs and must be noexcept. The
//     hook is process-global, so tests that install one must run serially.
struct sqlite3;

namespace motif::db::fault
{

enum class point : std::uint8_t
{
    before_build,
    after_build,
    before_artifact_validation,
    after_artifact_validation,
    before_artifact_fsync,
    after_artifact_fsync,
    before_artifact_rename,
    after_artifact_rename,
    before_artifact_directory_fsync,
    after_artifact_directory_fsync,
    before_synchronous_full,
    before_snapshot_begin,
    after_snapshot_row_write,
    before_sqlite_commit,
    after_sqlite_commit,
    before_synchronous_restore,
    before_snapshot_install,
    after_snapshot_install,
    before_manifest_write,
    after_manifest_temp_write,
    after_manifest_temp_fsync,
    before_manifest_rename,
    after_manifest_rename,
    before_manifest_directory_fsync,
    // The cache directory fsync completed: the mirror is fully durable.
    after_manifest_write,
    before_cleanup,
    after_cleanup_unlink,
    // Canonical (game-row) transaction commits driven by game_writer.
    before_canonical_commit,
    after_canonical_commit,
    // After an ambiguous COMMIT, just before the stored snapshot row is read
    // back to decide what happened. fail_io makes that read fail, leaving the
    // outcome unknown.
    before_commit_reconcile,
    // The one-operation Elo/result edit (database_manager::patch_game_metadata).
    // fail_io at before_disk_admission models "not enough free space for the
    // candidate artifacts": the edit is refused before any file is written.
    before_disk_admission,
    // Inside the FULL transaction, after the canonical UPDATE(s) / after the one
    // mutation-sequence bump, before the snapshot row is replaced. fail_io rolls
    // the whole transaction back.
    after_canonical_update,
    after_sequence_bump,
};

enum class artifact_kind : std::uint8_t
{
    none,
    postings,
    tree,
    // Composite-snapshot publication of metadata runs/checkpoints and tree patch
    // runs/checkpoints (see database_manager::publish_snapshot_chain).
    chain,
    // The same artifacts published by an edit together with its canonical UPDATE
    // and sequence advance, in one transaction.
    edit,
};

enum class fault_action : std::uint8_t
{
    proceed,
    fail_io,
    // Honored only at point::before_sqlite_commit. Models a COMMIT that SQLite
    // reports as an error although its transaction is already gone, i.e. the
    // outcome is unknown. The commit is really applied first, then the error
    // is reported ("the row landed after all").
    commit_error_after_commit,
    // Same, but the transaction is really discarded first ("the row is lost").
    commit_error_after_rollback,
};

// publication_generation is the generation being published (0 where no
// publication is in flight).
using hook_function = fault_action (*)(point, artifact_kind, std::uint64_t publication_generation) noexcept;

// Installs hook (nullptr clears it) and returns the previous one.
auto set_hook(hook_function hook) noexcept -> hook_function;

// Called by the boundaries themselves; returns proceed when no hook is set.
auto inject(point where,
            artifact_kind kind = artifact_kind::none,
            std::uint64_t publication_generation = 0,
            sqlite3* connection = nullptr) noexcept -> fault_action;

// Restores the previous hook on destruction.
class scoped_hook
{
  public:
    explicit scoped_hook(hook_function hook) noexcept
        : previous_ {set_hook(hook)}
    {
    }

    ~scoped_hook() { set_hook(previous_); }

    scoped_hook(scoped_hook const&) = delete;
    auto operator=(scoped_hook const&) -> scoped_hook& = delete;
    scoped_hook(scoped_hook&&) = delete;
    auto operator=(scoped_hook&&) -> scoped_hook& = delete;

  private:
    hook_function previous_;
};

// The SQLite connection the boundary that called the current hook is acting
// on, or nullptr when the boundary supplied none or no hook is running. Valid
// only for the duration of the hook call, on the calling thread; a hook may run
// read-only statements on it (PRAGMA synchronous, PRAGMA journal_mode, ...) to
// observe connection state at the boundary. It must not begin, commit or roll
// back a transaction or call database_manager.
auto hook_connection() noexcept -> sqlite3*;

}  // namespace motif::db::fault
