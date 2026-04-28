#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <ios>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "motif/import/import_pipeline.hpp"

#include <chesslib/board/board.hpp>  // NOLINT(misc-include-cleaner)
#include <chesslib/board/move_codec.hpp>
#include <chesslib/util/san.hpp>
#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)
#include <spdlog/spdlog.h>
#include <taskflow/algorithm/pipeline.hpp>  // NOLINT(misc-include-cleaner)
#include <taskflow/taskflow.hpp>  // NOLINT(misc-include-cleaner)
#include <tl/expected.hpp>

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

namespace
{

auto current_time_ns() noexcept -> std::int64_t
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr std::string_view event_marker = "[Event \"";

auto count_games(std::filesystem::path const& pgn_path) -> result<std::size_t>
{
    auto file = std::ifstream {pgn_path, std::ios::binary | std::ios::ate};
    if (!file.is_open()) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const file_size = file.tellg();
    if (file_size == std::streampos {0}) {
        return std::size_t {0};
    }
    file.seekg(0);

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
    std::optional<motif::import::error_code> error;
    slot_state state {slot_state::empty};
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto prepare_game(pgn::game const& pgn_game) -> result<prepared_game>
{
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

    auto board = chesslib::board {};
    std::vector<std::uint16_t> encoded_moves;
    encoded_moves.reserve(pgn_game.moves.size());

    for (auto const& node : pgn_game.moves) {
        auto move_result = chesslib::san::from_string(board, node.san);
        if (!move_result) {
            return tl::unexpected {error_code::parse_error};
        }
        encoded_moves.push_back(chesslib::codec::encode(*move_result));
        chesslib::move_maker mmaker {board, *move_result};
        mmaker.make();
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
    return run_from(pgn_path, 0, 0, 0, config);
}

auto import_pipeline::resume(std::filesystem::path const& pgn_path, import_config const& config) -> result<import_summary>
{
    auto chk = read_checkpoint(db_.dir());
    if (!chk) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const checkpoint_source = std::filesystem::absolute(std::filesystem::path {chk->source_path}).lexically_normal();
    auto const requested_source = std::filesystem::absolute(pgn_path).lexically_normal();
    if (checkpoint_source != requested_source) {
        return tl::unexpected {error_code::invalid_state};
    }

    return run_from(pgn_path, chk->byte_offset, chk->games_committed, chk->last_game_id, config);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto import_pipeline::run_from(std::filesystem::path const& pgn_path,
                               // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                               std::size_t start_offset,
                               std::int64_t pre_committed,
                               std::int64_t pre_last_game_id,
                               import_config const& config) -> result<import_summary>
{
    if (config.num_workers == 0 || config.num_lines == 0) {
        auto log = spdlog::get("motif.import");
        if (log) {
            log->error("import_config validation failed: num_workers={} num_lines={}", config.num_workers, config.num_lines);
        }
        return tl::unexpected {error_code::invalid_state};
    }

    auto log = spdlog::get("motif.import");

    stop_requested_.store(false, std::memory_order_relaxed);
    games_processed_.store(0, std::memory_order_relaxed);
    games_committed_.store(0, std::memory_order_relaxed);
    games_skipped_.store(0, std::memory_order_relaxed);
    games_errored_.store(0, std::memory_order_relaxed);
    total_games_.store(0, std::memory_order_relaxed);
    start_time_ns_.store(current_time_ns(), std::memory_order_relaxed);
    phase_.store(import_phase::ingesting, std::memory_order_relaxed);

    if (auto game_count = count_games(pgn_path); game_count.has_value()) {
        total_games_.store(*game_count, std::memory_order_relaxed);
    }

    pgn_reader reader {pgn_path};
    if (start_offset > 0) {
        if (auto seek_res = reader.seek_to_offset(start_offset); !seek_res) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    std::int64_t last_game_id = pre_last_game_id;
    std::int64_t committed = pre_committed;
    std::size_t batch_pending = 0;
    bool sqlite_tx_open = false;
    bool eof_reached = false;
    bool stopped = false;
    std::size_t checkpoint_offset = start_offset;
    std::optional<error_code> fatal_error;

    auto begin_sqlite_batch = [&]() -> bool
    {
        if (sqlite_tx_open) {
            return true;
        }
        auto begin_res = db_.store().begin_transaction();
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
        db_.store().rollback_transaction();
        sqlite_tx_open = false;
    };

    auto commit_sqlite_batch = [&]() -> bool
    {
        if (!sqlite_tx_open) {
            return true;
        }
        auto commit_res = db_.store().commit_transaction();
        if (!commit_res) {
            return false;
        }
        sqlite_tx_open = false;
        return true;
    };

    auto write_progress_checkpoint = [&]() -> void
    {
        import_checkpoint const chk {
            .source_path = pgn_path.string(),
            .byte_offset = checkpoint_offset,
            .games_committed = committed,
            .last_game_id = last_game_id,
        };
        if (auto wres = write_checkpoint(db_.dir(), chk); !wres) {
            if (log) {
                log->error("checkpoint write failed");
            }
        }
    };

    if (!begin_sqlite_batch()) {
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

        auto ins = db_.store().insert(prep.game_row);
        if (!ins) {
            if (ins.error() == motif::db::error_code::duplicate) {
                games_skipped_.fetch_add(1, std::memory_order_relaxed);
                if (log) {
                    log->warn("duplicate game skipped at offset {}", slot.game_start_offset);
                }
            } else {
                games_errored_.fetch_add(1, std::memory_order_relaxed);
                rollback_sqlite_batch();
                fatal_error = error_code::io_failure;
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

        last_game_id = static_cast<std::int64_t>(game_id);
        committed++;
        batch_pending++;
        games_committed_.fetch_add(1, std::memory_order_relaxed);

        if (batch_pending >= config.batch_size) {
            if (!commit_sqlite_batch()) {
                rollback_sqlite_batch();
                fatal_error = error_code::io_failure;
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
                fatal_error = error_code::io_failure;
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
    tf::Executor executor {config.num_workers};  // NOLINT(misc-include-cleaner)
    executor.run(taskflow).wait();

    if (fatal_error.has_value() && *fatal_error != error_code::eof) {
        rollback_sqlite_batch();
        return tl::unexpected {*fatal_error};
    }

    if (!commit_sqlite_batch()) {
        rollback_sqlite_batch();
        return tl::unexpected {error_code::io_failure};
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
            .elapsed = elapsed,
        };
    }

    if (config.rebuild_positions_after_import) {
        phase_.store(import_phase::rebuilding, std::memory_order_relaxed);
        if (auto rebuild_res = db_.rebuild_position_store(config.sort_positions_by_zobrist_after_rebuild); !rebuild_res) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    delete_checkpoint(db_.dir());

    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds {current_time_ns() - start_time_ns_.load(std::memory_order_relaxed)});

    return import_summary {
        .total_attempted = games_processed_.load(std::memory_order_relaxed),
        .committed = static_cast<std::size_t>(committed - pre_committed),
        .skipped = games_skipped_.load(std::memory_order_relaxed),
        .errors = games_errored_.load(std::memory_order_relaxed),
        .elapsed = elapsed,
    };
}

}  // namespace motif::import
