#pragma once

#include <memory>

#include <sqlite3.h>

namespace motif::db::detail
{

struct stmt_deleter
{
    auto operator()(sqlite3_stmt* stmt) const noexcept -> void { sqlite3_finalize(stmt); }
};

using unique_stmt = std::unique_ptr<sqlite3_stmt, stmt_deleter>;

}  // namespace motif::db::detail
