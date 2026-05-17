#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "motif/app/workspace_controller.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>

#include "motif/app/app_config.hpp"
#include "motif/app/database_workspace.hpp"
#include "motif/app/pgn_launch_queue.hpp"

namespace
{

struct tmp_dir
{
    std::filesystem::path path;

    explicit tmp_dir(std::string const& suffix)
    {
        auto const tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("motif_wc_test_" + suffix + "_" + std::to_string(tick));
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
    // NOLINTBEGIN(hicpp-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,hicpp-no-array-decay,cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    static char arg0[] = "motif_wc_test";
    static char* argv[] = {arg0, nullptr};
    static QCoreApplication app(argc, argv);
    // NOLINTEND(hicpp-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,hicpp-no-array-decay,cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    return app;
}

void pump_events_for(std::chrono::milliseconds const duration)
{
    QEventLoop loop;
    QTimer::singleShot(static_cast<int>(duration.count()), &loop, &QEventLoop::quit);
    loop.exec();
}

auto write_minimal_pgn(std::filesystem::path const& dir) -> std::filesystem::path
{
    std::filesystem::create_directories(dir);
    auto const pgn_path = dir / "test.pgn";
    std::ofstream out(pgn_path);
    out << "[Event \"Test\"]\n"
           "[Site \"?\"]\n"
           "[Date \"2024.01.01\"]\n"
           "[Round \"1\"]\n"
           "[White \"Player A\"]\n"
           "[Black \"Player B\"]\n"
           "[Result \"1-0\"]\n\n"
           "1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 1-0\n";
    return pgn_path;
}

struct test_context
{
    motif::app::app_config cfg;
    motif::app::database_workspace workspace {&cfg};
    motif::app::pgn_launch_queue pgn_queue;
    motif::app::workspace_controller controller {&workspace, &pgn_queue};

    test_context() { ensure_qcore_application(); }
};

constexpr auto k_open_timeout_ms = std::chrono::milliseconds {2000};
constexpr auto k_import_timeout_ms = std::chrono::milliseconds {5000};

}  // namespace

// ── open_database ────────────────────────────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace_controller: open_database success path emits loading transitions then active", "[motif-app]")
{
    test_context ctx;
    tmp_dir const db_dir {"open_success"};
    REQUIRE(ctx.workspace.create_database(db_dir.path.string(), "TestDB").has_value());
    ctx.workspace.close_active();

    int loading_changes {0};
    int active_changes {0};
    int recent_changes {0};
    int errors {0};
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::loading_changed, [&]() -> void { ++loading_changes; });
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::active_changed, [&]() -> void { ++active_changes; });
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::recent_changed, [&]() -> void { ++recent_changes; });
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::error_occurred, [&](QString const&) -> void { ++errors; });

    auto const result = ctx.controller.open_database(QString::fromStdString(db_dir.path.string()));
    REQUIRE(result);
    REQUIRE(ctx.controller.is_loading());

    pump_events_for(k_open_timeout_ms);

    REQUIRE_FALSE(ctx.controller.is_loading());
    REQUIRE(ctx.controller.has_active());
    CHECK(loading_changes >= 2);  // true then false
    CHECK(active_changes == 1);
    CHECK(recent_changes == 1);
    CHECK(errors == 0);
    CHECK(ctx.controller.loading_name().isEmpty());
    CHECK(ctx.controller.loading_total_games() == 0);
}

