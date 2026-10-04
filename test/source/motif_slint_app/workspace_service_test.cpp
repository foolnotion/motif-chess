#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

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

TEST_CASE("workspace_config: persists notation layout preference", "[motif-slint-app]")
{
    tmp_dir const tmp {"notation_layout"};
    auto const config_path = tmp_config {tmp.path}.path;
    auto service = motif::slint_app::workspace_service {config_path};

    REQUIRE_FALSE(service.notation_below());
    REQUIRE(service.set_notation_below(true).has_value());

    motif::slint_app::workspace_service const restored {config_path};
    REQUIRE(restored.notation_below());
}

TEST_CASE("workspace_config: persists the workspace preset and defaults to review", "[motif-slint-app]")
{
    using motif::slint_app::workspace_preset_kind;
    tmp_dir const tmp {"workspace_preset"};
    auto const config_path = tmp_config {tmp.path}.path;
    auto service = motif::slint_app::workspace_service {config_path};

    REQUIRE(service.preset() == workspace_preset_kind::review);
    for (auto const kind : {workspace_preset_kind::explorer, workspace_preset_kind::analysis, workspace_preset_kind::review}) {
        REQUIRE(service.set_preset(kind).has_value());
        REQUIRE(service.preset() == kind);
        motif::slint_app::workspace_service const restored {config_path};
        REQUIRE(restored.preset() == kind);
    }
}

TEST_CASE("workspace_config: a config without a preset, or with an unknown one, restores review", "[motif-slint-app]")
{
    using motif::slint_app::workspace_preset_kind;
    tmp_dir const tmp {"workspace_preset_unknown"};
    auto const config_path = tmp_config {tmp.path}.path;
    std::filesystem::create_directories(config_path.parent_path());
    {
        // Written before presets existed: no workspace_preset key.
        auto file = std::ofstream {config_path};
        file << R"({"database_directory":"","recent_databases":[],"engine_paths":[],"ui_preferences":{"notation_layout":"below"}})";
    }
    auto const legacy = motif::slint_app::workspace_service {config_path};
    REQUIRE(legacy.preset() == workspace_preset_kind::review);
    REQUIRE(legacy.notation_below());

    {
        auto file = std::ofstream {config_path};
        file << R"({"ui_preferences":{"workspace_preset":"dock-everything"}})";
    }
    motif::slint_app::workspace_service const unknown {config_path};
    REQUIRE(unknown.preset() == workspace_preset_kind::review);
}

TEST_CASE("workspace_service: a preset that cannot be saved is not adopted", "[motif-slint-app]")
{
    using motif::slint_app::workspace_preset_kind;
    tmp_dir const tmp {"workspace_preset_save_failure"};
    std::filesystem::create_directories(tmp.path);
    {
        auto file = std::ofstream {tmp.path / "not-a-directory"};
        file << "blocking file";
    }
    motif::slint_app::workspace_service service {tmp.path / "not-a-directory" / "config.json"};

    REQUIRE(service.preset() == workspace_preset_kind::review);
    auto const result = service.set_preset(workspace_preset_kind::analysis);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(service.preset() == workspace_preset_kind::review);
}

namespace
{

// Makes every later save of `config_path` fail: its directory becomes a file.
void break_config_saves(std::filesystem::path const& config_path)
{
    std::filesystem::remove_all(config_path.parent_path());
    auto file = std::ofstream {config_path.parent_path()};
    file << "blocking file";
}

}  // namespace

TEST_CASE("workspace_service: engines default to none with MultiPV 3 and depth 20", "[motif-slint-app]")
{
    motif::slint_app::workspace_service const service;

    auto const engines = service.engines();
    CHECK(engines.paths.empty());
    CHECK(engines.selected.empty());
    CHECK(engines.multipv == 3);
    CHECK(engines.limit == motif::slint_app::analysis_limit_kind::depth);
    CHECK(engines.depth == 20);
    CHECK(engines.movetime_ms == 1000);
}

TEST_CASE("workspace_service: add_engine appends, selects and persists the path", "[motif-slint-app]")
{
    tmp_dir const tmp {"engine_add"};
    auto const config_path = tmp_config {tmp.path}.path;
    auto service = motif::slint_app::workspace_service {config_path};

    REQUIRE(service.add_engine("/opt/engines/stockfish").has_value());
    REQUIRE(service.add_engine("  /opt/engines/lc0\t\n").has_value());

    auto const engines = service.engines();
    CHECK(engines.paths == std::vector<std::string> {"/opt/engines/stockfish", "/opt/engines/lc0"});
    CHECK(engines.selected == "/opt/engines/lc0");
    auto const restored = motif::slint_app::workspace_service {config_path}.engines();
    CHECK(restored.paths == engines.paths);
    CHECK(restored.selected == "/opt/engines/lc0");
}

