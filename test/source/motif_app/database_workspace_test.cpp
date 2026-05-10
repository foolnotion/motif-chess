#include <chrono>
#include <filesystem>
#include <string>

#include "motif/app/database_workspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/app/app_config.hpp"

namespace
{

struct tmp_dir
{
    std::filesystem::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("motif_ws_test_" + suffix + "_" + std::to_string(tick));
    }

    ~tmp_dir() { std::filesystem::remove_all(path); }

    tmp_dir(tmp_dir const&) = delete;
    auto operator=(tmp_dir const&) -> tmp_dir& = delete;
    tmp_dir(tmp_dir&&) = delete;
    auto operator=(tmp_dir&&) -> tmp_dir& = delete;
};

}  // namespace

TEST_CASE("database_workspace: initial state is none", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);
    REQUIRE(workspace.kind() == motif::app::database_kind::none);
    REQUIRE_FALSE(workspace.has_active());
    REQUIRE(workspace.persistent_db() == nullptr);
}

TEST_CASE("database_workspace: create_database produces persistent state", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);
    tmp_dir const tmp {"create"};

    auto const res = workspace.create_database(tmp.path.string(), "TestDB");
    REQUIRE(res.has_value());
    REQUIRE(workspace.kind() == motif::app::database_kind::persistent);
    REQUIRE(workspace.has_active());
    REQUIRE(workspace.display_name() == "TestDB");
    REQUIRE(workspace.is_temporary() == false);
    REQUIRE(workspace.persistent_db() != nullptr);
}

TEST_CASE("database_workspace: create_database adds to recent list", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);
    tmp_dir const tmp {"recent"};

    auto const res = workspace.create_database(tmp.path.string(), "RecentDB");
    REQUIRE(res.has_value());
    REQUIRE(cfg.recent_databases.size() == 1);
    REQUIRE(cfg.recent_databases[0].name == "RecentDB");
}

TEST_CASE("database_workspace: open_scratch produces scratch state", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);

    auto const res = workspace.open_scratch();
    REQUIRE(res.has_value());
    REQUIRE(workspace.kind() == motif::app::database_kind::scratch);
    REQUIRE(workspace.has_active());
    REQUIRE(workspace.is_temporary());
    REQUIRE(workspace.persistent_db() == nullptr);
}

TEST_CASE("database_workspace: scratch is never added to recent list", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);

    auto const res = workspace.open_scratch();
    REQUIRE(res.has_value());
    REQUIRE(cfg.recent_databases.empty());
}

TEST_CASE("database_workspace: failed open leaves previous active db unchanged", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);
    tmp_dir const tmp {"failopen"};

    auto const create_res = workspace.create_database(tmp.path.string(), "OriginalDB");
    REQUIRE(create_res.has_value());

    auto const bad_open = workspace.open_database("/nonexistent/path/that/does/not/exist");
    REQUIRE_FALSE(bad_open.has_value());

    // Previous active database must be preserved.
    REQUIRE(workspace.kind() == motif::app::database_kind::persistent);
    REQUIRE(workspace.display_name() == "OriginalDB");
}

TEST_CASE("database_workspace: close_active returns to none state", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);

    auto const res = workspace.open_scratch();
    REQUIRE(res.has_value());
    REQUIRE(workspace.has_active());

    workspace.close_active();
    REQUIRE(workspace.kind() == motif::app::database_kind::none);
    REQUIRE_FALSE(workspace.has_active());
    REQUIRE(workspace.persistent_db() == nullptr);
}

TEST_CASE("database_workspace: remove_recent_entry removes path from config", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::push_recent(cfg, "A", "/path/a");
    motif::app::push_recent(cfg, "B", "/path/b");

    motif::app::database_workspace workspace(&cfg);
    auto const res = workspace.remove_recent_entry("/path/a");
    REQUIRE(res.has_value());

    REQUIRE(cfg.recent_databases.size() == 1);
    REQUIRE(cfg.recent_databases[0].path == "/path/b");
}

TEST_CASE("database_workspace: recent_with_status marks unavailable paths", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::push_recent(cfg, "Missing", "/nonexistent/missing_db");

    motif::app::database_workspace const workspace(&cfg);
    auto const statuses = workspace.recent_with_status();

    REQUIRE(statuses.size() == 1);
    REQUIRE_FALSE(statuses[0].available);
    REQUIRE(statuses[0].entry.path == "/nonexistent/missing_db");
}

TEST_CASE("database_workspace: persistent_db is null for scratch", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);

    auto const res = workspace.open_scratch();
    REQUIRE(res.has_value());
    REQUIRE(workspace.persistent_db() == nullptr);
}

TEST_CASE("database_workspace: open an existing bundle by path", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);
    tmp_dir const tmp {"openbypath"};

    auto const create_res = workspace.create_database(tmp.path.string(), "ExistingDB");
    REQUIRE(create_res.has_value());

    // Close and re-open the same bundle
    workspace.close_active();

    motif::app::app_config cfg2;
    motif::app::database_workspace workspace2(&cfg2);
    auto const open_res = workspace2.open_database(tmp.path.string());
    REQUIRE(open_res.has_value());
    REQUIRE(workspace2.kind() == motif::app::database_kind::persistent);
    REQUIRE(workspace2.display_name() == "ExistingDB");
}
