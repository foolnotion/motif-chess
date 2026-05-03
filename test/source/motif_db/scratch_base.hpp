#pragma once

#include <optional>

#include <duckdb.h>

#include "motif/db/game_store.hpp"
#include "motif/db/position_store.hpp"

struct sqlite3;

namespace motif::db
{

class scratch_base
{
  public:
    static auto instance() -> scratch_base&;

    scratch_base(scratch_base const&) = delete;
    auto operator=(scratch_base const&) -> scratch_base& = delete;
    scratch_base(scratch_base&&) = delete;
    auto operator=(scratch_base&&) -> scratch_base& = delete;

    [[nodiscard]] auto store() noexcept -> game_store&;
    [[nodiscard]] auto positions() noexcept -> position_store&;

  private:
    scratch_base();
    ~scratch_base();

    sqlite3* conn_ {nullptr};
    duckdb_database duck_db_ {nullptr};
    duckdb_connection duck_con_ {nullptr};
    std::optional<game_store> store_;
    std::optional<position_store> positions_;
};

}  // namespace motif::db
