#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "motif/app/game_list_model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QModelIndex>
#include <QtCore/QTimer>

#include "motif/app/app_config.hpp"
#include "motif/app/database_workspace.hpp"
#include "motif/db/types.hpp"

namespace
{

struct tmp_dir
{
    std::filesystem::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("motif_glist_test_" + suffix + "_" + std::to_string(tick));
    }

    ~tmp_dir() { std::filesystem::remove_all(path); }

    tmp_dir(tmp_dir const&) = delete;
    auto operator=(tmp_dir const&) -> tmp_dir& = delete;
    tmp_dir(tmp_dir&&) = delete;
    auto operator=(tmp_dir&&) noexcept -> tmp_dir& = delete;
};

auto ensure_qcore_application() -> QCoreApplication&
{
    static auto argc = 1;
    static char arg0[] = "motif_app_test";
    static char* argv[] = {arg0, nullptr};
    static QCoreApplication app(argc, argv);
    return app;
}

void pump_events_for(std::chrono::milliseconds const duration)
{
    QEventLoop loop;
    QTimer::singleShot(static_cast<int>(duration.count()), &loop, &QEventLoop::quit);
    loop.exec();
}

// Build a minimal game with the required fields.
[[nodiscard]] auto make_game(std::string white_name,  // NOLINT(llvm-prefer-static-over-anonymous-namespace)
                             std::string black_name,
                             std::string result_str) -> motif::db::game
{
    return motif::db::game {
        .white = motif::db::player {.name = std::move(white_name), .elo = std::nullopt, .title = std::nullopt, .country = std::nullopt},
        .black = motif::db::player {.name = std::move(black_name), .elo = std::nullopt, .title = std::nullopt, .country = std::nullopt},
        .event_details = std::nullopt,
        .date = std::nullopt,
        .result = std::move(result_str),
        .eco = std::nullopt,
        .moves = {},
        .extra_tags = {},
        .provenance = {},
    };
}

}  // namespace

// ── Data path: empty database ────────────────────────────────────────────────

TEST_CASE("game_list_model data path: empty database returns zero rows", "[game-list-model]")
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);
    tmp_dir const tmp {"empty"};

    REQUIRE(workspace.create_database(tmp.path.string(), "EmptyDB").has_value());
    auto* mgr = workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 0);
    CHECK(list_result->games.empty());
}

// ── Data path: single game ───────────────────────────────────────────────────

TEST_CASE("game_list_model data path: single inserted game is retrieved", "[game-list-model]")
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);
    tmp_dir const tmp {"single"};

    REQUIRE(workspace.create_database(tmp.path.string(), "SingleDB").has_value());
    auto* mgr = workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    auto single_game = make_game("Magnus Carlsen", "Fabiano Caruana", "1-0");
    single_game.event_details = motif::db::event {.name = "World Championship", .site = std::nullopt, .date = std::nullopt};
    single_game.date = "2021.11.26";
    single_game.eco = "C65";
    REQUIRE(mgr->store().insert(single_game).has_value());

    motif::db::search_filter filter;
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 1);
    REQUIRE(list_result->games.size() == 1);

    auto const& entry = list_result->games[0];
    CHECK(entry.white == "Magnus Carlsen");
    CHECK(entry.black == "Fabiano Caruana");
    CHECK(entry.result == "1-0");
    CHECK(entry.event == "World Championship");
    CHECK(entry.date == "2021.11.26");
    CHECK(entry.eco == "C65");
}

// ── Data path: player-name filter ────────────────────────────────────────────

namespace
{

struct player_filter_db
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace {&cfg};
    tmp_dir tmp {"filter_name"};

    player_filter_db()
    {
        REQUIRE(workspace.create_database(tmp.path.string(), "FilterDB").has_value());
        auto* mgr = workspace.persistent_db();
        REQUIRE(mgr != nullptr);
        REQUIRE(mgr->store().insert(make_game("Alice Smith", "Bob Jones", "1-0")).has_value());
        REQUIRE(mgr->store().insert(make_game("Charlie Brown", "Alice Smith", "0-1")).has_value());
        REQUIRE(mgr->store().insert(make_game("Dave White", "Eve Black", "1/2-1/2")).has_value());
    }
};

}  // namespace

