// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>
#include <string_view>

#include <tl/expected.hpp>

namespace motif::search
{

enum class error_code : std::uint8_t
{
    ok,
    invalid_argument,
    io_failure,
};

template<typename T>
using result = tl::expected<T, error_code>;

[[nodiscard]] constexpr auto to_string(error_code code) noexcept
    -> std::string_view
{
    switch (code) {
        case error_code::ok:
            return "ok";
        case error_code::invalid_argument:
            return "invalid_argument";
        case error_code::io_failure:
            return "io_failure";
    }

    return "unknown";
}

}  // namespace motif::search
