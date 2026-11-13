#pragma once

#include <cstdint>
#include <mutex>

namespace motif::db
{

class resource_governor;

// Defaults keep a single bundle at the historical 200 MB SQLite page cache.
inline constexpr std::uint64_t default_total_sqlite_cache_bytes = std::uint64_t {1} << 30U;
inline constexpr std::uint64_t default_max_bundle_sqlite_cache_bytes = std::uint64_t {200'000} * 1024U;
inline constexpr std::uint64_t default_min_bundle_sqlite_cache_bytes = std::uint64_t {16} << 20U;

// Per-process limits shared by every open bundle.
struct resource_limits
{
    // Sum of page-cache grants handed to all open bundles.
    std::uint64_t total_sqlite_cache_bytes {default_total_sqlite_cache_bytes};
    // What an uncontended bundle receives.
    std::uint64_t max_bundle_sqlite_cache_bytes {default_max_bundle_sqlite_cache_bytes};
    // Every bundle receives at least this much, even when the total is
    // exhausted, so the sum can exceed the total by up to floor * bundles.
    std::uint64_t min_bundle_sqlite_cache_bytes {default_min_bundle_sqlite_cache_bytes};
};

// RAII share of the page-cache budget; returns it on destruction.
class sqlite_cache_lease
{
  public:
    sqlite_cache_lease() noexcept = default;
    ~sqlite_cache_lease();
    sqlite_cache_lease(sqlite_cache_lease const&) = delete;
    auto operator=(sqlite_cache_lease const&) -> sqlite_cache_lease& = delete;
    sqlite_cache_lease(sqlite_cache_lease&& other) noexcept;
    auto operator=(sqlite_cache_lease&& other) noexcept -> sqlite_cache_lease&;

    [[nodiscard]] auto bytes() const noexcept -> std::uint64_t { return bytes_; }

  private:
    friend class resource_governor;

    sqlite_cache_lease(resource_governor* owner, std::uint64_t bytes) noexcept
        : owner_ {owner}
        , bytes_ {bytes}
    {
    }

    void release() noexcept;

    resource_governor* owner_ {nullptr};
    std::uint64_t bytes_ {};
};

// Thread-safe accounting for resources shared across open bundles. A lease
// must not outlive its governor.
class resource_governor
{
  public:
    explicit resource_governor(resource_limits limits = {}) noexcept
        : limits_ {limits}
    {
    }

    resource_governor(resource_governor const&) = delete;
    auto operator=(resource_governor const&) -> resource_governor& = delete;
    resource_governor(resource_governor&&) = delete;
    auto operator=(resource_governor&&) -> resource_governor& = delete;
    ~resource_governor() = default;

    [[nodiscard]] static auto process() noexcept -> resource_governor&;

    [[nodiscard]] auto acquire_sqlite_cache() -> sqlite_cache_lease;
    [[nodiscard]] auto sqlite_cache_in_use() const -> std::uint64_t;

  private:
    friend class sqlite_cache_lease;

    resource_limits limits_;
    mutable std::mutex mutex_;
    std::uint64_t sqlite_cache_in_use_ {};
};

}  // namespace motif::db
