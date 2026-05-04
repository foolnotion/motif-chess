#pragma once

#include <cstddef>
#include <cstdint>

#include <pgnlib/types.hpp>  // NOLINT(misc-include-cleaner)

#include "motif/db/database_manager.hpp"
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
    explicit import_worker(motif::db::database_manager& database) noexcept;

    // Convert pgn::game to stored game row + DuckDB position rows.
    // Errors:
    //   duplicate   — identity key already in DB; no rows written
    //   parse_error — chesslib rejected a SAN move in the main line; no rows written
    //   io_failure  — a DB write failed
    [[nodiscard]] auto process(pgn::game const& pgn_game) -> result<process_result>;

  private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    motif::db::database_manager& db_;
};

}  // namespace motif::import
