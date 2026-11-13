#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "motif/import/import_pipeline.hpp"

#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)
#include <spdlog/spdlog.h>
#include <taskflow/algorithm/pipeline.hpp>  // NOLINT(misc-include-cleaner)
#include <taskflow/taskflow.hpp>  // NOLINT(misc-include-cleaner)
#include <tl/expected.hpp>

#include "motif/chess/chess.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/types.hpp"
#include "motif/import/checkpoint.hpp"
#include "motif/import/error.hpp"
#include "motif/import/pgn_helpers.hpp"
#include "motif/import/pgn_reader.hpp"

namespace motif::import
{

auto default_worker_count() noexcept -> std::size_t
{
    constexpr std::size_t min_workers = 1;
    constexpr std::size_t half_divisor = 2;
    auto const detected = static_cast<std::size_t>(std::thread::hardware_concurrency());
    return std::max(min_workers, detected / half_divisor);
}

namespace
{

auto current_time_ns() noexcept -> std::int64_t
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr std::string_view event_marker = "[Event \"";

auto count_games(std::filesystem::path const& pgn_path, std::size_t start_offset) -> result<std::size_t>
{
    auto file = std::ifstream {pgn_path, std::ios::binary | std::ios::ate};
    if (!file.is_open()) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const file_size = file.tellg();
    if (file_size < std::streampos {0}) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const source_size = static_cast<std::size_t>(file_size);
    if (std::cmp_greater(start_offset, source_size)) {
        return tl::unexpected {error_code::io_failure};
    }
    if (std::cmp_equal(start_offset, source_size)) {
        return std::size_t {0};
    }
    file.seekg(static_cast<std::streamoff>(start_offset));
    if (!file) {
        return tl::unexpected {error_code::io_failure};
    }

    constexpr std::size_t buf_size = 1048576;
    constexpr std::size_t overlap = event_marker.size() - 1;
    auto buf = std::string(buf_size + overlap, '\0');
    std::size_t count = 0;
    std::size_t carry = 0;

    while (true) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        file.read(buf.data() + static_cast<std::ptrdiff_t>(carry), static_cast<std::streamsize>(buf_size));
        auto const read_len = static_cast<std::size_t>(file.gcount());
        if (read_len == 0 && carry == 0) {
            break;
        }

        auto const total = carry + read_len;
        auto const* data = buf.data();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        auto const* end = data + total;

        auto const* pos = data;
        while (pos < end) {
            pos = std::search(pos, end, event_marker.begin(), event_marker.end());
            if (pos == end) {
                break;
            }
            ++count;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            pos += event_marker.size();
        }

        if (read_len == 0) {
            break;
        }

        auto const keep = std::min(total, overlap);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        std::memmove(buf.data(), end - keep, keep);
        carry = keep;
    }

