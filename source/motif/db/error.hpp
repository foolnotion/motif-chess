#pragma once

#include <cstdint>
#include <string>
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
    not_editable,
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
        case error_code::not_editable:
            return "not_editable";
    }

    return "unknown";
}

[[nodiscard]] inline auto to_string(error const& err) noexcept -> std::string_view
{
    return to_string(err.code);
}

}  // namespace motif::db
