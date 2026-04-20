#pragma once

#include <filesystem>

#include "motif/import/error.hpp"

namespace motif::import
{

struct log_config
{
    std::filesystem::path log_dir {"logs"};
    bool json_sink {false};
};

auto initialize_logging(log_config const& cfg) -> result<void>;
auto shutdown_logging() -> result<void>;

}  // namespace motif::import