    return count;
}

struct prepared_game
{
    motif::db::game game_row;
};

enum class slot_state : std::uint8_t
{
    empty,
    ready,
    parse_error
};

struct pipeline_slot
{
    pgn::game pgn_game {};
    std::size_t game_start_offset {};
    std::size_t next_game_offset {};
    std::optional<prepared_game> prepared;
    std::optional<motif::import::error> error;
    slot_state state {slot_state::empty};
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto prepare_game(pgn::game const& pgn_game) -> result<prepared_game>
{
    if (pgn_game.moves.empty()) {
        return tl::unexpected {error_code::empty_game};
    }

    auto const& tags = pgn_game.tags;

    auto const white_elo_opt = parse_elo(find_tag(tags, "WhiteElo"));
    auto const black_elo_opt = parse_elo(find_tag(tags, "BlackElo"));
    auto const white_title_raw = find_tag(tags, "WhiteTitle");
    auto const black_title_raw = find_tag(tags, "BlackTitle");
    auto const event_name = find_tag(tags, "Event");
    auto const site_raw = find_tag(tags, "Site");
    auto const date_raw = find_tag(tags, "Date");
    auto const eco_raw = find_tag(tags, "ECO");

    auto const valid_date = (!date_raw.empty() && date_raw != "????.??.??") ? std::optional<std::string> {date_raw} : std::nullopt;

    motif::db::game game_row;
    game_row.white.name = find_tag(tags, "White");
    game_row.white.elo = white_elo_opt ? std::optional<std::int32_t> {*white_elo_opt} : std::nullopt;
    game_row.white.title = white_title_raw.empty() ? std::nullopt : std::optional<std::string> {white_title_raw};

    game_row.black.name = find_tag(tags, "Black");
    game_row.black.elo = black_elo_opt ? std::optional<std::int32_t> {*black_elo_opt} : std::nullopt;
    game_row.black.title = black_title_raw.empty() ? std::nullopt : std::optional<std::string> {black_title_raw};

    if (!event_name.empty()) {
        game_row.event_details = motif::db::event {
            .name = event_name,
            .site = site_raw.empty() ? std::nullopt : std::optional<std::string> {site_raw},
            .date = valid_date,
        };
    }

    game_row.date = valid_date;
    game_row.eco = eco_raw.empty() ? std::nullopt : std::optional<std::string> {eco_raw};
    game_row.result = pgn_result_to_string(pgn_game.result);

    if (pgn_game.moves.size() > std::numeric_limits<std::uint16_t>::max()) {
        return tl::unexpected {error_code::parse_error};
    }

    auto board = motif::chess::board {};
    std::vector<std::uint16_t> encoded_moves;
    encoded_moves.reserve(pgn_game.moves.size());

    for (auto const& node : pgn_game.moves) {
        auto move_result = motif::chess::apply_san(board, node.san);
        if (!move_result) {
            return tl::unexpected {error_code::parse_error};
        }
        encoded_moves.push_back(*move_result);
    }

    game_row.moves = std::move(encoded_moves);
    return prepared_game {.game_row = std::move(game_row)};
}

}  // namespace

import_pipeline::import_pipeline(motif::db::database_manager& dbm) noexcept
    : db_ {dbm}
{
}

auto import_pipeline::progress() const noexcept -> import_progress
{
    auto const start_time_ns = start_time_ns_.load(std::memory_order_relaxed);
    auto elapsed = std::chrono::milliseconds {0};
    if (start_time_ns != 0) {
        auto const now_ns = current_time_ns();
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds {now_ns - start_time_ns});
    }

    return import_progress {
        .games_processed = games_processed_.load(std::memory_order_relaxed),
        .games_committed = games_committed_.load(std::memory_order_relaxed),
        .games_skipped = games_skipped_.load(std::memory_order_relaxed),
        .errors = games_errored_.load(std::memory_order_relaxed),
        .total_games = total_games_.load(std::memory_order_relaxed),
        .elapsed = elapsed,
        .phase = phase_.load(std::memory_order_relaxed),
    };
}

auto import_pipeline::request_stop() noexcept -> void
{
    stop_requested_.store(true, std::memory_order_relaxed);
}

auto import_pipeline::run(std::filesystem::path const& pgn_path, import_config const& config) -> result<import_summary>
{
    if (config.num_workers == 0 || config.num_lines == 0) {
        return tl::unexpected {error_code::invalid_state};
    }

    // Same scope as resume(): the checkpoint read, the store().count_games() reconciliation in
    // resume_from_checkpoint(), and run_from() must all observe one generation.
    auto const generation_lock = db_.lock_generation();
    auto const chk = read_checkpoint(db_.dir());
    if (chk) {
        auto const checkpoint_source = std::filesystem::absolute(std::filesystem::path {chk->source_path}).lexically_normal();
        auto const requested_source = std::filesystem::absolute(pgn_path).lexically_normal();
        if (checkpoint_source == requested_source) {
            auto resumed = resume_from_checkpoint(pgn_path, *chk, config);
            if (resumed || resumed.error() != error_code::invalid_state) {
                return resumed;
            }
            delete_checkpoint(db_.dir());
        }
    } else if (chk.error() != error_code::not_found) {
        return tl::unexpected {chk.error()};
    }
    return run_from(pgn_path, 0, /*resumed=*/false, config);
}