TEST_CASE("game_list_model data path: lowercase query matches either side", "[game-list-model]")
{
    player_filter_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.player_name = "alice";
    filter.player_color = motif::db::player_color::either;
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 2);
    CHECK(list_result->games.size() == 2);
}

TEST_CASE("game_list_model data path: uppercase query returns same games case-insensitively", "[game-list-model]")
{
    player_filter_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.player_name = "ALICE";
    filter.player_color = motif::db::player_color::either;
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 2);
}

TEST_CASE("game_list_model data path: white-only filter matches player only as white", "[game-list-model]")
{
    player_filter_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.player_name = "Alice";
    filter.player_color = motif::db::player_color::white;
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 1);
    REQUIRE(list_result->games.size() == 1);
    CHECK(list_result->games[0].white == "Alice Smith");
}

TEST_CASE("game_list_model data path: black-only filter matches player only as black", "[game-list-model]")
{
    player_filter_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.player_name = "Alice";
    filter.player_color = motif::db::player_color::black;
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 1);
    REQUIRE(list_result->games.size() == 1);
    CHECK(list_result->games[0].black == "Alice Smith");
}

TEST_CASE("game_list_model data path: substring query matches all games for that player", "[game-list-model]")
{
    player_filter_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.player_name = "smit";
    filter.player_color = motif::db::player_color::either;
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 2);
}

TEST_CASE("game_list_model data path: non-matching name returns zero results", "[game-list-model]")
{
    player_filter_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.player_name = "ZZZNoSuchPlayer";
    filter.player_color = motif::db::player_color::either;
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 0);
    CHECK(list_result->games.empty());
}

// ── Data path: result filter ─────────────────────────────────────────────────

namespace
{

struct result_filter_db
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace {&cfg};
    tmp_dir tmp {"filter_result"};

    result_filter_db()
    {
        REQUIRE(workspace.create_database(tmp.path.string(), "ResultDB").has_value());
        auto* mgr = workspace.persistent_db();
        REQUIRE(mgr != nullptr);
        REQUIRE(mgr->store().insert(make_game("P1", "P2", "1-0")).has_value());
        REQUIRE(mgr->store().insert(make_game("P3", "P4", "0-1")).has_value());
        REQUIRE(mgr->store().insert(make_game("P5", "P6", "1/2-1/2")).has_value());
        REQUIRE(mgr->store().insert(make_game("P7", "P8", "1-0")).has_value());
    }
};

}  // namespace

TEST_CASE("game_list_model data path: result filter 1-0 returns white wins", "[game-list-model]")
{
    result_filter_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.result = "1-0";
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 2);
    for (auto const& entry : list_result->games) {
        CHECK(entry.result == "1-0");
    }
}

TEST_CASE("game_list_model data path: result filter 0-1 returns black win", "[game-list-model]")
{
    result_filter_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.result = "0-1";
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 1);
}

TEST_CASE("game_list_model data path: result filter 1/2-1/2 returns draw", "[game-list-model]")
{
    result_filter_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.result = "1/2-1/2";
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 1);
}

TEST_CASE("game_list_model data path: no result filter returns all games", "[game-list-model]")
{
    result_filter_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 4);
}

// ── Data path: duplicate player names ────────────────────────────────────────

TEST_CASE("game_list_model data path: duplicate player names both appear", "[game-list-model]")
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);
    tmp_dir const tmp {"dup_names"};

    REQUIRE(workspace.create_database(tmp.path.string(), "DupDB").has_value());
    auto* mgr = workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    REQUIRE(mgr->store().insert(make_game("Garry Kasparov", "Anatoly Karpov", "1-0")).has_value());
    REQUIRE(mgr->store().insert(make_game("Anatoly Karpov", "Garry Kasparov", "0-1")).has_value());
    REQUIRE(mgr->store().insert(make_game("Garry Kasparov", "Bobby Fischer", "1/2-1/2")).has_value());
    REQUIRE(mgr->store().insert(make_game("Bobby Fischer", "Mikhail Tal", "1-0")).has_value());

    motif::db::search_filter filter;
    filter.player_name = "Kasparov";
    filter.player_color = motif::db::player_color::either;
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 3);
    CHECK(list_result->games.size() == 3);
}

