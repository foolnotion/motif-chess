#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "motif/import/checkpoint.hpp"

#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <tl/expected.hpp>

#include "motif/db/manifest.hpp"
#include "motif/import/error.hpp"

namespace motif::import
{

auto checkpoint_path(std::filesystem::path const& db_dir) -> std::filesystem::path
{
    return db_dir / "import.checkpoint.json";
}

auto write_checkpoint(std::filesystem::path const& db_dir, import_checkpoint const& checkpoint) -> result<void>
{
    std::string buffer;
    auto const write_err = glz::write_json(checkpoint, buffer);
    if (write_err) {
        return tl::unexpected {error_code::io_failure};
    }

    auto const path = checkpoint_path(db_dir);
    auto const temp_path = std::filesystem::path {path.string() + ".tmp"};
    {
        std::ofstream file {temp_path, std::ios::trunc};
        if (!file.is_open()) {
            return tl::unexpected {error_code::io_failure};
        }
        file << buffer;
        file.close();
        if (file.fail()) {
            std::error_code cleanup_err;
            std::filesystem::remove(temp_path, cleanup_err);
            return tl::unexpected {error_code::io_failure};
        }
    }

    // Sync the completed file before replacing the marker, then sync its parent
    // directory. The directory operation is best-effort on Windows.
    if (!motif::db::sync_filesystem_path(temp_path, /*directory=*/false)) {
        std::error_code cleanup_err;
        std::filesystem::remove(temp_path, cleanup_err);
        return tl::unexpected {error_code::io_failure};
    }

    std::error_code fserr;
    std::filesystem::rename(temp_path, path, fserr);
    if (fserr) {
        std::filesystem::remove(temp_path, fserr);
        return tl::unexpected {error_code::io_failure};
    }
    if (!motif::db::sync_filesystem_path(db_dir, /*directory=*/true)) {
        return tl::unexpected {error_code::io_failure};
    }
    return {};
}

auto read_checkpoint(std::filesystem::path const& db_dir) -> result<import_checkpoint>
{
    auto const path = checkpoint_path(db_dir);
    std::error_code fserr;
    auto const exists = std::filesystem::exists(path, fserr);
    if (fserr) {
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

    import_checkpoint checkpoint;
    auto const read_err = glz::read<glz::opts {.error_on_unknown_keys = false}>(checkpoint, oss.str());
    if (read_err) {
        return tl::unexpected {error_code::io_failure};
    }
    return checkpoint;
}

auto delete_checkpoint(std::filesystem::path const& db_dir) noexcept -> void
{
    std::error_code fserr;
    auto const removed = std::filesystem::remove(checkpoint_path(db_dir), fserr);
    // Persist the removal where directory sync is supported. Callers deleting
    // an uploaded source must also handle sync failure conservatively.
    if (removed && !fserr) {
        static_cast<void>(motif::db::sync_filesystem_path(db_dir, /*directory=*/true));
    }
}

}  // namespace motif::import