auto import_pipeline::resume(std::filesystem::path const& pgn_path, import_config const& config) -> result<import_summary>
{
    if (config.num_workers == 0 || config.num_lines == 0) {
        return tl::unexpected {error_code::invalid_state};
    }
    auto const generation_lock = db_.lock_generation();
    auto chk = read_checkpoint(db_.dir());
    if (!chk) {
        return tl::unexpected {error_code::io_failure};
    }
    return resume_from_checkpoint(pgn_path, *chk, config);
}

auto import_pipeline::restore_identity_index() -> result<std::size_t>
{
    auto dedup_res = db_.writer().deduplicate();
    if (!dedup_res) {
        if (auto const log = spdlog::get("motif.import"); log) {
            log->error("identity index restore failed; the database needs recovery (reopen it or resume the import)");
        }
        return tl::unexpected {error {error_code::recovery_required, "identity index could not be restored"}};
    }
    return dedup_res->removed;
}

auto import_pipeline::resume_from_checkpoint(std::filesystem::path const& pgn_path,
                                             import_checkpoint const& checkpoint,
                                             import_config const& config) -> result<import_summary>
{
    auto const checkpoint_source = std::filesystem::absolute(std::filesystem::path {checkpoint.source_path}).lexically_normal();
    auto const requested_source = std::filesystem::absolute(pgn_path).lexically_normal();
    if (checkpoint_source != requested_source) {
        return tl::unexpected {error_code::invalid_state};
    }

    auto const index_exists = db_.writer().identity_index_exists();
    if (!index_exists) {
        return tl::unexpected {error_code::io_failure};
    }

    // The checkpoint only says that an import of this source was interrupted. Its
    // cursor is never used: the JSON file is written after its batch's SQLite
    // COMMIT, a NORMAL commit can be lost to a power failure while the file
    // survives, and nothing in the file can prove otherwise (see checkpoint.hpp).
    // The source is re-read from offset 0 with checked inserts, which skip the
    // games that survived as duplicates and import the ones that did not.
    if (!*index_exists) {
        // A process or power failure can occur after raw batches commit but
        // before deduplicate() restores the identity index. Reconcile that
        // durable SQLite state before resuming with checked inserts.
        // deduplicate() can remove canonical games committed by the prior,
        // interrupted session; each such change advances the mutation sequence,
        // so the previously published snapshot is stale by construction and
        // this reconciliation never needs to touch a derived index itself.
        if (auto restored = restore_identity_index(); !restored) {
            return tl::unexpected {restored.error()};
        }
    }
    // Games already stored are not this run's: count from the recovered database.
    auto const game_count = db_.store().count_games();
    if (!game_count) {
        return tl::unexpected {error_code::io_failure};
    }

    return run_from(pgn_path, *game_count, /*resumed=*/true, config);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto import_pipeline::run_from(std::filesystem::path const& pgn_path,
                               std::int64_t const pre_committed,
                               bool const resumed,
                               import_config const& config) -> result<import_summary>
{
    // No invalidation step: every canonical batch commit advances the mutation
    // sequence inside its own transaction, which atomically makes the previously
    // published snapshot stale. A failed or all-duplicate import changes nothing.
    auto const generation_lock = db_.lock_generation();
    if (config.num_workers == 0 || config.num_lines == 0) {
        auto log = spdlog::get("motif.import");
        if (log) {
            log->error("import_config validation failed: num_workers={} num_lines={}", config.num_workers, config.num_lines);
        }
        return tl::unexpected {error_code::invalid_state};
    }

    // An unreadable source is rejected before anything is changed -- no
    // checkpoint, no dropped identity index.
    if (!std::ifstream {pgn_path, std::ios::binary}.is_open()) {
        return tl::unexpected {error_code::io_failure};
    }

    auto log = spdlog::get("motif.import");

    // Deliberately not resetting stop_requested_ here: request_stop() can be
    // called (e.g. via DELETE /api/imports/:id) after a session's pipeline is
    // constructed but before its worker thread reaches this point, and each
    // import_pipeline is single-use (one run()/resume() call per HTTP
    // session) -- clearing a pending request here would silently drop that
    // cancellation and run the import to completion anyway.
    games_processed_.store(0, std::memory_order_relaxed);
    games_committed_.store(0, std::memory_order_relaxed);
    games_skipped_.store(0, std::memory_order_relaxed);
    games_errored_.store(0, std::memory_order_relaxed);
    total_games_.store(0, std::memory_order_relaxed);
    start_time_ns_.store(current_time_ns(), std::memory_order_relaxed);
    phase_.store(import_phase::ingesting, std::memory_order_relaxed);

    if (auto game_count = count_games(pgn_path, 0); game_count.has_value()) {
        total_games_.store(*game_count, std::memory_order_relaxed);
    }

    pgn_reader reader {pgn_path};

    std::int64_t last_game_id = 0;
    std::int64_t committed = pre_committed;
    std::size_t batch_pending = 0;
    bool sqlite_tx_open = false;
    bool eof_reached = false;
    bool stopped = false;
    std::size_t checkpoint_offset = 0;
    std::optional<error> fatal_error;
    // The cursor as of the last successful COMMIT (see write_progress_checkpoint).
    std::size_t durable_offset = 0;
    std::int64_t durable_committed = pre_committed;
    std::int64_t durable_last_game_id = 0;

    // A fresh import (never resumed) skips the per-row identity check during
    // ingest and reconciles duplicates in one pass afterward -- see
    // deduplicate_and_rebuild_index below. resume() keeps the checked path:
    // reasoning about a partially-committed DB is already nontrivial without
    // also tracking "is the identity index currently dropped". An existing
    // checkpoint means a checked resume even when it records zero accepted
    // games, so this is an explicit flag, not a game-count proxy.
    bool const raw_mode = !resumed;

    auto begin_sqlite_batch = [&]() -> bool
    {
        if (sqlite_tx_open) {
            return true;
        }
        auto begin_res = db_.writer().begin_transaction();
        if (!begin_res) {
            return false;
        }
        sqlite_tx_open = true;
        return true;
    };

    auto rollback_sqlite_batch = [&]() noexcept -> void
    {
        if (!sqlite_tx_open) {
            return;
        }
        db_.writer().rollback_transaction();
        sqlite_tx_open = false;
    };

    auto commit_sqlite_batch = [&]() -> bool
    {
        if (!sqlite_tx_open) {
            return true;
        }
        auto commit_res = db_.writer().commit_transaction();
        if (!commit_res) {
            return false;
        }
        sqlite_tx_open = false;
        // These games are now committed in SQLite, and the cursor describing
        // them is the only one a checkpoint may record. Postings and the
        // opening tree index (if enabled) are rebuilt from the persisted game
        // store once ingest finishes -- see below.
        durable_offset = checkpoint_offset;
        durable_committed = committed;
        durable_last_game_id = last_game_id;
        return true;
    };

    // Writes the cursor the last successful COMMIT made durable, never the
    // in-flight position, so the file never claims games a rollback discarded.
    // It marks the import as interrupted; resume does not seek to it.
    auto write_progress_checkpoint = [&]() -> bool
    {
        import_checkpoint const chk {
            .source_path = pgn_path.string(),
            .byte_offset = durable_offset,
            .games_committed = durable_committed,
            .last_game_id = durable_last_game_id,
        };
        if (auto wres = write_checkpoint(db_.dir(), chk); !wres) {
            if (log) {
                log->error("checkpoint write failed");
            }
            return false;
        }
        return true;
    };

    // Once drop_identity_index() below succeeds, every remaining early-return
    // path in this function must call this before returning: the DB is left
    // without its uniqueness guarantee -- and, once any raw insert has
    // landed, with potential unreconciled duplicates -- until deduplicate()
    // runs. It is the shared recovery path (fatal error, cancel, normal end).
    // Failure is reported to the caller as recovery_required rather than being
    // swallowed. Removed duplicates shrink the committed count, so the
    // checkpoint is rewritten to keep its reported count accurate.
    auto restore_after_raw_ingest = [&]() -> result<std::size_t>
    {
        if (!raw_mode) {
            return std::size_t {0};
        }
        auto restored = restore_identity_index();
        if (!restored) {
            return restored;
        }
        auto const removed = *restored;
        if (removed != 0U) {
            committed -= static_cast<std::int64_t>(removed);
            durable_committed -= static_cast<std::int64_t>(removed);
            games_committed_.fetch_sub(removed, std::memory_order_relaxed);
            games_skipped_.fetch_add(removed, std::memory_order_relaxed);
            write_progress_checkpoint();
        }
        return removed;
    };

    // Write the marker before the first canonical commit and retain it through
    // dedup and rebuild. If it cannot be written the database is untouched.
    // On POSIX it is also synchronized for power-loss recovery; see the
    // checkpoint contract for the Windows directory-sync limitation.
    if (!write_progress_checkpoint()) {
        return tl::unexpected {error_code::io_failure};
    }

    if (raw_mode) {
        if (auto drop_res = db_.writer().drop_identity_index(); !drop_res) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    if (!begin_sqlite_batch()) {
        if (auto restored = restore_after_raw_ingest(); !restored) {
            return tl::unexpected {restored.error()};
        }
        return tl::unexpected {error_code::io_failure};
    }

    std::vector<pipeline_slot> slots(config.num_lines);

    auto stage0 = [&](tf::Pipeflow& pflow) -> void
    {
        if (stop_requested_.load(std::memory_order_relaxed)) {
            stopped = true;
            eof_reached = true;
            pflow.stop();
            return;
        }
        if (eof_reached) {
            pflow.stop();
            return;
        }
        auto& slot = slots[pflow.line()];
        slot.state = slot_state::empty;
        slot.prepared.reset();
        slot.error.reset();
        slot.game_start_offset = reader.byte_offset();
        slot.next_game_offset = 0;
        auto game_res = reader.next();
        if (!game_res) {
            if (game_res.error() == error_code::parse_error) {
                slot.state = slot_state::parse_error;
                return;
            }
            fatal_error = game_res.error();
            eof_reached = true;
            pflow.stop();
            return;
        }
        slot.pgn_game = std::move(*game_res);
        slot.next_game_offset = reader.byte_offset();
        slot.state = slot_state::ready;
    };

    auto stage1 = [&](tf::Pipeflow& pflow) -> void
    {
        auto& slot = slots[pflow.line()];
        if (slot.state != slot_state::ready) {
            return;
        }
        auto prep = prepare_game(slot.pgn_game);
        if (!prep) {
            slot.state = slot_state::parse_error;
            slot.error = prep.error();
        } else {
            slot.prepared = std::move(*prep);
        }
    };

    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    auto stage2 = [&](tf::Pipeflow& pflow) -> void
    {
        auto& slot = slots[pflow.line()];
        games_processed_.fetch_add(1, std::memory_order_relaxed);

        if (slot.state == slot_state::parse_error) {
            games_skipped_.fetch_add(1, std::memory_order_relaxed);
            games_errored_.fetch_add(1, std::memory_order_relaxed);
            if (log) {
                auto const error_desc = slot.error ? to_string(*slot.error) : "pgn read error";
                auto const tags_available = !slot.pgn_game.tags.empty();
                if (tags_available) {
                    auto const white = find_tag(slot.pgn_game.tags, "White");
                    auto const black = find_tag(slot.pgn_game.tags, "Black");
                    auto const event = find_tag(slot.pgn_game.tags, "Event");
                    log->warn(
                        "Skipped game at offset {}: {}. White: \"{}\", " "Black: \"{}\", Event: \"{}\"",
                        slot.game_start_offset,
                        error_desc,
                        white.empty() ? "N/A" : white,
                        black.empty() ? "N/A" : black,
                        event.empty() ? "N/A" : event);
                } else {
                    log->warn("Skipped game at offset {}: {} (headers unavailable)", slot.game_start_offset, error_desc);
                }
            }
            slot.state = slot_state::empty;
            return;
        }
        if (!slot.prepared) {
            slot.state = slot_state::empty;
            return;
        }

        auto& prep = *slot.prepared;

        auto ins = raw_mode ? db_.writer().insert_raw(prep.game_row) : db_.writer().insert(prep.game_row);
        if (!ins) {
            if (ins.error() == motif::db::error_code::duplicate) {
                games_skipped_.fetch_add(1, std::memory_order_relaxed);
                if (log) {
                    log->warn("duplicate game skipped at offset {}", slot.game_start_offset);
                }
            } else {
                games_errored_.fetch_add(1, std::memory_order_relaxed);
                rollback_sqlite_batch();
                fatal_error = error {error_code::io_failure};
                eof_reached = true;
                pflow.stop();
                if (log) {
                    log->error("SQLite insert failed at offset {}", slot.game_start_offset);
                }
            }
            slot.prepared.reset();
            slot.state = slot_state::empty;
            return;
        }

        checkpoint_offset = slot.next_game_offset;
        auto const game_id = *ins;

        last_game_id = static_cast<std::int64_t>(game_id.value);
        committed++;
        batch_pending++;
        games_committed_.fetch_add(1, std::memory_order_relaxed);

        if (batch_pending >= config.batch_size) {
            if (!commit_sqlite_batch()) {
                rollback_sqlite_batch();
                fatal_error = error {error_code::io_failure};
                eof_reached = true;
                pflow.stop();
                if (log) {
                    log->error("SQLite batch commit failed");
                }
                slot.prepared.reset();
                slot.state = slot_state::empty;
                return;
            }

            write_progress_checkpoint();
            batch_pending = 0;
            if (stop_requested_.load(std::memory_order_relaxed)) {
                stopped = true;
                eof_reached = true;
                pflow.stop();
                slot.prepared.reset();
                slot.state = slot_state::empty;
                return;
            }
            if (!begin_sqlite_batch()) {
                fatal_error = error {error_code::io_failure};
                eof_reached = true;
                pflow.stop();
                if (log) {
                    log->error("SQLite batch begin failed");
                }
                slot.prepared.reset();
                slot.state = slot_state::empty;
                return;
            }
        }

        slot.prepared.reset();
        slot.state = slot_state::empty;
    };

    tf::Taskflow taskflow;  // NOLINT(misc-include-cleaner)
    tf::Pipeline pipeline {
        // NOLINT(misc-include-cleaner)
        config.num_lines,
        tf::Pipe {tf::PipeType::SERIAL, std::move(stage0)},
        tf::Pipe {tf::PipeType::PARALLEL, std::move(stage1)},
        tf::Pipe {tf::PipeType::SERIAL, std::move(stage2)},
    };

    taskflow.composed_of(pipeline);
    auto const ingest_started = std::chrono::steady_clock::now();
    tf::Executor executor {config.num_workers};  // NOLINT(misc-include-cleaner)
    executor.run(taskflow).wait();

    if (fatal_error.has_value() && *fatal_error != error_code::eof) {
        rollback_sqlite_batch();
        if (auto restored = restore_after_raw_ingest(); !restored) {
            return tl::unexpected {restored.error()};
        }
        return tl::unexpected {*fatal_error};
    }

    if (!commit_sqlite_batch()) {
        rollback_sqlite_batch();
        if (auto restored = restore_after_raw_ingest(); !restored) {
            return tl::unexpected {restored.error()};
        }
        return tl::unexpected {error_code::io_failure};
    }
    // The initial marker remains until success; this updates its reported
    // cursor after the final batch.
    write_progress_checkpoint();
    auto const ingest_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ingest_started);
    auto position_postings_metrics = motif::db::position_postings_build_metrics {};
    auto opening_tree_index_metrics = motif::db::opening_tree_index_build_metrics {};

    // Runs even on early stop/cancel: raw_mode dropped the identity index
    // before ingest, so the DB is left without it -- and with unreconciled
    // raw duplicates -- until this completes. Never skip it once raw_mode
    // ingest has started.
    std::size_t duplicates_removed = 0;
    std::chrono::milliseconds dedup_elapsed {0};
    if (raw_mode) {
        phase_.store(import_phase::deduplicating, std::memory_order_relaxed);
        auto const dedup_started = std::chrono::steady_clock::now();
        auto restored = restore_after_raw_ingest();
        if (!restored) {
            // Cancel or not, the index is still absent: this is not a completed
            // (or resumable-as-usual) import.
            return tl::unexpected {restored.error()};
        }
        dedup_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - dedup_started);
        duplicates_removed = *restored;
    }

    if (stopped || stop_requested_.load(std::memory_order_relaxed)) {
        write_progress_checkpoint();
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::nanoseconds {current_time_ns() - start_time_ns_.load(std::memory_order_relaxed)});

        return import_summary {
            .total_attempted = games_processed_.load(std::memory_order_relaxed),
            .committed = static_cast<std::size_t>(committed - pre_committed),
            .skipped = games_skipped_.load(std::memory_order_relaxed),
            .errors = games_errored_.load(std::memory_order_relaxed),
            .duplicates_removed = duplicates_removed,
            .ingest_elapsed = ingest_elapsed,
            .dedup_elapsed = dedup_elapsed,
            .position_postings_metrics = std::nullopt,
            .opening_tree_index_metrics = std::nullopt,
            .elapsed = elapsed,
        };
    }

    if (config.build_position_postings_after_import && !db_.dir().empty()) {
        phase_.store(import_phase::rebuilding, std::memory_order_relaxed);
        if (auto postings_res = db_.rebuild_position_postings(&position_postings_metrics); !postings_res) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    if (config.build_opening_tree_index_after_import) {
        phase_.store(import_phase::rebuilding, std::memory_order_relaxed);
        if (auto index_res = db_.rebuild_opening_tree_index(&opening_tree_index_metrics); !index_res) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    delete_checkpoint(db_.dir());
    db_.writer().clear_insert_caches();

    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds {current_time_ns() - start_time_ns_.load(std::memory_order_relaxed)});

    return import_summary {
        .total_attempted = games_processed_.load(std::memory_order_relaxed),
        .committed = static_cast<std::size_t>(committed - pre_committed),
        .skipped = games_skipped_.load(std::memory_order_relaxed),
        .errors = games_errored_.load(std::memory_order_relaxed),
        .duplicates_removed = duplicates_removed,
        .ingest_elapsed = ingest_elapsed,
        .dedup_elapsed = dedup_elapsed,
        .position_postings_metrics = config.build_position_postings_after_import
            ? std::optional<motif::db::position_postings_build_metrics> {position_postings_metrics}
            : std::optional<motif::db::position_postings_build_metrics> {},
        .opening_tree_index_metrics = config.build_opening_tree_index_after_import
            ? std::optional<motif::db::opening_tree_index_build_metrics> {opening_tree_index_metrics}
            : std::optional<motif::db::opening_tree_index_build_metrics> {},
        .elapsed = elapsed,
    };
}

}  // namespace motif::import
