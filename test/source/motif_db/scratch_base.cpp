#include <exception>

#include "scratch_base.hpp"

#include <duckdb.h>
#include <sqlite3.h>

#include "motif/db/error.hpp"
#include "motif/db/game_store.hpp"
#include "motif/db/schema.hpp"

namespace motif::db
{

namespace
{
// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
void must(result<void> const& res)
{
    if (!res) {
        std::terminate();
    }
}
}  // namespace

scratch_base::scratch_base()
{
    sqlite3_open(":memory:", &conn_);
    must(schema::initialize(conn_));
    store_.emplace(conn_);

    duckdb_open(nullptr, &duck_db_);
    duckdb_connect(duck_db_, &duck_con_);
    positions_.emplace(duck_con_);
    must(positions_->initialize_schema());
}

scratch_base::~scratch_base()
{
    positions_.reset();
    if (duck_con_ != nullptr) {
        duckdb_disconnect(&duck_con_);
        duck_con_ = nullptr;
    }
    if (duck_db_ != nullptr) {
        duckdb_close(&duck_db_);
        duck_db_ = nullptr;
    }
    store_.reset();
    if (conn_ != nullptr) {
        sqlite3_close(conn_);
        conn_ = nullptr;
    }
}

auto scratch_base::instance() -> scratch_base&
{
    static scratch_base s_instance;
    return s_instance;
}

auto scratch_base::store() noexcept -> game_store&
{
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *store_;
}

auto scratch_base::positions() noexcept -> position_store&
{
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *positions_;
}

}  // namespace motif::db
