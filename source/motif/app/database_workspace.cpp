#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "motif/app/database_workspace.hpp"

#include <fmt/format.h>
#include <tl/expected.hpp>

#include "motif/app/app_config.hpp"
#include "motif/app/error.hpp"
#include "motif/db/database_manager.hpp"
#include "motif/db/error.hpp"

namespace motif::app
{

database_workspace::database_workspace(app_config* config)
    : config_(config)
{
}

auto database_workspace::create_database(std::string const& dir_path, std::string const& name) -> result<void>
{
    auto create_res = motif::db::database_manager::create(std::filesystem::path {dir_path}, name);
    if (!create_res) {
        return tl::unexpected {error {error_code::io_failure, fmt::format("create failed: {}", motif::db::to_string(create_res.error()))}};
    }
    auto next_config = *config_;
    push_recent(next_config, name, dir_path);
    if (auto save_res = save_config(next_config); !save_res) {
        return tl::unexpected {error {save_res.error().code, "failed to persist recent databases"}};
    }

    *config_ = std::move(next_config);
    db_.emplace(std::move(*create_res));
    kind_ = database_kind::persistent;
    display_name_ = name;
    active_path_ = dir_path;
    return {};
}

auto database_workspace::open_database(std::string const& dir_path) -> result<void>
{
    auto open_res = motif::db::database_manager::open(std::filesystem::path {dir_path});
    if (!open_res) {
        return tl::unexpected {error {error_code::io_failure, fmt::format("open failed: {}", motif::db::to_string(open_res.error()))}};
    }
    auto const opened_name = std::string {open_res->manifest().name};
    auto next_config = *config_;
    push_recent(next_config, opened_name, dir_path);
    if (auto save_res = save_config(next_config); !save_res) {
        return tl::unexpected {error {save_res.error().code, "failed to persist recent databases"}};
    }

    // Only replace active database on full success.
    *config_ = std::move(next_config);
    db_.emplace(std::move(*open_res));
    kind_ = database_kind::persistent;
    display_name_ = opened_name;
    active_path_ = dir_path;
    return {};
}

auto database_workspace::open_scratch() -> result<void>
{
    auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto const scratch_dir = std::filesystem::temp_directory_path() / fmt::format("motif_scratch_{}", tick);
    auto scratch_res = motif::db::database_manager::create(scratch_dir, "Scratch");
    if (!scratch_res) {
        return tl::unexpected {error {error_code::io_failure, "failed to open scratch database"}};
    }
    db_.emplace(std::move(*scratch_res));
    kind_ = database_kind::scratch;
    display_name_ = "Scratch";
    active_path_ = "";
    scratch_dir_ = scratch_dir;
    return {};
}

void database_workspace::close_active()
{
    if (db_) {
        db_->close();
        db_.reset();
    }
    if (scratch_dir_) {
        std::error_code remove_err;
        std::filesystem::remove_all(*scratch_dir_, remove_err);
        scratch_dir_.reset();
    }
    kind_ = database_kind::none;
    display_name_ = "";
    active_path_ = "";
}

auto database_workspace::remove_recent_entry(std::string const& path) -> result<void>
{
    auto next_config = *config_;
    remove_recent(next_config, path);
    if (auto save_res = save_config(next_config); !save_res) {
        return tl::unexpected {error {save_res.error().code, "failed to persist recent databases"}};
    }
    *config_ = std::move(next_config);
    return {};
}

auto database_workspace::recent_with_status() const -> std::vector<recent_status>
{
    std::vector<recent_status> result;
    result.reserve(config_->recent_databases.size());
    for (auto const& entry : config_->recent_databases) {
        std::error_code fs_err;
        auto const root = std::filesystem::path {entry.path};
        auto const has_games = std::filesystem::exists(root / "games.db", fs_err);
        auto const has_duckdb = !fs_err && std::filesystem::exists(root / "positions.duckdb", fs_err);
        auto const has_manifest = !fs_err && std::filesystem::exists(root / "manifest.json", fs_err);
        result.push_back(recent_status {
            .entry = entry,
            .available = (!fs_err && has_games && has_duckdb && has_manifest),
        });
    }
    return result;
}

auto database_workspace::persistent_db() noexcept -> motif::db::database_manager*
{
    if (kind_ == database_kind::persistent && db_) {
        return &*db_;
    }
    return nullptr;
}

}  // namespace motif::app
