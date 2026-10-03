#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "motif/slint_app/workspace_service.hpp"

#include <tl/expected.hpp>

#include "motif/db/error.hpp"

namespace motif::slint_app
{

namespace
{

auto to_service_message(motif::db::error const& database_error) -> std::string
{
    return database_error.message.empty() ? std::string {motif::db::to_string(database_error)} : database_error.message;
}

}  // namespace

auto is_bundle_available(std::filesystem::path const& dir_path) -> bool
{
    if (dir_path.empty()) {
        return false;
    }
    std::error_code fs_error;
    return std::filesystem::is_regular_file(dir_path / "games.db", fs_error)
        && std::filesystem::is_regular_file(dir_path / "manifest.json", fs_error);
}

workspace_service::workspace_service(std::filesystem::path config_file)
    : config_file_ {std::move(config_file)}
{
    if (config_file_.empty()) {
        return;
    }

    auto config = load_config(config_file_);
    if (config) {
        config_ = std::move(*config);
    } else {
        error_message_ = config.error().message;
    }
}

auto workspace_service::create_database(std::string const& dir_path, std::string const& name) -> result<void>
{
    if (dir_path.empty() || name.empty()) {
        error_message_ = "Directory path and name are required.";
        return tl::unexpected {error_code::invalid_argument};
    }

    auto created = motif::db::database_manager::create(std::filesystem::path {dir_path}, name);
    if (!created) {
        error_message_ = to_service_message(created.error());
        return tl::unexpected {error_code::database_failure};
    }

    auto next_config = config_;
    push_recent(next_config, name, dir_path);
    if (auto persisted = persist_config(next_config); !persisted) {
        // Roll back only what create() produced; a pre-existing directory and
        // unrelated files inside it must survive a failed config write.
        created->discard_created_bundle();
        return persisted;
    }

    db_.emplace(std::move(*created));
    config_ = std::move(next_config);
    kind_ = database_kind::persistent;
    display_name_ = name;
    active_path_ = dir_path;
    error_message_.clear();
    return {};
}

auto workspace_service::recent_database_states() const -> std::vector<recent_database_state>
{
    auto states = std::vector<recent_database_state> {};
    states.reserve(config_.recent_databases.size());
    for (auto const& entry : config_.recent_databases) {
        states.push_back({.name = entry.name, .path = entry.path, .available = is_bundle_available(std::filesystem::path {entry.path})});
    }
    return states;
}

auto workspace_service::remove_recent(std::string const& dir_path) -> result<void>
{
    auto next_config = config_;
    auto const removed =
        std::erase_if(next_config.recent_databases, [&dir_path](recent_entry const& entry) -> bool { return entry.path == dir_path; });
    if (removed == 0) {
        error_message_ = "That database is not in the recent list.";
        return tl::unexpected {error_code::invalid_argument};
    }
    if (auto persisted = persist_config(next_config); !persisted) {
        return persisted;
    }
    config_ = std::move(next_config);
    error_message_.clear();
    return {};
}

auto workspace_service::open_database(std::string const& dir_path) -> result<void>
{
    if (dir_path.empty()) {
        error_message_ = "Directory path is required.";
        return tl::unexpected {error_code::invalid_argument};
    }

    auto opened = motif::db::database_manager::open(std::filesystem::path {dir_path});
    if (!opened) {
        error_message_ = to_service_message(opened.error());
        return tl::unexpected {error_code::database_failure};
    }

    auto opened_name = std::string {opened->manifest().name};
    auto next_config = config_;
    push_recent(next_config, opened_name, dir_path);
    if (auto persisted = persist_config(next_config); !persisted) {
        return persisted;
    }

    db_.emplace(std::move(*opened));
    config_ = std::move(next_config);
    kind_ = database_kind::persistent;
    display_name_ = opened_name;
    active_path_ = dir_path;
    error_message_.clear();
    return {};
}

auto workspace_service::activate_scratch() -> result<void>
{
    auto scratch = motif::db::database_manager::create_scratch();
    if (!scratch) {
        error_message_ = to_service_message(scratch.error());
        return tl::unexpected {error_code::database_failure};
    }

    db_.emplace(std::move(*scratch));
    kind_ = database_kind::scratch;
    display_name_ = "Scratch";
    active_path_.clear();
    error_message_.clear();
    return {};
}

void workspace_service::close_active() noexcept
{
    if (db_) {
        db_->close();
        db_.reset();
    }
    kind_ = database_kind::none;
    display_name_.clear();
    active_path_.clear();
    error_message_.clear();
}

auto workspace_service::set_notation_below(bool const below) -> result<void>
{
    auto next_config = config_;
    next_config.ui_preferences.notation_layout = below ? "below" : "right";
    if (auto persisted = persist_config(next_config); !persisted) {
        return persisted;
    }
    config_ = std::move(next_config);
    return {};
}

auto workspace_service::set_preset(workspace_preset_kind const kind) -> result<void>
{
    auto next_config = config_;
    next_config.ui_preferences.workspace_preset = std::string {to_string(kind)};
    if (auto persisted = persist_config(next_config); !persisted) {
        return persisted;
    }
    config_ = std::move(next_config);
    return {};
}

auto workspace_service::persist_config(workspace_config const& config) -> result<void>
{
    if (config_file_.empty()) {
        return {};
    }

    auto const saved = save_config(config, config_file_);
    if (!saved) {
        error_message_ = saved.error().message;
        return tl::unexpected {error_code::database_failure};
    }
    return {};
}

}  // namespace motif::slint_app