TEST_CASE("workspace_service: add_engine ignores an empty or duplicate path and says why", "[motif-slint-app]")
{
    tmp_dir const tmp {"engine_add_rejected"};
    auto service = motif::slint_app::workspace_service {tmp_config {tmp.path}.path};
    REQUIRE(service.add_engine("/opt/engines/stockfish").has_value());
    REQUIRE(service.add_engine("/opt/engines/lc0").has_value());

    auto const empty = service.add_engine("   ");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == motif::slint_app::error_code::invalid_argument);
    CHECK_FALSE(service.error_message().empty());

    auto const duplicate = service.add_engine(" /opt/engines/stockfish ");
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error() == motif::slint_app::error_code::invalid_argument);
    CHECK(service.error_message().find("already") != std::string_view::npos);

    auto const engines = service.engines();
    CHECK(engines.paths.size() == 2);
    // A rejected path does not steal the selection.
    CHECK(engines.selected == "/opt/engines/lc0");
}

TEST_CASE("workspace_service: remove_engine moves the selection to the first remaining engine", "[motif-slint-app]")
{
    tmp_dir const tmp {"engine_remove"};
    auto const config_path = tmp_config {tmp.path}.path;
    auto service = motif::slint_app::workspace_service {config_path};
    for (auto const* path : {"/e/one", "/e/two", "/e/three"}) {
        REQUIRE(service.add_engine(path).has_value());
    }
    REQUIRE(service.select_engine("/e/two").has_value());

    REQUIRE(service.remove_engine("/e/two").has_value());
    CHECK(service.engines().paths == std::vector<std::string> {"/e/one", "/e/three"});
    CHECK(service.engines().selected == "/e/one");

    // Removing an engine that is not selected keeps the selection.
    REQUIRE(service.select_engine("/e/three").has_value());
    REQUIRE(service.remove_engine("/e/one").has_value());
    CHECK(service.engines().selected == "/e/three");

    REQUIRE(service.remove_engine("/e/three").has_value());
    CHECK(service.engines().paths.empty());
    CHECK(service.engines().selected.empty());
    CHECK(motif::slint_app::workspace_service {config_path}.engines().paths.empty());

    auto const missing = service.remove_engine("/e/three");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == motif::slint_app::error_code::invalid_argument);
}

TEST_CASE("workspace_service: a stored selection that is no longer listed falls back to the first engine", "[motif-slint-app]")
{
    tmp_dir const tmp {"engine_selection_fallback"};
    auto const config_path = tmp_config {tmp.path}.path;
    std::filesystem::create_directories(config_path.parent_path());
    {
        auto file = std::ofstream {config_path};
        file << R"({"engine_paths":["/e/one","/e/two"],"ui_preferences":{"engine_selected":"/e/gone"}})";
    }

    auto const service = motif::slint_app::workspace_service {config_path};

    CHECK(service.engines().selected == "/e/one");
}

TEST_CASE("workspace_service: select_engine rejects an engine that is not listed", "[motif-slint-app]")
{
    tmp_dir const tmp {"engine_select_unknown"};
    auto service = motif::slint_app::workspace_service {tmp_config {tmp.path}.path};
    REQUIRE(service.add_engine("/e/one").has_value());

    auto const result = service.select_engine("/e/two");

    REQUIRE_FALSE(result.has_value());
    CHECK(service.engines().selected == "/e/one");
}