TEST_CASE("workspace_controller: open_database reentry is blocked while loading", "[motif-app]")
{
    test_context ctx;
    tmp_dir const db_dir {"open_reentry"};
    REQUIRE(ctx.workspace.create_database(db_dir.path.string(), "TestDB").has_value());
    ctx.workspace.close_active();

    int active_changes {0};
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::active_changed, [&]() -> void { ++active_changes; });

    auto const first = ctx.controller.open_database(QString::fromStdString(db_dir.path.string()));
    auto const second = ctx.controller.open_database(QString::fromStdString(db_dir.path.string()));
    REQUIRE(first);
    REQUIRE_FALSE(second);

    pump_events_for(k_open_timeout_ms);

    CHECK(active_changes == 1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace_controller: open_database failure clears loading and emits error", "[motif-app]")
{
    test_context ctx;

    int loading_changes {0};
    int active_changes {0};
    int errors {0};
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::loading_changed, [&]() -> void { ++loading_changes; });
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::active_changed, [&]() -> void { ++active_changes; });
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::error_occurred, [&](QString const&) -> void { ++errors; });

    auto const result = ctx.controller.open_database(QStringLiteral("/nonexistent/path/does/not/exist"));
    REQUIRE(result);

    pump_events_for(k_open_timeout_ms);

    REQUIRE_FALSE(ctx.controller.is_loading());
    REQUIRE_FALSE(ctx.controller.has_active());
    CHECK(errors == 1);
    CHECK(active_changes == 0);
    CHECK(loading_changes >= 2);
    CHECK(ctx.controller.loading_name().isEmpty());
    CHECK(ctx.controller.loading_total_games() == 0);
}

// ── import_pgn ───────────────────────────────────────────────────────────────

TEST_CASE("workspace_controller: import_pgn without active DB emits error", "[motif-app]")
{
    test_context ctx;

    int errors {0};
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::error_occurred, [&](QString const&) -> void { ++errors; });

    ctx.controller.import_pgn(QStringLiteral("anything.pgn"));

    REQUIRE_FALSE(ctx.controller.is_importing());
    REQUIRE(errors == 1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("workspace_controller: import_pgn happy path emits start/progress/finish", "[motif-app]")
{
    test_context ctx;
    tmp_dir const db_dir {"import_happy"};
    tmp_dir const pgn_dir {"import_pgn"};
    REQUIRE(ctx.workspace.create_database(db_dir.path.string(), "TestDB").has_value());

    auto const pgn_path = write_minimal_pgn(pgn_dir.path);

    int importing_changes {0};
    int progress_changes {0};
    int finished_count {0};
    int errors {0};
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::importing_changed, [&]() -> void { ++importing_changes; });
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::import_progress_changed, [&]() -> void { ++progress_changes; });
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::import_finished, [&](int, int) -> void { ++finished_count; });
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::error_occurred, [&](QString const&) -> void { ++errors; });

    ctx.controller.import_pgn(QString::fromStdString(pgn_path.string()));
    REQUIRE(ctx.controller.is_importing());

    pump_events_for(k_import_timeout_ms);

    REQUIRE_FALSE(ctx.controller.is_importing());
    CHECK(errors == 0);
    CHECK(finished_count == 1);
    CHECK(importing_changes >= 2);
    CHECK(progress_changes >= 1);
    CHECK(ctx.controller.import_processed() >= 1);
}

TEST_CASE("workspace_controller: import_pgn reentry is ignored while importing", "[motif-app]")
{
    test_context ctx;
    tmp_dir const db_dir {"import_reentry"};
    tmp_dir const pgn_dir {"import_reentry_pgn"};
    REQUIRE(ctx.workspace.create_database(db_dir.path.string(), "TestDB").has_value());

    auto const pgn_path = write_minimal_pgn(pgn_dir.path);

    int finished_count {0};
    QObject::connect(&ctx.controller, &motif::app::workspace_controller::import_finished, [&](int, int) -> void { ++finished_count; });

    ctx.controller.import_pgn(QString::fromStdString(pgn_path.string()));
    ctx.controller.import_pgn(QString::fromStdString(pgn_path.string()));  // must be ignored

    pump_events_for(k_import_timeout_ms);

    CHECK(finished_count == 1);
}

TEST_CASE("workspace_controller: destruction while import in progress does not crash or hang", "[motif-app]")
{
    ensure_qcore_application();
    tmp_dir const db_dir {"import_teardown"};
    tmp_dir const pgn_dir {"import_teardown_pgn"};

    motif::app::app_config cfg;
    motif::app::database_workspace workspace {&cfg};
    motif::app::pgn_launch_queue const pgn_queue;
    REQUIRE(workspace.create_database(db_dir.path.string(), "TestDB").has_value());

    auto const pgn_path = write_minimal_pgn(pgn_dir.path);

    {
        motif::app::workspace_controller controller {&workspace, &pgn_queue};
        controller.import_pgn(QString::fromStdString(pgn_path.string()));
    }
    REQUIRE(true);
}