// ── Data path: combined player + result filter ───────────────────────────────

namespace
{

struct combined_filter_db
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace {&cfg};
    tmp_dir tmp {"combined"};

    combined_filter_db()
    {
        REQUIRE(workspace.create_database(tmp.path.string(), "CombDB").has_value());
        auto* mgr = workspace.persistent_db();
        REQUIRE(mgr != nullptr);
        REQUIRE(mgr->store().insert(make_game("Magnus", "Fabiano", "1-0")).has_value());
        REQUIRE(mgr->store().insert(make_game("Magnus", "Hikaru", "0-1")).has_value());
        REQUIRE(mgr->store().insert(make_game("Fabiano", "Magnus", "1/2-1/2")).has_value());
    }
};

}  // namespace

TEST_CASE("game_list_model data path: player + 1-0 result filter narrows to one game", "[game-list-model]")
{
    combined_filter_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.player_name = "Magnus";
    filter.player_color = motif::db::player_color::either;
    filter.result = "1-0";
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 1);
}

TEST_CASE("game_list_model data path: player + 0-1 result filter narrows to one game", "[game-list-model]")
{
    combined_filter_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.player_name = "Magnus";
    filter.player_color = motif::db::player_color::either;
    filter.result = "0-1";
    filter.limit = motif::db::max_search_limit;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == 1);
}

// ── Data path: pagination ─────────────────────────────────────────────────────

namespace
{

constexpr int k_num_test_games = 25;
constexpr std::size_t k_page_size = 10;
constexpr std::size_t k_page_offset = 20;
constexpr std::size_t k_far_offset = 100;
constexpr std::size_t k_remain_count = 5;

struct pagination_db
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace {&cfg};
    tmp_dir tmp {"pagination"};

    pagination_db()
    {
        REQUIRE(workspace.create_database(tmp.path.string(), "PageDB").has_value());
        auto* mgr = workspace.persistent_db();
        REQUIRE(mgr != nullptr);
        for (int idx = 0; idx < k_num_test_games; ++idx) {
            auto page_game = make_game("White" + std::to_string(idx), "Black" + std::to_string(idx), "1-0");
            REQUIRE(mgr->store().insert(page_game).has_value());
        }
    }
};

}  // namespace

TEST_CASE("game_list_model data path: total_count reflects all games regardless of limit", "[game-list-model]")
{
    pagination_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.limit = k_page_size;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == k_num_test_games);
    CHECK(list_result->games.size() == k_page_size);
}

TEST_CASE("game_list_model data path: offset skips games correctly", "[game-list-model]")
{
    pagination_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.limit = k_page_size;
    filter.offset = k_page_offset;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == k_num_test_games);
    CHECK(list_result->games.size() == k_remain_count);
}

TEST_CASE("game_list_model data path: offset beyond total returns empty games with correct total_count", "[game-list-model]")
{
    pagination_db fixture;
    auto* mgr = fixture.workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    motif::db::search_filter filter;
    filter.limit = k_page_size;
    filter.offset = k_far_offset;

    auto list_result = mgr->store().find_games(filter);
    REQUIRE(list_result.has_value());
    CHECK(list_result->total_count == k_num_test_games);
    CHECK(list_result->games.empty());
}

