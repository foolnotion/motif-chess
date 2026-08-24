#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <system_error>

#include "motif/import/checkpoint.hpp"

#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <tl/expected.hpp>

#include "motif/import/error.hpp"

namespace motif::import
{

auto stat_source(std::filesystem::path const& source_path) -> result<source_stat>
{
    std::error_code fserr;
    auto const size = std::filesystem::file_size(source_path, fserr);
    if (fserr) {
        return tl::unexpected {error_code::io_failure};
    }
    fserr.clear();
    auto const mtime = std::filesystem::last_write_time(source_path, fserr);
    if (fserr) {
        return tl::unexpected {error_code::io_failure};
    }
    return source_stat {
        .size = static_cast<std::uint64_t>(size),
        .mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count(),
    };
}

auto hash_source(std::filesystem::path const& source_path) -> result<std::uint64_t>
{
    auto file = std::ifstream {source_path, std::ios::binary};
    if (!file.is_open()) {
        return tl::unexpected {error_code::io_failure};
    }

    constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    auto hash = offset_basis;
    constexpr std::size_t hash_buffer_size = 65'536;
    auto buffer = std::array<char, hash_buffer_size> {};
    while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || file.gcount() > 0) {
        auto const count = static_cast<std::size_t>(file.gcount());
        for (auto const byte : std::span {buffer}.first(count)) {
            hash ^= static_cast<unsigned char>(byte);
            hash *= prime;
        }
    }
    if (!file.eof()) {
        return tl::unexpected {error_code::io_failure};
    }
    return hash;
}

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
        if (!file) {
            return tl::unexpected {error_code::io_failure};
        }
    }

    std::error_code fserr;
    std::filesystem::rename(temp_path, path, fserr);
    if (fserr) {
        std::filesystem::remove(temp_path, fserr);
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
    auto const read_err = glz::read_json(checkpoint, oss.str());
    if (read_err) {
        return tl::unexpected {error_code::io_failure};
    }
    return checkpoint;
}

auto delete_checkpoint(std::filesystem::path const& db_dir) noexcept -> void
{
    std::error_code fserr;
    std::filesystem::remove(checkpoint_path(db_dir), fserr);
    // ignore fserr — silently succeed if absent
}

}  // namespace motif::import
