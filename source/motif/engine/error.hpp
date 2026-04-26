#pragma once

#include <cstdint>

namespace motif::engine
{

enum class error_code : std::uint8_t
{
    analysis_not_found,
    analysis_already_terminal,
    engine_not_configured,
    engine_start_failed,
};

}  // namespace motif::engine
