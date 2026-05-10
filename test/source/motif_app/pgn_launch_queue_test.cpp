#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "motif/app/pgn_launch_queue.hpp"

#include <catch2/catch_test_macros.hpp>

namespace
{

struct tmp_pgn_file
{
    std::filesystem::path path;

    explicit tmp_pgn_file(std::string const& name)
    {
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        auto const fname = std::filesystem::path {name};
        auto const unique = fname.stem().string() + "_" + std::to_string(tick) + fname.extension().string();
        path = std::filesystem::temp_directory_path() / unique;
        std::ofstream {path};
    }

    ~tmp_pgn_file()
    {
        std::error_code remove_err;
        std::filesystem::remove(path, remove_err);
    }

    tmp_pgn_file(tmp_pgn_file const&) = delete;
    auto operator=(tmp_pgn_file const&) -> tmp_pgn_file& = delete;
    tmp_pgn_file(tmp_pgn_file&&) = delete;
    auto operator=(tmp_pgn_file&&) -> tmp_pgn_file& = delete;
};

}  // namespace

TEST_CASE("pgn_launch_queue: empty args produces empty queue", "[motif-app]")
{
    auto const queue = motif::app::parse_pgn_args({});
    REQUIRE(queue.empty());
    REQUIRE(queue.invalid_paths.empty());
}

TEST_CASE("pgn_launch_queue: valid .pgn file is accepted", "[motif-app]")
{
    tmp_pgn_file const pgn {"motif_app_test_valid.pgn"};
    auto const queue = motif::app::parse_pgn_args({pgn.path.string()});
    REQUIRE(queue.size() == 1);
    REQUIRE(queue.valid_paths[0] == std::filesystem::canonical(pgn.path));
    REQUIRE(queue.invalid_paths.empty());
}

TEST_CASE("pgn_launch_queue: mixed-case .PGN extension is accepted", "[motif-app]")
{
    tmp_pgn_file const pgn {"motif_app_test_upper.PGN"};
    auto const queue = motif::app::parse_pgn_args({pgn.path.string()});
    REQUIRE(queue.size() == 1);
    REQUIRE(queue.invalid_paths.empty());
}

TEST_CASE("pgn_launch_queue: non-.pgn argument is rejected", "[motif-app]")
{
    auto const queue = motif::app::parse_pgn_args({"some_file.txt"});
    REQUIRE(queue.empty());
    REQUIRE(queue.invalid_paths.size() == 1);
    REQUIRE(queue.invalid_paths[0].error_message == "not a .pgn file");
}

TEST_CASE("pgn_launch_queue: missing file path is rejected", "[motif-app]")
{
    auto const queue = motif::app::parse_pgn_args({"/nonexistent/path/game.pgn"});
    REQUIRE(queue.empty());
    REQUIRE(queue.invalid_paths.size() == 1);
    REQUIRE(queue.invalid_paths[0].error_message == "path does not exist");
}

TEST_CASE("pgn_launch_queue: directory with .pgn name is rejected", "[motif-app]")
{
    auto const dir = std::filesystem::temp_directory_path() / "motif_app_test_dir.pgn";
    std::filesystem::create_directories(dir);
    auto const queue = motif::app::parse_pgn_args({dir.string()});
    std::filesystem::remove(dir);
    REQUIRE(queue.empty());
    REQUIRE(queue.invalid_paths.size() == 1);
    REQUIRE(queue.invalid_paths[0].error_message == "not a regular file");
}

TEST_CASE("pgn_launch_queue: multiple files — valid and invalid mixed", "[motif-app]")
{
    tmp_pgn_file const pgn {"motif_app_test_mixed.pgn"};
    auto const queue = motif::app::parse_pgn_args({
        pgn.path.string(),
        "missing.pgn",
        "document.txt",
    });
    REQUIRE(queue.size() == 1);
    REQUIRE(queue.invalid_paths.size() == 2);
}

TEST_CASE("pgn_launch_queue: parse_pgn_args has no filesystem side effects", "[motif-app]")
{
    auto const tmp = std::filesystem::temp_directory_path();
    auto count = [&]() -> std::ptrdiff_t
    { return std::distance(std::filesystem::directory_iterator {tmp}, std::filesystem::directory_iterator {}); };
    auto const before = count();
    auto const queue = motif::app::parse_pgn_args({"nonexistent.pgn"});
    REQUIRE(queue.empty());
    CHECK(count() == before);
}
