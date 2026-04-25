#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "motif/db/manifest.hpp"

#include <fmt/chrono.h>  // NOLINT(misc-include-cleaner) — provides chrono format specializations
#include <fmt/format.h>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <tl/expected.hpp>

#include "motif/db/error.hpp"
#include "motif/db/schema.hpp"

namespace motif::db
{

namespace
{

// NOLINTNEXTLINE(llvm-prefer-static-over-anonymous-namespace)
auto now_iso8601() -> std::string
{
    auto const now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return fmt::format("{:%Y-%m-%dT%H:%M:%S}Z", now);
}

}  // namespace

auto make_manifest(std::string const& name) -> db_manifest
{
    return db_manifest {
        .name = name,
        .schema_version = schema::current_version,
        .game_count = 0,
        .created_at = now_iso8601(),
    };
}

auto write_manifest(std::filesystem::path const& path, db_manifest const& manifest) -> result<void>
{
    std::string buffer;
    auto const write_err = glz::write_json(manifest, buffer);
    if (write_err) {
        return tl::unexpected {error_code::io_failure};
    }

    std::ofstream file {path};
    if (!file.is_open()) {
        return tl::unexpected {error_code::io_failure};
    }
    file << buffer;
    if (!file) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto read_manifest(std::filesystem::path const& path) -> result<db_manifest>
{
    std::error_code fs_err;
    auto const exists = std::filesystem::exists(path, fs_err);
    if (fs_err) {
        return tl::unexpected {error_code::io_failure};
    }
    if (!exists) {
        return tl::unexpected {error_code::not_found};
    }

    std::ifstream file {path};
    if (!file.is_open()) {
        return tl::unexpected {error_code::io_failure};
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    if (!file) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const buffer = oss.str();

    db_manifest parsed;
    auto const read_err = glz::read_json(parsed, buffer);
    if (read_err) {
        return tl::unexpected {error_code::io_failure};
    }
    return parsed;
}

}  // namespace motif::db
