#include <algorithm>
#include <cstdint>
#include <mutex>
#include <utility>

#include "motif/db/resource_governor.hpp"

namespace motif::db
{

sqlite_cache_lease::~sqlite_cache_lease()
{
    release();
}

sqlite_cache_lease::sqlite_cache_lease(sqlite_cache_lease&& other) noexcept
    : owner_ {std::exchange(other.owner_, nullptr)}
    , bytes_ {std::exchange(other.bytes_, 0U)}
{
}

auto sqlite_cache_lease::operator=(sqlite_cache_lease&& other) noexcept -> sqlite_cache_lease&
{
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0U);
    }
    return *this;
}

void sqlite_cache_lease::release() noexcept
{
    if (owner_ == nullptr) {
        return;
    }
    {
        auto const lock = std::scoped_lock {owner_->mutex_};
        owner_->sqlite_cache_in_use_ -= bytes_;
    }
    owner_ = nullptr;
    bytes_ = 0U;
}

auto resource_governor::process() noexcept -> resource_governor&
{
    static resource_governor governor;
    return governor;
}

auto resource_governor::acquire_sqlite_cache() -> sqlite_cache_lease
{
    auto const lock = std::scoped_lock {mutex_};
    auto const remaining =
        limits_.total_sqlite_cache_bytes > sqlite_cache_in_use_ ? limits_.total_sqlite_cache_bytes - sqlite_cache_in_use_ : 0U;
    auto const grant = std::max(limits_.min_bundle_sqlite_cache_bytes, std::min(limits_.max_bundle_sqlite_cache_bytes, remaining));
    sqlite_cache_in_use_ += grant;
    return sqlite_cache_lease {this, grant};
}

auto resource_governor::sqlite_cache_in_use() const -> std::uint64_t
{
    auto const lock = std::scoped_lock {mutex_};
    return sqlite_cache_in_use_;
}

}  // namespace motif::db
