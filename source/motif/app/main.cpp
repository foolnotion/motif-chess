#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QStringList>
#include <chrono>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <kddockwidgets/KDDockWidgets.h>
#include <kddockwidgets/qtquick/Platform.h>

#include "motif/app/app_config.hpp"
#include "motif/app/board_model.hpp"
#include "motif/app/database_workspace.hpp"
#include "motif/app/game_list_model.hpp"
#include "motif/app/pgn_launch_queue.hpp"
#include "motif/app/workspace_controller.hpp"

int main(int argc, char* argv[])
{
    auto const start = std::chrono::steady_clock::now();

    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);  // NOLINT(misc-include-cleaner)
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("motif-chess"));
    QGuiApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QGuiApplication::setOrganizationName(QStringLiteral("motif"));

    KDDockWidgets::initFrontend(KDDockWidgets::FrontendType::QtQuick);

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
    motif::app::game_list_model game_list(&workspace);

    QObject::connect(&controller, &motif::app::workspace_controller::active_changed, &game_list, &motif::app::game_list_model::refresh);

    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    fmt::print(stdout, "startup: {}ms\n", elapsed);

    QQmlApplicationEngine engine;

    auto add_qt_paths_from_prefixes = [&](QString const& prefixes)
    {
        for (auto const& prefix : prefixes.split(':', Qt::SkipEmptyParts)) {
            auto const qml_candidate = QDir(prefix).filePath(QStringLiteral("lib/qt-6/qml"));
            if (QFileInfo(qml_candidate).isDir()) {
                engine.addImportPath(qml_candidate);
            }

            auto const plugin_candidate = QDir(prefix).filePath(QStringLiteral("lib/qt-6/plugins"));
            if (QFileInfo(plugin_candidate).isDir()) {
                QCoreApplication::addLibraryPath(plugin_candidate);
            }
        }
    };

    add_qt_paths_from_prefixes(QString::fromLocal8Bit(qgetenv("CMAKE_PREFIX_PATH")));
    add_qt_paths_from_prefixes(QString::fromLocal8Bit(qgetenv("NIXPKGS_CMAKE_PREFIX_PATH")));

    auto const qml2_import_path = QString::fromLocal8Bit(qgetenv("QML2_IMPORT_PATH"));
    for (auto const& import_path : qml2_import_path.split(':', Qt::SkipEmptyParts)) {
        engine.addImportPath(import_path);
    }

    auto const qml_import_path = QLibraryInfo::path(QLibraryInfo::QmlImportsPath);
    if (!qml_import_path.isEmpty()) {
        engine.addImportPath(qml_import_path);
    }

    auto const qt_plugin_path = QLibraryInfo::path(QLibraryInfo::PluginsPath);
    if (!qt_plugin_path.isEmpty()) {
        QCoreApplication::addLibraryPath(qt_plugin_path);
    }

    engine.rootContext()->setContextProperty(QStringLiteral("workspace"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("board"), &board);
    engine.rootContext()->setContextProperty(QStringLiteral("game_list"), &game_list);
    KDDockWidgets::QtQuick::Platform::instance()->setQmlEngine(&engine);
    engine.loadFromModule(QStringLiteral("com.motif.app"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    return QGuiApplication::exec();
}
