#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "motif/slint_app/pgn_launch_queue.hpp"

#include <catch2/catch_test_macros.hpp>

#include "motif/slint_app/import_service.hpp"
#include "motif/slint_app/workspace_service.hpp"

namespace
{

struct tmp_dir
{
    std::filesystem::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("motif_slint_app_launch_test_" + suffix + "_" + std::to_string(tick));
        std::filesystem::create_directories(path);
    }

    ~tmp_dir() { std::filesystem::remove_all(path); }

    tmp_dir(tmp_dir const&) = delete;
    auto operator=(tmp_dir const&) -> tmp_dir& = delete;
    tmp_dir(tmp_dir&&) = delete;
    auto operator=(tmp_dir&&) -> tmp_dir& = delete;
};

auto touch(std::filesystem::path const& path) -> std::filesystem::path
{
    auto file = std::ofstream {path};
    file << "[Event \"x\"]\n";
    return path;
}

auto write_games(std::filesystem::path const& path, int const count, std::string const& player) -> std::filesystem::path
{
    auto file = std::ofstream {path};
    for (auto index = 0; index < count; ++index) {
        file << "[Event \"Launch\"]\n[Site \"?\"]\n[Date \"2024.01.01\"]\n[Round \"" << index + 1 << "\"]\n[White \"" << player << index
             << "\"]\n[Black \"Opponent\"]\n[Result \"1-0\"]\n\n1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 1-0\n\n";
    }
    return path;
}

auto wait_until_idle(motif::slint_app::import_service const& importer) -> motif::slint_app::import_snapshot
{
    constexpr auto poll_interval = std::chrono::milliseconds {10};
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds {20};
    while (importer.active() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(poll_interval);
    }
    return importer.snapshot();
}

}  // namespace

TEST_CASE("pgn_launch_queue: no arguments yields an empty queue", "[motif-slint-app][launch]")
{
    auto const queue = motif::slint_app::pgn_launch_queue::from_arguments({});
    REQUIRE(queue.empty());
    REQUIRE(queue.rejected().empty());
    REQUIRE(queue.rejection_message().empty());
}

TEST_CASE("pgn_launch_queue: existing .pgn files queue in order, case-insensitively", "[motif-slint-app][launch]")
{
    tmp_dir const tmp {"valid"};
    auto const first = touch(tmp.path / "first.pgn");
    auto const second = touch(tmp.path / "SECOND.PGN");
    auto const arguments = std::vector<std::string> {first.string(), second.string()};

    auto queue = motif::slint_app::pgn_launch_queue::from_arguments(arguments);

    REQUIRE(queue.pending() == 2);
    REQUIRE(queue.rejected().empty());
    REQUIRE(queue.take_next() == first);
    REQUIRE(queue.take_next() == second);
    REQUIRE_FALSE(queue.take_next().has_value());
    REQUIRE(queue.empty());
}

TEST_CASE("pgn_launch_queue: relative paths are resolved against the working directory", "[motif-slint-app][launch]")
{
    tmp_dir const tmp {"relative"};
    touch(tmp.path / "rel.pgn");
    auto const previous = std::filesystem::current_path();
    std::filesystem::current_path(tmp.path);
    auto const arguments = std::vector<std::string> {"rel.pgn"};
    auto queue = motif::slint_app::pgn_launch_queue::from_arguments(arguments);
    std::filesystem::current_path(previous);

    REQUIRE(queue.pending() == 1);
    auto const resolved = queue.take_next();
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->is_absolute());  // NOLINT(bugprone-unchecked-optional-access)
    REQUIRE(resolved->filename() == "rel.pgn");  // NOLINT(bugprone-unchecked-optional-access)
}

TEST_CASE("pgn_launch_queue: a valid document received after startup queues for import", "[motif-slint-app][launch]")
{
    tmp_dir const tmp {"late"};
    auto const pgn = touch(tmp.path / "from_finder.pgn");
    auto queue = motif::slint_app::pgn_launch_queue {};

    queue.enqueue(pgn.string());

    REQUIRE(queue.pending() == 1);
    REQUIRE(queue.rejected().empty());
    REQUIRE(queue.take_next() == pgn);
}

