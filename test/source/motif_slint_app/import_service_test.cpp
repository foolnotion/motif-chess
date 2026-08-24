#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "motif/slint_app/import_service.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/db/database_manager.hpp"

namespace
{

using namespace std::chrono_literals;
constexpr std::size_t long_import_game_count = 500;

struct tmp_dir
{
    std::filesystem::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("motif_slint_import_test_" + suffix + "_" + std::to_string(tick));
        std::filesystem::create_directories(path);
    }

    ~tmp_dir() { std::filesystem::remove_all(path); }

    tmp_dir(tmp_dir const&) = delete;
    auto operator=(tmp_dir const&) -> tmp_dir& = delete;
    tmp_dir(tmp_dir&&) = delete;
    auto operator=(tmp_dir&&) -> tmp_dir& = delete;
};

auto write_games(std::filesystem::path const& path, std::size_t count) -> void
{
    std::ofstream output {path};
    for (std::size_t index = 0; index < count; ++index) {
        output << "[Event \"Test\"]\n"
                  "[Site \"?\"]\n"
                  "[Date \"2024.01.01\"]\n"
                  "[Round \""
               << index + 1 << "\"]\n"
               << "[White \"Player " << index << "\"]\n"
               << "[Black \"Opponent " << index << "\"]\n"
               << "[Result \"1-0\"]\n\n"
                  "1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 1-0\n\n";
    }
}

auto wait_for_terminal(motif::slint_app::import_service const& service) -> motif::slint_app::import_snapshot
{
    auto const deadline = std::chrono::steady_clock::now() + 20s;
    while (service.active() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    return service.snapshot();
}

}  // namespace

TEST_CASE("import_service: rejects start without an active database", "[motif-slint-app]")
{
    motif::slint_app::import_service service;
    auto const result = service.start(nullptr, "games.pgn");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == motif::slint_app::import_error_code::no_database);
    REQUIRE_FALSE(service.active());
    REQUIRE(service.snapshot().state == motif::slint_app::import_state::idle);
}

TEST_CASE("import_service: rejects an empty path", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    motif::slint_app::import_service service;
    auto const result = service.start(&*database, {});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == motif::slint_app::import_error_code::invalid_argument);
}

TEST_CASE("import_service: imports a real PGN and publishes terminal summary", "[motif-slint-app]")
{
    tmp_dir const temporary {"success"};
    auto const pgn_path = temporary.path / "games.pgn";
    write_games(pgn_path, 2);
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    motif::slint_app::import_service service;

    REQUIRE(service.start(&*database, pgn_path).has_value());
    auto const snapshot = wait_for_terminal(service);
    REQUIRE(snapshot.state == motif::slint_app::import_state::completed);
    REQUIRE(snapshot.processed == 2);
    REQUIRE(snapshot.committed == 2);
    REQUIRE(snapshot.errors == 0);
    auto const count = database->store().count_games();
    REQUIRE(count.has_value());
    REQUIRE(*count == 2);

    REQUIRE(service.start(&*database, pgn_path).has_value());
    REQUIRE(wait_for_terminal(service).state == motif::slint_app::import_state::completed);
}

TEST_CASE("import_service: reports asynchronous pipeline failures", "[motif-slint-app]")
{
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    motif::slint_app::import_service service;
    REQUIRE(service.start(&*database, "/nonexistent/path/games.pgn").has_value());
    auto const snapshot = wait_for_terminal(service);
    REQUIRE(snapshot.state == motif::slint_app::import_state::failed);
    REQUIRE_FALSE(snapshot.error_message.empty());
}

TEST_CASE("import_service: rejects reentry and cancellation reaches a terminal state", "[motif-slint-app]")
{
    tmp_dir const temporary {"cancel"};
    auto const pgn_path = temporary.path / "games.pgn";
    write_games(pgn_path, long_import_game_count);
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    motif::slint_app::import_service service;

    REQUIRE(service.start(&*database, pgn_path).has_value());
    auto const second = service.start(&*database, pgn_path);
    REQUIRE_FALSE(second.has_value());
    REQUIRE(second.error() == motif::slint_app::import_error_code::busy);
    service.request_cancel();
    auto const snapshot = wait_for_terminal(service);
    REQUIRE(snapshot.state == motif::slint_app::import_state::canceled);
    REQUIRE_FALSE(service.active());
}

TEST_CASE("import_service: destruction cancels and joins an active import", "[motif-slint-app]")
{
    tmp_dir const temporary {"destruction"};
    auto const pgn_path = temporary.path / "games.pgn";
    write_games(pgn_path, long_import_game_count);
    auto database = motif::db::database_manager::create_scratch();
    REQUIRE(database.has_value());
    {
        motif::slint_app::import_service service;
        REQUIRE(service.start(&*database, pgn_path).has_value());
    }
    SUCCEED();
}
