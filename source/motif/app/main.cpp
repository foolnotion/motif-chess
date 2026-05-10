#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <chrono>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <kddockwidgets/KDDockWidgets.h>
#include <kddockwidgets/qtquick/Platform.h>

#include "motif/app/app_config.hpp"
#include "motif/app/board_model.hpp"
#include "motif/app/database_workspace.hpp"
#include "motif/app/pgn_launch_queue.hpp"
#include "motif/app/workspace_controller.hpp"

int main(int argc, char* argv[])
{
    auto const start = std::chrono::steady_clock::now();

    KDDockWidgets::initFrontend(KDDockWidgets::FrontendType::QtQuick);

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("motif-chess"));
    QGuiApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QGuiApplication::setOrganizationName(QStringLiteral("motif"));

    // Collect positional arguments after Qt has consumed its own options.
    auto const qt_args = QGuiApplication::arguments();
    std::vector<std::string> raw_args;
    for (int i = 1; i < qt_args.size(); ++i) {
        raw_args.push_back(qt_args[i].toStdString());
    }
    auto pgn_queue = motif::app::parse_pgn_args(raw_args);

    for (auto const& invalid : pgn_queue.invalid_paths) {
        fmt::print(stderr, "warning: skipped launch argument '{}': {}\n", invalid.raw, invalid.error_message);
    }

    auto config_res = motif::app::load_config();
    auto config = motif::app::default_config();
    if (!config_res) {
        fmt::print(stderr,
                   "error: failed to load config: {} ({}); using in-memory defaults\n",
                   motif::app::to_string(config_res.error().code),
                   config_res.error().message);
    } else {
        config = std::move(*config_res);
    }

    motif::app::database_workspace workspace(&config);
    motif::app::workspace_controller controller(&workspace, &pgn_queue);
    motif::app::board_model board(&workspace);

    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    fmt::print(stdout, "startup: {}ms\n", elapsed);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("workspace"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("board"), &board);
    KDDockWidgets::QtQuick::Platform::instance()->setQmlEngine(&engine);
    engine.loadFromModule(QStringLiteral("com.motif.app"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    return QGuiApplication::exec();
}
