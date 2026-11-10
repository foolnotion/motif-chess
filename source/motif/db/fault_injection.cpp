#include <atomic>
#include <cstdint>

#include "motif/db/fault_injection.hpp"

namespace motif::db::fault
{

namespace
{
// The seam is deliberately process-global mutable state (see fault_injection.hpp).
std::atomic<hook_function> installed_hook {nullptr};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
// Per-thread scratch naming the connection of the hook call in flight.
thread_local sqlite3* current_connection {nullptr};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
}  // namespace

auto set_hook(hook_function const hook) noexcept -> hook_function
{
    return installed_hook.exchange(hook, std::memory_order_acq_rel);
}

auto inject(point const where, artifact_kind const kind, std::uint64_t const publication_generation, sqlite3* const connection) noexcept
    -> fault_action
{
    auto const hook = installed_hook.load(std::memory_order_acquire);
    if (hook == nullptr) {
        return fault_action::proceed;
    }
    current_connection = connection;
    auto const action = hook(where, kind, publication_generation);
    current_connection = nullptr;
    return action;
}

auto hook_connection() noexcept -> sqlite3*
{
    return current_connection;
}

}  // namespace motif::db::fault