TEST_CASE("workspace_service: engine options persist, and out-of-range values are clamped", "[motif-slint-app]")
{
    using motif::slint_app::analysis_limit_kind;
    tmp_dir const tmp {"engine_options"};
    auto const config_path = tmp_config {tmp.path}.path;
    auto service = motif::slint_app::workspace_service {config_path};

    REQUIRE(service.set_engine_options(4, analysis_limit_kind::movetime, 30, 5000).has_value());
    auto const restored = motif::slint_app::workspace_service {config_path}.engines();
    CHECK(restored.multipv == 4);
    CHECK(restored.limit == analysis_limit_kind::movetime);
    CHECK(restored.depth == 30);
    CHECK(restored.movetime_ms == 5000);

    REQUIRE(service.set_engine_options(0, analysis_limit_kind::depth, 500, 5).has_value());
    auto const low = service.engines();
    CHECK(low.multipv == 1);
    CHECK(low.depth == 99);
    CHECK(low.movetime_ms == 100);

    REQUIRE(service.set_engine_options(50, analysis_limit_kind::movetime, 0, 9'000'000).has_value());
    auto const high = service.engines();
    CHECK(high.multipv == 5);
    CHECK(high.depth == 1);
    CHECK(high.movetime_ms == 600'000);
}

TEST_CASE("workspace_service: hand-edited engine options outside their ranges load clamped", "[motif-slint-app]")
{
    tmp_dir const tmp {"engine_options_hand_edited"};
    auto const config_path = tmp_config {tmp.path}.path;
    std::filesystem::create_directories(config_path.parent_path());
    {
        auto file = std::ofstream {config_path};
        file << R"({"ui_preferences":{"engine_multipv":42,"engine_limit":"forever","engine_depth":-3,"engine_movetime_ms":1}})";
    }

    auto const engines = motif::slint_app::workspace_service {config_path}.engines();

    CHECK(engines.multipv == 5);
    CHECK(engines.limit == motif::slint_app::analysis_limit_kind::depth);
    CHECK(engines.depth == 1);
    CHECK(engines.movetime_ms == 100);
}

TEST_CASE("workspace_service: engine changes that cannot be saved are rolled back", "[motif-slint-app]")
{
    using motif::slint_app::analysis_limit_kind;
    tmp_dir const tmp {"engine_save_failure"};
    auto const config_path = tmp_config {tmp.path}.path;
    auto service = motif::slint_app::workspace_service {config_path};
    REQUIRE(service.add_engine("/e/one").has_value());
    REQUIRE(service.add_engine("/e/two").has_value());
    break_config_saves(config_path);

    CHECK_FALSE(service.add_engine("/e/three").has_value());
    CHECK_FALSE(service.remove_engine("/e/two").has_value());
    CHECK_FALSE(service.select_engine("/e/one").has_value());
    CHECK_FALSE(service.set_engine_options(5, analysis_limit_kind::movetime, 10, 2000).has_value());

    auto const engines = service.engines();
    CHECK(engines.paths == std::vector<std::string> {"/e/one", "/e/two"});
    CHECK(engines.selected == "/e/two");
    CHECK(engines.multipv == 3);
    CHECK(engines.limit == analysis_limit_kind::depth);
    CHECK_FALSE(service.error_message().empty());
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

TEST_CASE("workspace_service: failed persistence keeps a pre-existing directory and its files", "[motif-slint-app]")
{
    tmp_dir const tmp {"persistence_failure_existing"};
    auto const database_path = tmp.path / "user-folder";
    std::filesystem::create_directories(database_path / "documents");
    auto const sentinel = database_path / "important.txt";
    auto const nested_sentinel = database_path / "documents" / "notes.txt";
    {
        auto file = std::ofstream {sentinel};
        file << "irreplaceable";
    }
    {
        auto file = std::ofstream {nested_sentinel};
        file << "notes";
    }
    // A regular file where the config's parent directory must be forces the config write to fail.
    auto const config_path = tmp.path / "not-a-directory" / "config.json";
    {
        auto file = std::ofstream {tmp.path / "not-a-directory"};
        file << "blocking file";
    }
    motif::slint_app::workspace_service service {config_path};

    auto const result = service.create_database(database_path.string(), "PersistentDB");

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == motif::slint_app::error_code::database_failure);
    REQUIRE_FALSE(service.has_active());
    REQUIRE(service.recent_databases().empty());
    REQUIRE(std::filesystem::exists(database_path));
    REQUIRE(read_file(sentinel) == "irreplaceable");
    REQUIRE(read_file(nested_sentinel) == "notes");
    REQUIRE_FALSE(std::filesystem::exists(database_path / "games.db"));
    REQUIRE_FALSE(std::filesystem::exists(database_path / "manifest.json"));
    REQUIRE_FALSE(std::filesystem::exists(database_path / ".motif.lock"));
}

TEST_CASE("workspace_service: failed persistence removes a directory that create made", "[motif-slint-app]")
{
    tmp_dir const tmp {"persistence_failure_fresh"};
    std::filesystem::create_directories(tmp.path);
    auto const config_path = tmp.path / "not-a-directory" / "config.json";
    {
        auto file = std::ofstream {tmp.path / "not-a-directory"};
        file << "blocking file";
    }
    motif::slint_app::workspace_service service {config_path};
    auto const database_path = tmp.path / "fresh-database";

    REQUIRE_FALSE(service.create_database(database_path.string(), "PersistentDB").has_value());

    REQUIRE_FALSE(std::filesystem::exists(database_path));
    REQUIRE(std::filesystem::exists(tmp.path / "not-a-directory"));
}

TEST_CASE("workspace_service: recent states flag bundles that are no longer on disk", "[motif-slint-app]")
{
    tmp_dir const present {"recent_present"};
    tmp_dir const missing {"recent_missing"};
    auto const config_path = tmp_config {present.path}.path;
    {
        motif::slint_app::workspace_service creator {config_path};
        REQUIRE(creator.create_database(missing.path.string(), "Missing").has_value());
        creator.close_active();
        REQUIRE(creator.create_database(present.path.string(), "Present").has_value());
    }
    std::filesystem::remove_all(missing.path);
    // A directory without the bundle files is also unavailable.
    tmp_dir const empty_dir {"recent_empty_dir"};
    std::filesystem::create_directories(empty_dir.path);

    motif::slint_app::workspace_service service {config_path};
    auto config = motif::slint_app::workspace_config {};
    config.recent_databases = service.recent_databases();
    motif::slint_app::push_recent(config, "EmptyDir", empty_dir.path.string());
    REQUIRE(motif::slint_app::save_config(config, config_path).has_value());
    motif::slint_app::workspace_service const reloaded {config_path};

    auto const states = reloaded.recent_database_states();
    REQUIRE(states.size() == 3);
    REQUIRE(states[0].name == "EmptyDir");
    REQUIRE_FALSE(states[0].available);
    REQUIRE(states[1].name == "Present");
    REQUIRE(states[1].available);
    REQUIRE(states[2].name == "Missing");
    REQUIRE_FALSE(states[2].available);
}

TEST_CASE("workspace_service: remove_recent forgets an entry and persists it", "[motif-slint-app]")
{
    tmp_dir const tmp_a {"remove_a"};
    tmp_dir const tmp_b {"remove_b"};
    auto const config_path = tmp_config {tmp_a.path}.path;
    {
        motif::slint_app::workspace_service service {config_path};
        REQUIRE(service.create_database(tmp_a.path.string(), "A").has_value());
        service.close_active();
        REQUIRE(service.create_database(tmp_b.path.string(), "B").has_value());
        service.close_active();
        REQUIRE(service.remove_recent(tmp_a.path.string()).has_value());
        REQUIRE(service.recent_databases().size() == 1);
        REQUIRE(service.recent_databases().front().path == tmp_b.path.string());
    }

    motif::slint_app::workspace_service const restored {config_path};
    REQUIRE(restored.recent_databases().size() == 1);
    REQUIRE(restored.recent_databases().front().name == "B");
    // Forgetting a recent entry never deletes the bundle itself.
    REQUIRE(std::filesystem::exists(tmp_a.path / "games.db"));
}

TEST_CASE("workspace_service: remove_recent rejects unknown paths and keeps the list on persistence failure", "[motif-slint-app]")
{
    tmp_dir const tmp {"remove_failure"};
    std::filesystem::create_directories(tmp.path);
    auto const config_path = tmp.path / "config" / "config.json";
    motif::slint_app::workspace_service service {config_path};
    REQUIRE(service.create_database((tmp.path / "bundle").string(), "Bundle").has_value());

    auto const unknown = service.remove_recent("/not/in/the/list");
    REQUIRE_FALSE(unknown.has_value());
    REQUIRE(unknown.error() == motif::slint_app::error_code::invalid_argument);
    REQUIRE(service.recent_databases().size() == 1);

    // Replace the config directory with a regular file so the next write fails.
    std::filesystem::remove_all(tmp.path / "config");
    {
        auto file = std::ofstream {tmp.path / "config"};
        file << "blocking file";
    }
    auto const failed = service.remove_recent((tmp.path / "bundle").string());
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error() == motif::slint_app::error_code::database_failure);
    REQUIRE(service.recent_databases().size() == 1);
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
