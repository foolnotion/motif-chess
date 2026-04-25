// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <cstdint>
#include <string_view>

#include <tl/expected.hpp>

namespace motif::db
{

enum class error_code : std::uint8_t
{
    ok,
    not_found,
    schema_mismatch,
    io_failure,
    duplicate,
};

template<typename T>
using result = tl::expected<T, error_code>;

[[nodiscard]] constexpr auto to_string(error_code code) noexcept
    -> std::string_view
{
    switch (code) {
        case error_code::ok:
            return "ok";
        case error_code::not_found:
            return "not_found";
        case error_code::schema_mismatch:
            return "schema_mismatch";
        case error_code::io_failure:
            return "io_failure";
        case error_code::duplicate:
            return "duplicate";
    }

    return "unknown";
}

}  // namespace motif::db
