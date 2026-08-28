#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "motif/slint_app/workspace_service.hpp"

#include <catch2/catch_test_macros.hpp>

namespace
{

struct tmp_dir
{
    std::filesystem::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("motif_slint_app_ws_test_" + suffix + "_" + std::to_string(tick));
    }

    ~tmp_dir() { std::filesystem::remove_all(path); }

    tmp_dir(tmp_dir const&) = delete;
    auto operator=(tmp_dir const&) -> tmp_dir& = delete;
    tmp_dir(tmp_dir&&) = delete;
    auto operator=(tmp_dir&&) -> tmp_dir& = delete;
};

struct tmp_config
{
    std::filesystem::path path;

    explicit tmp_config(std::filesystem::path const& root)
        : path(root / "motif-chess" / "config.json")
    {
    }
};

auto read_file(std::filesystem::path const& path) -> std::string
{
    auto file = std::ifstream {path};
    return {std::istreambuf_iterator<char> {file}, {}};
}

}  // namespace

TEST_CASE("workspace_service: initial state is none", "[motif-slint-app]")
{
    motif::slint_app::workspace_service const service;
    REQUIRE(service.kind() == motif::slint_app::database_kind::none);
    REQUIRE_FALSE(service.has_active());
    REQUIRE_FALSE(service.is_temporary());
    REQUIRE(service.recent_databases().empty());
}

TEST_CASE("workspace_service: active_database follows workspace state", "[motif-slint-app]")
{
    motif::slint_app::workspace_service service;
    REQUIRE(service.active_database() == nullptr);
    REQUIRE(service.activate_scratch().has_value());
    REQUIRE(service.active_database() != nullptr);
    service.close_active();
    REQUIRE(service.active_database() == nullptr);
}

TEST_CASE("workspace_service: create_database produces persistent state", "[motif-slint-app]")
{
    motif::slint_app::workspace_service service;
    tmp_dir const tmp {"create"};
    REQUIRE(service.create_database(tmp.path.string(), "TestDB").has_value());
    REQUIRE(service.kind() == motif::slint_app::database_kind::persistent);
    REQUIRE(service.display_name() == "TestDB");
    REQUIRE(service.active_path() == tmp.path.string());
    REQUIRE_FALSE(service.is_temporary());
}

TEST_CASE("workspace_service: create_database promotes to recent list", "[motif-slint-app]")
{
    motif::slint_app::workspace_service service;
    tmp_dir const tmp {"recent"};
    REQUIRE(service.create_database(tmp.path.string(), "RecentDB").has_value());
    REQUIRE(service.recent_databases().size() == 1);
    REQUIRE(service.recent_databases().front().name == "RecentDB");
    REQUIRE(service.recent_databases().front().path == tmp.path.string());
}

TEST_CASE("workspace_service: open_database opens an existing bundle by path", "[motif-slint-app]")
{
    motif::slint_app::workspace_service creator;
    tmp_dir const tmp {"openbypath"};
    REQUIRE(creator.create_database(tmp.path.string(), "ExistingDB").has_value());
    creator.close_active();
    motif::slint_app::workspace_service opener;
    REQUIRE(opener.open_database(tmp.path.string()).has_value());
    REQUIRE(opener.kind() == motif::slint_app::database_kind::persistent);
    REQUIRE(opener.display_name() == "ExistingDB");
}

TEST_CASE("workspace_service: open_database on a missing bundle fails with database_failure", "[motif-slint-app]")
{
    motif::slint_app::workspace_service service;
    auto const result = service.open_database("/nonexistent/path/that/does/not/exist");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == motif::slint_app::error_code::database_failure);
    REQUIRE_FALSE(service.error_message().empty());
}

TEST_CASE("workspace_service: create_database validates required fields", "[motif-slint-app]")
{
    motif::slint_app::workspace_service service;
    tmp_dir const tmp {"invalid_create"};
    auto const empty_path = service.create_database("", "SomeName");
    REQUIRE_FALSE(empty_path.has_value());
    REQUIRE(empty_path.error() == motif::slint_app::error_code::invalid_argument);
    auto const empty_name = service.create_database(tmp.path.string(), "");
    REQUIRE_FALSE(empty_name.has_value());
    REQUIRE(empty_name.error() == motif::slint_app::error_code::invalid_argument);
    REQUIRE_FALSE(service.has_active());
    REQUIRE(service.recent_databases().empty());
}

TEST_CASE("workspace_service: open_database validates required path", "[motif-slint-app]")
{
    motif::slint_app::workspace_service service;
    auto const result = service.open_database("");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == motif::slint_app::error_code::invalid_argument);
    REQUIRE_FALSE(service.has_active());
    REQUIRE(service.recent_databases().empty());
}

TEST_CASE("workspace_service: successful operation clears prior error", "[motif-slint-app]")
{
    motif::slint_app::workspace_service service;
    tmp_dir const tmp {"clear_error"};
    REQUIRE_FALSE(service.open_database("/nonexistent/path/that/does/not/exist").has_value());
    REQUIRE_FALSE(service.error_message().empty());
    REQUIRE(service.create_database(tmp.path.string(), "AfterError").has_value());
    REQUIRE(service.error_message().empty());
}

