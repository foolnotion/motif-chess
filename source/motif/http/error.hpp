#pragma once

#include <cstdint>

#include <tl/expected.hpp>

namespace motif::http
{

enum class error_code : std::uint8_t
{
    ok,
    db_open_failed,
    listen_failed
};

template<typename T>
using result = tl::expected<T, error_code>;

}  // namespace motif::http
