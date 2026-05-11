#include <QVariantMap>
#include <QtGlobal>
#include <string>

#include "motif/app/workspace_controller.hpp"

#include "motif/app/database_workspace.hpp"
#include "motif/app/pgn_launch_queue.hpp"
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
    if (auto res = workspace_->open_database(dir_path.toStdString()); !res) {
        emit error_occurred(QString::fromStdString(res.error().message));
        return false;
    }
    emit active_changed();
    emit recent_changed();
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

auto workspace_controller::is_importing() const -> bool
{
    return is_importing_;
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
    emit importing_changed();  // NOLINT(misc-include-cleaner)

    motif::import::import_pipeline pipeline(*dbm);
    auto result = pipeline.run(path.toStdString());

    is_importing_ = false;
    emit importing_changed();  // NOLINT(misc-include-cleaner)

    if (!result) {
        emit error_occurred(QString::fromStdString(result.error().message));  // NOLINT(misc-include-cleaner)
        return;
    }
    emit import_finished(static_cast<int>(result->committed), static_cast<int>(result->errors));  // NOLINT(misc-include-cleaner)
}

}  // namespace motif::app