TEST_CASE("pgn_launch_queue: invalid arguments are reported, never silently dropped", "[motif-slint-app][launch]")
{
    tmp_dir const tmp {"invalid"};
    auto const valid = touch(tmp.path / "good.pgn");
    auto const wrong_extension = touch(tmp.path / "notes.txt");
    auto const directory = tmp.path / "folder.pgn";
    std::filesystem::create_directories(directory);
    auto const missing = tmp.path / "missing.pgn";
    auto const arguments =
        std::vector<std::string> {wrong_extension.string(), missing.string(), directory.string(), "", "--unknown-flag", valid.string()};

    auto queue = motif::slint_app::pgn_launch_queue::from_arguments(arguments);

    REQUIRE(queue.pending() == 1);
    REQUIRE(queue.take_next() == valid);
    auto const& rejected = queue.rejected();
    REQUIRE(rejected.size() == 5);
    REQUIRE(rejected[0].argument == wrong_extension.string());
    REQUIRE(rejected[0].reason == "not a .pgn file");
    REQUIRE(rejected[1].argument == missing.string());
    REQUIRE(rejected[1].reason == "path does not exist");
    REQUIRE(rejected[2].argument == directory.string());
    REQUIRE(rejected[2].reason == "not a regular file");
    REQUIRE(rejected[3].reason == "empty path");
    REQUIRE(rejected[4].argument == "--unknown-flag");
    REQUIRE(rejected[4].reason == "not a .pgn file");

    auto const message = queue.rejection_message();
    REQUIRE(message.starts_with("Skipped 5 launch arguments: "));
    REQUIRE(message.contains(missing.string() + "' (path does not exist)"));
}

TEST_CASE("pgn_launch_queue: a single rejection uses the singular message", "[motif-slint-app][launch]")
{
    auto const arguments = std::vector<std::string> {"/definitely/not/here.pgn"};
    auto const queue = motif::slint_app::pgn_launch_queue::from_arguments(arguments);
    REQUIRE(queue.empty());
    REQUIRE(queue.rejection_message() == "Skipped 1 launch argument: '/definitely/not/here.pgn' (path does not exist)");
}

TEST_CASE("pgn_launch_queue: clear drops pending paths but keeps rejection history", "[motif-slint-app][launch]")
{
    tmp_dir const tmp {"clear"};
    auto const first = touch(tmp.path / "a.pgn");
    auto const second = touch(tmp.path / "b.pgn");
    auto const arguments = std::vector<std::string> {first.string(), second.string(), "bad.txt"};
    auto queue = motif::slint_app::pgn_launch_queue::from_arguments(arguments);

    REQUIRE(queue.clear() == 2);
    REQUIRE(queue.empty());
    REQUIRE_FALSE(queue.take_next().has_value());
    REQUIRE(queue.rejected().size() == 1);
    REQUIRE(queue.clear() == 0);
}

// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST_CASE("pgn_launch_queue: queued paths wait for a database, then import through import_service", "[motif-slint-app][launch]")
{
    tmp_dir const tmp {"import_flow"};
    auto const first = write_games(tmp.path / "first.pgn", 3, "First");
    auto const second = write_games(tmp.path / "second.pgn", 2, "Second");
    auto const arguments = std::vector<std::string> {first.string(), second.string()};
    auto queue = motif::slint_app::pgn_launch_queue::from_arguments(arguments);
    auto workspace = motif::slint_app::workspace_service {};
    auto importer = motif::slint_app::import_service {};

    // Without an open database the import service refuses to start, and the
    // queue (which only hands out paths when the application asks) is untouched.
    auto const refused = importer.start(workspace.active_database(), first);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == motif::slint_app::import_error_code::no_database);
    REQUIRE(queue.pending() == 2);

    REQUIRE(workspace.activate_scratch().has_value());
    while (auto const next = queue.take_next()) {
        REQUIRE(importer.start(workspace.active_database(), *next).has_value());
        REQUIRE(wait_until_idle(importer).state == motif::slint_app::import_state::completed);
    }

    auto const count = workspace.active_database()->store().count_games();
    REQUIRE(count.has_value());
    REQUIRE(*count == 5);
}

// NOLINTEND(readability-function-cognitive-complexity)
