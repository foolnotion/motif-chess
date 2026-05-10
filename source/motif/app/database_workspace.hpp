#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "motif/app/app_config.hpp"
#include "motif/app/error.hpp"
#include "motif/db/database_manager.hpp"

namespace motif::app
{

enum class database_kind : std::uint8_t
{
    none,
    persistent,
    scratch,
};

struct recent_status
{
    recent_database_entry entry;
    bool available {false};
};

class database_workspace
{
  public:
    // config must outlive the workspace.
    explicit database_workspace(app_config* config);

    // Create a new persistent bundle. On success, promotes to recent list and saves config.
    auto create_database(std::string const& dir_path, std::string const& name) -> result<void>;

    // Open an existing persistent bundle. On success, promotes to recent list and saves config.
    // Failed opens leave the previous active database unchanged.
    auto open_database(std::string const& dir_path) -> result<void>;

    // Set scratch as active. Creates a temporary on-disk bundle; never added to the recent list.
    // The bundle is removed when close_active() is called.
    auto open_scratch() -> result<void>;

    // Release the active database (returns to none state).
    void close_active();

    // Remove a recent database entry by path and save config.
    auto remove_recent_entry(std::string const& path) -> result<void>;

    // Refresh availability flags for all recent entries.
    [[nodiscard]] auto recent_with_status() const -> std::vector<recent_status>;

    [[nodiscard]] auto kind() const noexcept -> database_kind { return kind_; }

    [[nodiscard]] auto has_active() const noexcept -> bool { return kind_ != database_kind::none; }

    [[nodiscard]] auto display_name() const noexcept -> std::string_view { return display_name_; }

    [[nodiscard]] auto active_path() const noexcept -> std::string_view { return active_path_; }

    [[nodiscard]] auto is_temporary() const noexcept -> bool { return kind_ == database_kind::scratch; }

    // Returns nullptr if no database is active (both none and scratch return nullptr).
    [[nodiscard]] auto persistent_db() noexcept -> motif::db::database_manager*;

  private:
    app_config* config_;
    std::optional<motif::db::database_manager> db_;
    database_kind kind_ {database_kind::none};
    std::string display_name_;
    std::string active_path_;
    std::optional<std::filesystem::path> scratch_dir_;
};

}  // namespace motif::app
