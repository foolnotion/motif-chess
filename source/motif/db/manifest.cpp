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
#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <unistd.h>
#endif

#include "motif/db/error.hpp"
#include "motif/db/schema.hpp"

namespace motif::db
{

namespace
{

auto sync_path(std::filesystem::path const& path, bool const directory) -> bool
{
#ifdef _WIN32
    if (directory) {
        return true;
    }
    auto const flags = directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    auto const handle = CreateFileW(path.c_str(),
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    flags,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    auto const result = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return result;
#else
    auto const flags = directory ? O_RDONLY | O_DIRECTORY | O_CLOEXEC : O_RDONLY | O_CLOEXEC;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg) -- POSIX open is required for fsync durability.
    auto const descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        return false;
    }
    auto const result = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return result;
#endif
}

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
        .source_generation = 0,
        .position_postings = std::nullopt,
        .opening_tree_index = std::nullopt,
    };
}

auto write_manifest(std::filesystem::path const& path, db_manifest const& manifest) -> result<void>
{
    std::string buffer;
    auto const write_err = glz::write_json(manifest, buffer);
    if (write_err) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const temp_path = path.string() + ".tmp";
    std::ofstream file {temp_path, std::ios::trunc};
    if (!file.is_open()) {
        return tl::unexpected {error_code::io_failure};
    }
    file << buffer;
    if (!file) {
        return tl::unexpected {error_code::io_failure};
    }
    file.close();
    if (!file) {
        return tl::unexpected {error_code::io_failure};
    }
    if (!sync_path(temp_path, /*directory=*/false)) {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        return tl::unexpected {error_code::io_failure};
    }
    std::error_code fs_err;
#ifdef _WIN32
    if (MoveFileExW(std::filesystem::path {temp_path}.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::filesystem::remove(temp_path, fs_err);
        return tl::unexpected {error_code::io_failure};
    }
#else
    std::filesystem::rename(temp_path, path, fs_err);
    if (fs_err) {
        std::filesystem::remove(temp_path, fs_err);
        return tl::unexpected {error_code::io_failure};
    }
#endif
    auto const directory_path = path.parent_path().empty() ? std::filesystem::path {"."} : path.parent_path();
    if (!sync_path(directory_path, /*directory=*/true)) {
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

    std::ostringstream buffer_stream;
    buffer_stream << file.rdbuf();
    if (!file) {
        return tl::unexpected {error_code::io_failure};
    }
    auto const buffer = buffer_stream.str();
    db_manifest parsed;
    auto const read_err = glz::read_json(parsed, buffer);
    if (read_err) {
        return tl::unexpected {error_code::io_failure};
    }
    auto valid_artifact_name = [](std::optional<derived_index_manifest_entry> const& entry, std::string_view prefix) -> bool
    {
        if (!entry) {
            return true;
        }
        auto const artifact_path = std::filesystem::path {entry->filename};
        return !entry->filename.empty() && !artifact_path.has_parent_path() && artifact_path.filename() == artifact_path
            && entry->filename.starts_with(prefix);
    };
    if (!valid_artifact_name(parsed.position_postings, "positions.postings")
        || !valid_artifact_name(parsed.opening_tree_index, "opening_tree.idx"))
    {
        return tl::unexpected {error_code::schema_mismatch};
    }
    return parsed;
}

}  // namespace motif::db