TEST_CASE("game_list_model API: exposes rows, headers, ids and filters via Qt model interface", "[game-list-model]")
{
    (void)ensure_qcore_application();

    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);
    tmp_dir const tmp {"model_api"};

    REQUIRE(workspace.create_database(tmp.path.string(), "ModelApiDB").has_value());
    auto* mgr = workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    auto game_a = make_game("Alice Alpha", "Bob Beta", "1-0");
    game_a.event_details = motif::db::event {.name = "Alpha Event", .site = std::nullopt, .date = std::nullopt};
    game_a.date = "2026.05.10";
    game_a.eco = "C20";
    REQUIRE(mgr->store().insert(game_a).has_value());

    auto game_b = make_game("Carol Gamma", "Dave Delta", "0-1");
    game_b.event_details = motif::db::event {.name = "Beta Event", .site = std::nullopt, .date = std::nullopt};
    game_b.date = "2026.05.11";
    game_b.eco = "B01";
    REQUIRE(mgr->store().insert(game_b).has_value());

    motif::app::game_list_model model(&workspace);

    CHECK(model.columnCount(QModelIndex()) == motif::app::game_list_model::num_columns);
    CHECK(model.rowCount(QModelIndex()) == 2);
    CHECK(model.total_count() == 2);
    CHECK(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("White"));
    CHECK(model.headerData(5, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("ECO"));

    auto const idx_white = model.index(0, 0);
    auto const idx_event = model.index(0, 3);
    CHECK(model.data(idx_white, Qt::DisplayRole).toString() == QStringLiteral("Alice Alpha"));
    CHECK(model.data(idx_event, Qt::DisplayRole).toString() == QStringLiteral("Alpha Event"));

    auto const first_id = model.game_id_at(0);
    CHECK(first_id != 0);
    CHECK(model.game_id_at(-1) == 0);
    CHECK(model.game_id_at(999) == 0);

    model.set_result_filter(QStringLiteral("0-1"));
    CHECK(model.rowCount(QModelIndex()) == 1);
    CHECK(model.total_count() == 1);
    CHECK(model.data(model.index(0, 0), Qt::DisplayRole).toString() == QStringLiteral("Carol Gamma"));

    model.set_result_filter(QStringLiteral(""));
    CHECK(model.rowCount(QModelIndex()) == 2);
    CHECK(model.total_count() == 2);

    model.set_player_filter(QStringLiteral("alice"));
    pump_events_for(std::chrono::milliseconds {250});
    CHECK(model.rowCount(QModelIndex()) == 1);
    CHECK(model.total_count() == 1);
    CHECK(model.data(model.index(0, 0), Qt::DisplayRole).toString() == QStringLiteral("Alice Alpha"));

    model.set_player_filter(QStringLiteral(""));
    pump_events_for(std::chrono::milliseconds {250});
    CHECK(model.rowCount(QModelIndex()) == 2);
    CHECK(model.total_count() == 2);

    model.refresh();
    CHECK(model.rowCount(QModelIndex()) == 2);
}

TEST_CASE("game_list_model API: supports fetchMore pagination", "[game-list-model]")
{
    (void)ensure_qcore_application();

    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);
    tmp_dir const tmp {"model_fetch_more"};

    REQUIRE(workspace.create_database(tmp.path.string(), "ModelFetchDB").has_value());
    auto* mgr = workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    constexpr int num_games = 1200;
    for (int idx = 0; idx < num_games; ++idx) {
        auto game = make_game("W" + std::to_string(idx), "B" + std::to_string(idx), "1-0");
        REQUIRE(mgr->store().insert(game).has_value());
    }

    motif::app::game_list_model model(&workspace);
    CHECK(model.total_count() == num_games);
    CHECK(model.rowCount(QModelIndex()) < num_games);
    CHECK(model.canFetchMore(QModelIndex()));

    while (model.canFetchMore(QModelIndex())) {
        model.fetchMore(QModelIndex());
    }

    CHECK(model.rowCount(QModelIndex()) == num_games);
    CHECK_FALSE(model.canFetchMore(QModelIndex()));
}

TEST_CASE("game_list_model perf: initial model load stays below 100ms for 100k games", "[game-list-model][perf]")
{
    (void)ensure_qcore_application();

    motif::app::app_config cfg;
    motif::app::database_workspace workspace(&cfg);
    tmp_dir const tmp {"model_perf_100k"};

    REQUIRE(workspace.create_database(tmp.path.string(), "ModelPerfDB").has_value());
    auto* mgr = workspace.persistent_db();
    REQUIRE(mgr != nullptr);

    constexpr int num_games = 100000;
    for (int idx = 0; idx < num_games; ++idx) {
        auto game = make_game("W" + std::to_string(idx), "B" + std::to_string(idx), "1-0");
        REQUIRE(mgr->store().insert(game).has_value());
    }

    auto const start = std::chrono::steady_clock::now();
    motif::app::game_list_model model(&workspace);
    auto const elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    CHECK(model.total_count() == num_games);
    CHECK(elapsed_ms < 100);
}
