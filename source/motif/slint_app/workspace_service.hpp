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

    [[nodiscard]] auto error_message() const noexcept -> std::string_view { return error_message_; }

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
