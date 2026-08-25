#pragma once

#include <cstdint>

namespace motif::slint_app
{

enum class error_code : std::uint8_t
{
    database_failure,
    invalid_argument,
};

}  // namespace motif::slint_app