TEST_CASE("workspace_service: failed operations preserve active database", "[motif-slint-app]")
{
    motif::slint_app::workspace_service service;
    tmp_dir const tmp {"preserve"};
    REQUIRE(service.create_database(tmp.path.string(), "OriginalDB").has_value());
    REQUIRE_FALSE(service.open_database("/nonexistent/path/that/does/not/exist").has_value());
    REQUIRE(service.display_name() == "OriginalDB");
    REQUIRE(service.active_path() == tmp.path.string());
    REQUIRE_FALSE(service.create_database(tmp.path.string(), "Other").has_value());
    REQUIRE(service.display_name() == "OriginalDB");
}

TEST_CASE("workspace_service: scratch state is temporary and excluded from recents", "[motif-slint-app]")
{
    motif::slint_app::workspace_service service;
    REQUIRE(service.activate_scratch().has_value());
    REQUIRE(service.kind() == motif::slint_app::database_kind::scratch);
    REQUIRE(service.has_active());
    REQUIRE(service.is_temporary());
    REQUIRE(service.active_path().empty());
    REQUIRE(service.recent_databases().empty());
}

TEST_CASE("workspace_service: close_active clears active state and errors", "[motif-slint-app]")
{
    motif::slint_app::workspace_service service;
    REQUIRE_FALSE(service.open_database("/nonexistent/path/that/does/not/exist").has_value());
    service.close_active();
    REQUIRE(service.kind() == motif::slint_app::database_kind::none);
    REQUIRE_FALSE(service.has_active());
    REQUIRE(service.display_name().empty());
    REQUIRE(service.active_path().empty());
    REQUIRE(service.error_message().empty());
    service.close_active();
}

TEST_CASE("workspace_service: recent list orders newest first without duplicates", "[motif-slint-app]")
{
    motif::slint_app::workspace_service service;
    tmp_dir const tmp_a {"order_a"};
    tmp_dir const tmp_b {"order_b"};
    REQUIRE(service.create_database(tmp_a.path.string(), "A").has_value());
    service.close_active();
    REQUIRE(service.create_database(tmp_b.path.string(), "B").has_value());
    service.close_active();
    REQUIRE(service.open_database(tmp_a.path.string()).has_value());
    auto const& recent = service.recent_databases();
    REQUIRE(recent.size() == 2);
    REQUIRE(recent[0].path == tmp_a.path.string());
    REQUIRE(recent[1].path == tmp_b.path.string());
}

TEST_CASE("workspace_service: recent databases persist across service instances", "[motif-slint-app]")
{
    tmp_dir const tmp {"persist_recents"};
    auto const config = tmp_config {tmp.path};

    {
        motif::slint_app::workspace_service service {config.path};
        REQUIRE(service.create_database(tmp.path.string(), "PersistentDB").has_value());
    }

    motif::slint_app::workspace_service const restored {config.path};
    REQUIRE(restored.recent_databases().size() == 1);
    REQUIRE(restored.recent_databases().front().name == "PersistentDB");
    REQUIRE(restored.recent_databases().front().path == tmp.path.string());
}

TEST_CASE("workspace_config: serializes and restores preferences", "[motif-slint-app]")
{
    tmp_dir const tmp {"config_round_trip"};
    auto const config_path = tmp_config {tmp.path}.path;
    auto config = motif::slint_app::workspace_config {};
    config.database_directory = "/my/db";
    config.ui_preferences.board_theme = "dark";
    config.ui_preferences.prefetch_depth = 3;

    REQUIRE(motif::slint_app::save_config(config, config_path).has_value());
    auto const restored = motif::slint_app::load_config(config_path);
    REQUIRE(restored.has_value());
    REQUIRE(restored->database_directory == "/my/db");
    REQUIRE(restored->ui_preferences.board_theme == "dark");
    REQUIRE(restored->ui_preferences.prefetch_depth == 3);
}

TEST_CASE("workspace_config: malformed input remains untouched", "[motif-slint-app]")
{
    tmp_dir const tmp {"config_malformed"};
    auto const config_path = tmp_config {tmp.path}.path;
    std::filesystem::create_directories(config_path.parent_path());
    {
        auto file = std::ofstream {config_path};
        file << "{ this is not valid json";
    }
    auto const original = read_file(config_path);

    auto const result = motif::slint_app::load_config(config_path);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == motif::slint_app::config_error_code::malformed_config);
    REQUIRE(read_file(config_path) == original);
}

TEST_CASE("workspace_service: failed persistence preserves active workspace and recents", "[motif-slint-app]")
{
    tmp_dir const tmp {"persistence_failure"};
    std::filesystem::create_directories(tmp.path);
    auto const config_path = tmp.path / "not-a-directory" / "config.json";
    {
        auto file = std::ofstream {tmp.path / "not-a-directory"};
        file << "blocking file";
    }
    motif::slint_app::workspace_service service {config_path};
    auto const database_path = tmp.path / "database";

    auto const result = service.create_database(database_path.string(), "PersistentDB");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == motif::slint_app::error_code::database_failure);
    REQUIRE_FALSE(service.has_active());
    REQUIRE(service.recent_databases().empty());
}

TEST_CASE("workspace_config: write leaves no temporary file", "[motif-slint-app]")
{
    tmp_dir const tmp {"config_atomic_write"};
    auto const config_path = tmp_config {tmp.path}.path;
    auto const config = motif::slint_app::workspace_config {};

    REQUIRE(motif::slint_app::save_config(config, config_path).has_value());
    REQUIRE(std::filesystem::exists(config_path));
    for (auto const& entry : std::filesystem::directory_iterator {config_path.parent_path()}) {
        REQUIRE_FALSE(entry.path().filename().string().starts_with("config.json.tmp-"));
    }
}
