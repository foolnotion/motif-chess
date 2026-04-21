#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "motif/import/import_pipeline.hpp"

#include <chesslib/board/board.hpp>  // NOLINT(misc-include-cleaner)
#include <chesslib/board/move_codec.hpp>
#include <chesslib/util/san_replay.hpp>
#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)
#include <spdlog/spdlog.h>
#include <taskflow/algorithm/pipeline.hpp>  // NOLINT(misc-include-cleaner)
#include <taskflow/taskflow.hpp>  // NOLINT(misc-include-cleaner)
#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/position_store.hpp"
#include "motif/db/types.hpp"
#include "motif/import/checkpoint.hpp"
#include "motif/import/error.hpp"
#include "motif/import/pgn_reader.hpp"

namespace motif::import
{

namespace
{

constexpr std::int16_t k_max_elo = 32767;

auto current_time_ns() noexcept -> std::int64_t
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

auto find_tag(std::vector<pgn::tag> const& tags, std::string_view key)
    -> std::string
{
    for (auto const& tag : tags) {
        if (tag.key == key) {
            return tag.value;
        }
    }
    return {};
}

auto parse_elo(std::string const& raw) -> std::optional<std::int16_t>
{
    if (raw.empty() || raw == "?") {
        return std::nullopt;
    }
    int val {};
    auto const* const beg = raw.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto const* const fin = raw.data() + raw.size();
    auto const parsed = std::from_chars(beg, fin, val);
    if (parsed.ec != std::errc {} || parsed.ptr != fin || val < 0
        || val > k_max_elo)
    {
        return std::nullopt;
    }
    return static_cast<std::int16_t>(val);
}

auto pgn_result_to_string(pgn::result res) noexcept -> std::string
{
    switch (res) {
        case pgn::result::white:
            return "1-0";
        case pgn::result::black:
            return "0-1";
        case pgn::result::draw:
            return "1/2-1/2";
        case pgn::result::unknown:
            return "*";
    }
    return "*";
}

auto pgn_result_to_int8(pgn::result res) noexcept -> std::int8_t
{
    switch (res) {
        case pgn::result::white:
            return 1;
        case pgn::result::black:
            return -1;
        default:
            return 0;
    }
}

struct prepared_game
{
    motif::db::game game_row;
    std::vector<motif::db::position_row> position_rows;
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
    auto const result_int = pgn_result_to_int8(pgn_game.result);

    auto const white_title_raw = find_tag(tags, "WhiteTitle");
    auto const black_title_raw = find_tag(tags, "BlackTitle");
    auto const event_name = find_tag(tags, "Event");
    auto const site_raw = find_tag(tags, "Site");
    auto const date_raw = find_tag(tags, "Date");
    auto const eco_raw = find_tag(tags, "ECO");

    auto const valid_date = (!date_raw.empty() && date_raw != "????.??.??")
        ? std::optional<std::string> {date_raw}
        : std::nullopt;

    motif::db::game game_row;
    game_row.white.name = find_tag(tags, "White");
    game_row.white.elo = white_elo_opt
        ? std::optional<std::int32_t> {*white_elo_opt}
        : std::nullopt;
    game_row.white.title = white_title_raw.empty()
        ? std::nullopt
        : std::optional<std::string> {white_title_raw};

    game_row.black.name = find_tag(tags, "Black");
    game_row.black.elo = black_elo_opt
        ? std::optional<std::int32_t> {*black_elo_opt}
        : std::nullopt;
    game_row.black.title = black_title_raw.empty()
        ? std::nullopt
        : std::optional<std::string> {black_title_raw};

    if (!event_name.empty()) {
        game_row.event_details = motif::db::event {
            .name = event_name,
            .site = site_raw.empty() ? std::nullopt
                                     : std::optional<std::string> {site_raw},
            .date = valid_date,
        };
    }

    game_row.date = valid_date;
    game_row.eco =
        eco_raw.empty() ? std::nullopt : std::optional<std::string> {eco_raw};
    game_row.result = pgn_result_to_string(pgn_game.result);

    if (pgn_game.moves.size() >= std::numeric_limits<std::uint16_t>::max()) {
        return tl::unexpected {error_code::parse_error};
    }

    auto board = chesslib::board {};
    std::vector<std::uint16_t> encoded_moves;
    std::vector<motif::db::position_row> position_rows;
    encoded_moves.reserve(pgn_game.moves.size());
    position_rows.reserve(pgn_game.moves.size());

    auto replay = chesslib::san::replayer {board};
    for (auto const& node : pgn_game.moves) {
        auto move_result = replay.play(node.san);
        if (!move_result) {
            return tl::unexpected {error_code::parse_error};
        }
        encoded_moves.push_back(chesslib::codec::encode(*move_result));
        position_rows.push_back(motif::db::position_row {
            .zobrist_hash = board.hash(),
            .game_id = 0,
            .ply = static_cast<std::uint16_t>(encoded_moves.size()),
            .result = result_int,
            .white_elo = white_elo_opt,
            .black_elo = black_elo_opt,
        });
    }

