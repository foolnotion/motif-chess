// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>
#include <string_view>

#include <tl/expected.hpp>

namespace motif::import
{

enum class error_code : std::uint8_t
{
    ok,
    io_failure,
    invalid_state,
    eof,
    parse_error,
    duplicate,
    not_found,
    empty_game,
};

template<typename T>
using result = tl::expected<T, error_code>;

[[nodiscard]] constexpr auto to_string(error_code code) noexcept -> std::string_view
{
    switch (code) {
        case error_code::ok:
            return "ok";
        case error_code::io_failure:
            return "io_failure";
        case error_code::invalid_state:
            return "invalid_state";
        case error_code::eof:
            return "eof";
        case error_code::parse_error:
            return "parse_error";
        case error_code::duplicate:
            return "duplicate";
        case error_code::not_found:
            return "not_found";
        case error_code::empty_game:
            return "empty_game";
    }

    return "unknown";
}

}  // namespace motif::import
