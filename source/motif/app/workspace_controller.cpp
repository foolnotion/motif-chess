#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QVariantMap>
#include <QtGlobal>
#include <filesystem>
#include <string>

#include "motif/app/workspace_controller.hpp"

#include <fmt/format.h>

#include "motif/app/database_workspace.hpp"
#include "motif/app/pgn_launch_queue.hpp"
#include "motif/db/error.hpp"
#include "motif/db/manifest.hpp"
#include "motif/import/import_pipeline.hpp"

namespace motif::app
{

workspace_controller::workspace_controller(database_workspace* workspace, pgn_launch_queue const* pgn_queue, QObject* parent)
    : QObject(parent)
    , workspace_(workspace)
    , pgn_queue_(pgn_queue)
{
    Q_ASSERT(workspace != nullptr);
    Q_ASSERT(pgn_queue != nullptr);
}

workspace_controller::~workspace_controller()
{
    if (pipeline_) {
        pipeline_->request_stop();
    }
    if (open_thread_ != nullptr) {
        open_thread_->wait();
    }
    if (import_thread_ != nullptr) {
        import_thread_->wait();
    }
}

auto workspace_controller::has_active() const -> bool
{
    return workspace_->has_active();
}

auto workspace_controller::is_temporary() const -> bool
{
    return workspace_->is_temporary();
}

auto workspace_controller::display_name() const -> QString
{
    return QString::fromStdString(std::string {workspace_->display_name()});
}

auto workspace_controller::active_path() const -> QString
{
    return QString::fromStdString(std::string {workspace_->active_path()});
}

auto workspace_controller::recent_databases() const -> QVariantList
{
    QVariantList result;
    for (auto const& status : workspace_->recent_with_status()) {
        QVariantMap entry;
        entry[QStringLiteral("name")] = QString::fromStdString(status.entry.name);
        entry[QStringLiteral("path")] = QString::fromStdString(status.entry.path);
        entry[QStringLiteral("available")] = status.available;
        result.append(entry);
    }
    return result;
}

auto workspace_controller::has_queued_pgn() const -> bool
{
    return !pgn_queue_->empty();
}

auto workspace_controller::queued_pgn_count() const -> int
{
    return static_cast<int>(pgn_queue_->size());
}

bool workspace_controller::create_database(QString const& dir_path, QString const& name)
{
    if (auto res = workspace_->create_database(dir_path.toStdString(), name.toStdString()); !res) {
        emit error_occurred(QString::fromStdString(res.error().message));
        return false;
    }
    emit active_changed();
    emit recent_changed();
    return true;
}

bool workspace_controller::open_database(QString const& dir_path)
{
    if (is_loading_) {
        return false;
    }

    auto const path = std::filesystem::path(dir_path.toStdString());
    auto manifest_res = motif::db::read_manifest(path / "manifest.json");

    is_loading_ = true;
    loading_total_games_ = manifest_res ? static_cast<int>(manifest_res->game_count) : 0;
    loading_name_ = manifest_res ? QString::fromStdString(manifest_res->name) : dir_path;
    emit loading_changed();  // NOLINT(misc-include-cleaner)

    open_thread_ = QThread::create(
        [this, path, dir_path_str = dir_path.toStdString()]()
        {
            // Only the slow DuckDB open runs here; all workspace state writes happen
            // on the main thread in the QueuedConnection lambda below.
            auto open_res = motif::db::database_manager::open(path);
            bool const ok = open_res.has_value();

            auto db_ptr = ok ? std::make_shared<motif::db::database_manager>(std::move(*open_res)) : nullptr;
            QString const err =
                !ok ? QString::fromStdString(fmt::format("open failed: {}", motif::db::to_string(open_res.error()))) : QString {};

            QMetaObject::invokeMethod(
                this,
                [this, ok, db_ptr, dir_path_str, err]() mutable
                {
                    is_loading_ = false;
                    open_thread_ = nullptr;
                    emit loading_changed();  // NOLINT(misc-include-cleaner)
                    if (!ok) {
                        emit error_occurred(err);  // NOLINT(misc-include-cleaner)
                        return;
                    }
                    if (auto res = workspace_->accept_opened_database(std::move(*db_ptr), dir_path_str); !res) {
                        emit error_occurred(QString::fromStdString(res.error().message));  // NOLINT(misc-include-cleaner)
                        return;
                    }
                    emit active_changed();  // NOLINT(misc-include-cleaner)
                    emit recent_changed();  // NOLINT(misc-include-cleaner)
                },
                Qt::QueuedConnection);
        });
    connect(open_thread_, &QThread::finished, open_thread_, &QThread::deleteLater);
    open_thread_->start();
    return true;
}

bool workspace_controller::open_scratch()
{
    if (auto res = workspace_->open_scratch(); !res) {
        emit error_occurred(QString::fromStdString(res.error().message));
        return false;
    }
    emit active_changed();
    return true;
}

bool workspace_controller::remove_recent(QString const& path)
{
    if (auto res = workspace_->remove_recent_entry(path.toStdString()); !res) {
        emit error_occurred(QString::fromStdString(res.error().message));  // NOLINT(misc-include-cleaner)
        return false;
    }
    emit recent_changed();  // NOLINT(misc-include-cleaner)
    return true;
}

auto workspace_controller::is_loading() const -> bool
{
    return is_loading_;
}

auto workspace_controller::loading_total_games() const -> int
{
    return loading_total_games_;
}

auto workspace_controller::loading_name() const -> QString
{
    return loading_name_;
}

auto workspace_controller::is_importing() const -> bool
{
    return is_importing_;
}

auto workspace_controller::import_processed() const -> int
{
    return import_processed_;
}

auto workspace_controller::import_total() const -> int
{
    return import_total_;
}

auto workspace_controller::import_phase_text() const -> QString
{
    return import_phase_text_;
}

void workspace_controller::import_pgn(QString const& path)
{
    if (is_importing_) {
        return;
    }

    auto* dbm = workspace_->persistent_db();
    if (dbm == nullptr) {
        emit error_occurred(QStringLiteral("No active database"));  // NOLINT(misc-include-cleaner)
        return;
    }

    is_importing_ = true;
    import_processed_ = 0;
    import_total_ = 0;
    import_phase_text_ = QStringLiteral("Preparing…");
    emit importing_changed();  // NOLINT(misc-include-cleaner)
    emit import_progress_changed();  // NOLINT(misc-include-cleaner)

    pipeline_ = std::make_unique<import::import_pipeline>(*dbm);

    progress_timer_ = new QTimer(this);
    progress_timer_->setInterval(200);
    connect(progress_timer_, &QTimer::timeout, this, &workspace_controller::poll_import_progress);
    progress_timer_->start();

    import_thread_ = QThread::create(
        [this, path_str = path.toStdString()]()
        {
            auto result = pipeline_->run(path_str);
            bool ok = result.has_value();
            int committed = ok ? static_cast<int>(result->committed) : 0;
            int errors = ok ? static_cast<int>(result->errors) : 0;
            QString msg = !ok ? QString::fromStdString(result.error().message) : QString {};
            QMetaObject::invokeMethod(
                this, [this, ok, committed, errors, msg]() { finish_import(ok, committed, errors, msg); }, Qt::QueuedConnection);
        });
    connect(import_thread_, &QThread::finished, import_thread_, &QThread::deleteLater);
    import_thread_->start();
}

void workspace_controller::poll_import_progress()
{
    if (!pipeline_) {
        return;
    }
    auto p = pipeline_->progress();
    import_processed_ = static_cast<int>(p.games_processed);
    import_total_ = static_cast<int>(p.total_games);

    switch (p.phase) {
        case import::import_phase::ingesting:
            import_phase_text_ = QStringLiteral("Ingesting games…");
            break;
        case import::import_phase::rebuilding:
            import_phase_text_ = QStringLiteral("Rebuilding positions…");
            break;
        default:
            import_phase_text_ = QStringLiteral("Preparing…");
            break;
    }

    emit import_progress_changed();  // NOLINT(misc-include-cleaner)
}

void workspace_controller::finish_import(bool success, int committed, int errors, QString error_message)
{
    if (progress_timer_ != nullptr) {
        progress_timer_->stop();
        progress_timer_->deleteLater();
        progress_timer_ = nullptr;
    }
    import_thread_ = nullptr;

    is_importing_ = false;
    pipeline_.reset();
    emit importing_changed();  // NOLINT(misc-include-cleaner)

    if (!success) {
        emit error_occurred(error_message);  // NOLINT(misc-include-cleaner)
        return;
    }
    emit import_finished(committed, errors);  // NOLINT(misc-include-cleaner)
}

}  // namespace motif::app