    game_row.moves = std::move(encoded_moves);
    return prepared_game {.game_row = std::move(game_row),
                          .position_rows = std::move(position_rows)};
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
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::nanoseconds {now_ns - start_time_ns});
    }

    return import_progress {
        .games_processed = games_processed_.load(std::memory_order_relaxed),
        .games_committed = games_committed_.load(std::memory_order_relaxed),
        .games_skipped = games_skipped_.load(std::memory_order_relaxed),
        .elapsed = elapsed,
    };
}

auto import_pipeline::run(std::filesystem::path const& pgn_path,
                          import_config const& config) -> result<import_summary>
{
    return run_from(pgn_path, 0, 0, 0, config);
}

auto import_pipeline::resume(std::filesystem::path const& pgn_path,
                             import_config const& config)
    -> result<import_summary>
{
    auto chk = read_checkpoint(db_.dir());
    if (!chk) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const checkpoint_source =
        std::filesystem::absolute(std::filesystem::path {chk->source_path})
            .lexically_normal();
    auto const requested_source =
        std::filesystem::absolute(pgn_path).lexically_normal();
    if (checkpoint_source != requested_source) {
        return tl::unexpected {error_code::invalid_state};
    }

    return run_from(pgn_path,
                    chk->byte_offset,
                    chk->games_committed,
                    chk->last_game_id,
                    config);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto import_pipeline::run_from(
    std::filesystem::path const& pgn_path,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    std::size_t start_offset,
    std::int64_t pre_committed,
    std::int64_t pre_last_game_id,
    import_config const& config) -> result<import_summary>
{
    auto log = spdlog::get("motif.import");

    games_processed_.store(0, std::memory_order_relaxed);
    games_committed_.store(0, std::memory_order_relaxed);
    games_skipped_.store(0, std::memory_order_relaxed);
    start_time_ns_.store(current_time_ns(), std::memory_order_relaxed);

    pgn_reader reader {pgn_path};
    if (start_offset > 0) {
        if (auto seek_res = reader.seek_to_offset(start_offset); !seek_res) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    std::int64_t last_game_id = pre_last_game_id;
    std::int64_t committed = pre_committed;
    std::size_t batch_pending = 0;
    bool eof_reached = false;
    std::optional<error_code> fatal_error;

    std::vector<pipeline_slot> slots(config.num_lines);

    auto stage0 = [&](tf::Pipeflow& pflow) -> void
    {
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
            if (log) {
                log->warn("game skipped: parse error at offset {}",
                          slot.game_start_offset);
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
            games_skipped_.fetch_add(1, std::memory_order_relaxed);
            if (ins.error() == motif::db::error_code::duplicate) {
                if (log) {
                    log->warn("duplicate game skipped at offset {}",
                              slot.game_start_offset);
                }
            } else {
                if (log) {
                    log->error("SQLite insert failed at offset {}",
                               slot.game_start_offset);
                }
            }
            slot.prepared.reset();
            slot.state = slot_state::empty;
            return;
        }

        auto const game_id = *ins;
        for (auto& row : prep.position_rows) {
            row.game_id = game_id;
        }

        if (!prep.position_rows.empty()) {
            if (auto pos_res = db_.positions().insert_batch(prep.position_rows);
                !pos_res)
            {
                if (log) {
                    log->error("DuckDB insert_batch failed for game_id {}; "
                               "run rebuild_position_store to recover",
                               game_id);
                }
            }
        }

        last_game_id = static_cast<std::int64_t>(game_id);
        committed++;
        batch_pending++;
        games_committed_.fetch_add(1, std::memory_order_relaxed);

        if (batch_pending >= config.batch_size) {
            import_checkpoint const chk {
                .source_path = pgn_path.string(),
                .byte_offset = slot.next_game_offset,
                .games_committed = committed,
                .last_game_id = last_game_id,
            };
            if (auto wres = write_checkpoint(db_.dir(), chk); !wres) {
                if (log) {
                    log->error("checkpoint write failed");
                }
            }
            batch_pending = 0;
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
        return tl::unexpected {*fatal_error};
    }

    delete_checkpoint(db_.dir());

    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds {
            current_time_ns()
            - start_time_ns_.load(std::memory_order_relaxed)});

    return import_summary {
        .total_attempted = games_processed_.load(std::memory_order_relaxed),
        .committed = static_cast<std::size_t>(committed - pre_committed),
        .skipped = games_skipped_.load(std::memory_order_relaxed),
        .errors = 0,
        .elapsed = elapsed,
    };
}

}  // namespace motif::import
