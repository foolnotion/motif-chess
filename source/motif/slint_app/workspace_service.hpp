#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

#include "motif/db/database_manager.hpp"
#include "motif/slint_app/config.hpp"
#include "motif/slint_app/error.hpp"

namespace motif::slint_app
{

template<typename T>
using result = tl::expected<T, error_code>;

enum class database_kind : std::uint8_t
{
    none,
    persistent,
    scratch,
};

// A recent-database entry together with whether its bundle is still on disk.
struct recent_database_state
{
    std::string name;
    std::string path;
    bool available {false};
};

// True when dir_path holds the files database_manager::open() requires
// (games.db and manifest.json as regular files).
[[nodiscard]] auto is_bundle_available(std::filesystem::path const& dir_path) -> bool;

class workspace_service
{
  public:
    explicit workspace_service(std::filesystem::path config_file = std::filesystem::path {});
    auto create_database(std::string const& dir_path, std::string const& name) -> result<void>;
    auto open_database(std::string const& dir_path) -> result<void>;
    auto activate_scratch() -> result<void>;
    void close_active() noexcept;

    [[nodiscard]] auto kind() const noexcept -> database_kind { return kind_; }

    [[nodiscard]] auto has_active() const noexcept -> bool { return kind_ != database_kind::none; }

    [[nodiscard]] auto is_temporary() const noexcept -> bool { return kind_ == database_kind::scratch; }

    [[nodiscard]] auto display_name() const noexcept -> std::string_view { return display_name_; }

    [[nodiscard]] auto active_path() const noexcept -> std::string_view { return active_path_; }

    [[nodiscard]] auto recent_databases() const noexcept -> std::vector<recent_entry> const& { return config_.recent_databases; }

    // Recent entries in most-recent-first order, each tagged with a fresh
    // on-disk availability check. Entries are never dropped automatically;
    // the user removes them with remove_recent().
    [[nodiscard]] auto recent_database_states() const -> std::vector<recent_database_state>;

    // Forgets one recent entry (matched by path) and persists the per-user
    // config. On any failure the in-memory list is left unchanged.
    auto remove_recent(std::string const& dir_path) -> result<void>;

    [[nodiscard]] auto error_message() const noexcept -> std::string_view { return error_message_; }

    [[nodiscard]] auto notation_below() const noexcept -> bool { return config_.ui_preferences.notation_layout == "below"; }

    auto set_notation_below(bool below) -> result<void>;

    [[nodiscard]] auto preset() const noexcept -> workspace_preset_kind
    {
        return workspace_preset_from_string(config_.ui_preferences.workspace_preset);
    }

    auto set_preset(workspace_preset_kind kind) -> result<void>;

    [[nodiscard]] auto active_database() noexcept -> motif::db::database_manager* { return db_ ? &*db_ : nullptr; }

  private:
    std::optional<motif::db::database_manager> db_;
    workspace_config config_;
    std::filesystem::path config_file_;
    database_kind kind_ {database_kind::none};
    std::string display_name_;
    std::string active_path_;
    std::string error_message_;

    auto persist_config(workspace_config const& config) -> result<void>;
};

}  // namespace motif::slint_app
