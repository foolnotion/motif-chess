// Phase 2 stub — engine subprocess management deferred.
#include <string>

#include <tl/expected.hpp>

#include "motif/engine/engine_manager.hpp"
#include "motif/engine/error.hpp"

namespace motif::engine
{

// Stub methods intentionally remain non-static to preserve the Phase 2 manager API.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto engine_manager::start_analysis(analysis_params const& /*params*/) -> tl::expected<std::string, error_code>
{
    return tl::unexpected {error_code::engine_not_configured};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto engine_manager::stop_analysis(std::string const& /*analysis_id*/) -> tl::expected<void, error_code>
{
    return tl::unexpected {error_code::analysis_not_found};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto engine_manager::subscribe(std::string const& /*analysis_id*/,
                               info_callback const& /*on_info*/,
                               complete_callback const& /*on_complete*/,
                               error_callback const& /*on_error*/) -> tl::expected<subscription, error_code>
{
    return tl::unexpected {error_code::analysis_not_found};
}

}  // namespace motif::engine
