#pragma once

#include <cstddef>
#include <cstdint>

#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)

#include "motif/db/game_store.hpp"
#include "motif/db/position_store.hpp"
#include "motif/import/error.hpp"

namespace motif::import
{

struct process_result
{
    std::uint32_t game_id {};
    std::size_t positions_inserted {};
};

class import_worker
{
  public:
    explicit import_worker(motif::db::game_store& store, motif::db::position_store& positions) noexcept;

    // Convert pgn::game to stored game row + DuckDB position rows.
    // Errors:
    //   duplicate   — identity key already in DB; no rows written
    //   parse_error — chesslib rejected a SAN move in the main line; no rows
    //   written io_failure  — a DB write failed
    [[nodiscard]] auto process(pgn::game const& pgn_game) -> result<process_result>;

  private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    motif::db::game_store& store_;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    motif::db::position_store& positions_;
};

}  // namespace motif::import
