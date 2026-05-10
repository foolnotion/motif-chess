#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include "motif/app/database_workspace.hpp"
#include "motif/app/pgn_launch_queue.hpp"

namespace motif::app
{

// Exposes database_workspace to QML via Q_PROPERTY and Q_INVOKABLE.
// Does not own the workspace or pgn_queue — both must outlive this object.
class workspace_controller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool has_active READ has_active NOTIFY active_changed)
    Q_PROPERTY(bool is_temporary READ is_temporary NOTIFY active_changed)
    Q_PROPERTY(QString display_name READ display_name NOTIFY active_changed)
    Q_PROPERTY(QString active_path READ active_path NOTIFY active_changed)
    Q_PROPERTY(QVariantList recent_databases READ recent_databases NOTIFY recent_changed)
    Q_PROPERTY(bool has_queued_pgn READ has_queued_pgn CONSTANT)
    Q_PROPERTY(int queued_pgn_count READ queued_pgn_count CONSTANT)

  public:
    explicit workspace_controller(database_workspace* workspace, pgn_launch_queue const* pgn_queue, QObject* parent = nullptr);

    [[nodiscard]] auto has_active() const -> bool;
    [[nodiscard]] auto is_temporary() const -> bool;
    [[nodiscard]] auto display_name() const -> QString;
    [[nodiscard]] auto active_path() const -> QString;
    [[nodiscard]] auto recent_databases() const -> QVariantList;
    [[nodiscard]] auto has_queued_pgn() const -> bool;
    [[nodiscard]] auto queued_pgn_count() const -> int;

    Q_INVOKABLE bool create_database(QString const& dir_path, QString const& name);
    Q_INVOKABLE bool open_database(QString const& dir_path);
    Q_INVOKABLE bool open_scratch();
    Q_INVOKABLE bool remove_recent(QString const& path);

  signals:
    void active_changed();
    void recent_changed();
    void error_occurred(QString const& message);

  private:
    database_workspace* workspace_ {nullptr};
    pgn_launch_queue const* pgn_queue_ {nullptr};
};

}  // namespace motif::app
