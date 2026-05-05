#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <tl/expected.hpp>

namespace motif::engine
{

enum class error_code : std::uint8_t
{
    analysis_not_found,
    analysis_already_terminal,
    engine_not_configured,
    invalid_analysis_params,
    engine_start_failed,
    engine_stop_failed,
};

struct error
{
    error_code code;
    std::string message;

    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
    error(error_code err_code) noexcept
        : code(err_code)
    {
    }

    error(error_code err_code, std::string msg)
        : code(err_code)
        , message(std::move(msg))
    {
    }

    [[nodiscard]] friend auto operator==(error const& lhs, error_code rhs) noexcept -> bool { return lhs.code == rhs; }
};

template<typename T>
using result = tl::expected<T, error>;

[[nodiscard]] constexpr auto to_string(error_code code) noexcept -> std::string_view
{
    switch (code) {
        case error_code::analysis_not_found:
            return "analysis_not_found";
        case error_code::analysis_already_terminal:
            return "analysis_already_terminal";
        case error_code::engine_not_configured:
            return "engine_not_configured";
        case error_code::invalid_analysis_params:
            return "invalid_analysis_params";
        case error_code::engine_start_failed:
            return "engine_start_failed";
        case error_code::engine_stop_failed:
            return "engine_stop_failed";
    }

    return "unknown";
}

[[nodiscard]] inline auto to_string(error const& err) noexcept -> std::string_view
{
    return to_string(err.code);
}

}  // namespace motif::engine
