// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>
#include <span>

#include <duckdb.h>

#include "motif/db/error.hpp"
#include "motif/db/types.hpp"

namespace motif::db {

class position_store {
public:
    explicit position_store(duckdb_connection con) noexcept;

    auto initialize_schema() -> result<void>;
    auto insert_batch(std::span<position_row const> rows) -> result<void>;
    auto row_count() -> result<std::int64_t>;

private:
    duckdb_connection con_;
};

} // namespace motif::db
