#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include "motif/app/app_config.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdlib.h>  // NOLINT(hicpp-deprecated-headers,modernize-deprecated-headers) — POSIX setenv/unsetenv not in <cstdlib>

#include "motif/app/error.hpp"

namespace
{

struct tmp_config_dir
{
    std::filesystem::path root;
    std::filesystem::path config_file;

    explicit tmp_config_dir(std::string const& suffix)
    {
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() / ("motif_app_cfg_test_" + suffix + "_" + std::to_string(tick));
        std::filesystem::create_directories(root / "motif-chess");
        config_file = root / "motif-chess" / "config.json";
        // Save previous value to restore on teardown.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        char const* prev = getenv("XDG_CONFIG_HOME");
        if (prev != nullptr) {
            prev_xdg = prev;
        }
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        setenv("XDG_CONFIG_HOME", root.c_str(), 1);
    }

    ~tmp_config_dir()
    {
        std::filesystem::remove_all(root);
        if (prev_xdg) {
            // NOLINTNEXTLINE(concurrency-mt-unsafe)
            setenv("XDG_CONFIG_HOME", prev_xdg->c_str(), 1);
        } else {
            // NOLINTNEXTLINE(concurrency-mt-unsafe)
            unsetenv("XDG_CONFIG_HOME");
        }
    }

    tmp_config_dir(tmp_config_dir const&) = delete;
    auto operator=(tmp_config_dir const&) -> tmp_config_dir& = delete;
    tmp_config_dir(tmp_config_dir&&) = delete;
    auto operator=(tmp_config_dir&&) -> tmp_config_dir& = delete;

    std::optional<std::string> prev_xdg;
};

}  // namespace

TEST_CASE("app_config: config_path respects XDG_CONFIG_HOME", "[motif-app]")
{
    tmp_config_dir tmp {"xdg"};
    auto const path = motif::app::config_path();
    REQUIRE(path == tmp.config_file);
}

TEST_CASE("app_config: missing config creates defaults", "[motif-app]")
{
    tmp_config_dir const tmp {"defaults"};
    std::filesystem::remove(tmp.config_file);

    auto const res = motif::app::load_config(tmp.config_file);
    REQUIRE(res.has_value());
    REQUIRE(res->recent_databases.empty());
    REQUIRE(res->ui_preferences.prefetch_depth == motif::app::default_prefetch_depth);
    REQUIRE(std::filesystem::exists(tmp.config_file));
}

TEST_CASE("app_config: round-trip serialization", "[motif-app]")
{
    tmp_config_dir const tmp {"roundtrip"};
    motif::app::app_config cfg;
    cfg.database_directory = "/my/db";
    cfg.ui_preferences.board_theme = "dark";
    cfg.ui_preferences.prefetch_depth = 3;

    auto const save_res = motif::app::save_config(cfg, tmp.config_file);
    REQUIRE(save_res.has_value());

    auto const load_res = motif::app::load_config(tmp.config_file);
    REQUIRE(load_res.has_value());
    REQUIRE(load_res->database_directory == "/my/db");
    REQUIRE(load_res->ui_preferences.board_theme == "dark");
    REQUIRE(load_res->ui_preferences.prefetch_depth == 3);
}

TEST_CASE("app_config: malformed config is reported, file untouched", "[motif-app]")
{
    tmp_config_dir const tmp {"malformed"};
    {
        std::ofstream out {tmp.config_file};
        out << "{ this is not valid json";
    }
    auto const read_file = [](std::filesystem::path const& fpath) -> std::string
    {
        std::ifstream ifs {fpath};
        return {std::istreambuf_iterator<char> {ifs}, {}};
    };
    auto const original_content = read_file(tmp.config_file);

    auto const res = motif::app::load_config(tmp.config_file);
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error() == motif::app::error_code::malformed_config);

    // File must be left untouched.
    REQUIRE(read_file(tmp.config_file) == original_content);
}

TEST_CASE("app_config: push_recent deduplicates and prepends", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::push_recent(cfg, "A", "/path/a");
    motif::app::push_recent(cfg, "B", "/path/b");
    motif::app::push_recent(cfg, "A", "/path/a");  // duplicate

    REQUIRE(cfg.recent_databases.size() == 2);
    REQUIRE(cfg.recent_databases[0].path == "/path/a");  // promoted to front
    REQUIRE(cfg.recent_databases[1].path == "/path/b");
}

TEST_CASE("app_config: push_recent caps at 20 entries", "[motif-app]")
{
    motif::app::app_config cfg;
    static constexpr int overflow_count = 25;
    for (int i = 0; i < overflow_count; ++i) {
        motif::app::push_recent(cfg, "db", "/path/" + std::to_string(i));
    }
    REQUIRE(cfg.recent_databases.size() == 20);
}

TEST_CASE("app_config: remove_recent removes matching path", "[motif-app]")
{
    motif::app::app_config cfg;
    motif::app::push_recent(cfg, "A", "/path/a");
    motif::app::push_recent(cfg, "B", "/path/b");
    motif::app::remove_recent(cfg, "/path/a");
    REQUIRE(cfg.recent_databases.size() == 1);
    REQUIRE(cfg.recent_databases[0].path == "/path/b");
}

TEST_CASE("app_config: config is stored outside database bundles", "[motif-app]")
{
    // The config path must be under the user config dir, never inside a db bundle dir.
    tmp_config_dir const tmp {"separation"};
    auto const path = motif::app::config_path();
    // Config path should be under XDG_CONFIG_HOME, not under a database directory.
    REQUIRE(path.string().contains("motif-chess"));
    REQUIRE(path.extension() == ".json");
}
