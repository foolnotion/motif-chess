#include <algorithm>
#include <filesystem>
#include <string>
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

    db_.emplace(std::move(*created));
    kind_ = database_kind::persistent;
    display_name_ = name;
    active_path_ = dir_path;
    promote_recent(name, dir_path);
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
    db_.emplace(std::move(*opened));
    kind_ = database_kind::persistent;
    display_name_ = opened_name;
    active_path_ = dir_path;
    promote_recent(opened_name, dir_path);
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

void workspace_service::promote_recent(std::string const& name, std::string const& path)
{
    std::erase_if(recent_, [&path](recent_entry const& entry) -> bool { return entry.path == path; });
    recent_.insert(recent_.begin(), recent_entry {.name = name, .path = path});
}

}  // namespace motif::slint_app
