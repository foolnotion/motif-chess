#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <memory>

#include "motif/app/database_workspace.hpp"
#include "motif/app/pgn_launch_queue.hpp"

QT_FORWARD_DECLARE_CLASS(QThread)
QT_FORWARD_DECLARE_CLASS(QTimer)

namespace motif::import
{
class import_pipeline;
}  // namespace motif::import

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
    Q_PROPERTY(bool is_loading READ is_loading NOTIFY loading_changed)
    Q_PROPERTY(int loading_total_games READ loading_total_games NOTIFY loading_changed)
    Q_PROPERTY(QString loading_name READ loading_name NOTIFY loading_changed)
    Q_PROPERTY(bool is_importing READ is_importing NOTIFY importing_changed)
    Q_PROPERTY(int import_processed READ import_processed NOTIFY import_progress_changed)
    Q_PROPERTY(int import_total READ import_total NOTIFY import_progress_changed)
    Q_PROPERTY(QString import_phase_text READ import_phase_text NOTIFY import_progress_changed)

  public:
    explicit workspace_controller(database_workspace* workspace, pgn_launch_queue const* pgn_queue, QObject* parent = nullptr);
    ~workspace_controller() override;
    workspace_controller(workspace_controller const&) = delete;
    workspace_controller(workspace_controller&&) = delete;
    auto operator=(workspace_controller const&) -> workspace_controller& = delete;
    auto operator=(workspace_controller&&) -> workspace_controller& = delete;

    [[nodiscard]] auto has_active() const -> bool;
    [[nodiscard]] auto is_temporary() const -> bool;
    [[nodiscard]] auto display_name() const -> QString;
    [[nodiscard]] auto active_path() const -> QString;
    [[nodiscard]] auto recent_databases() const -> QVariantList;
    [[nodiscard]] auto has_queued_pgn() const -> bool;
    [[nodiscard]] auto queued_pgn_count() const -> int;
    [[nodiscard]] auto is_loading() const -> bool;
    [[nodiscard]] auto loading_total_games() const -> int;
    [[nodiscard]] auto loading_name() const -> QString;
    [[nodiscard]] auto is_importing() const -> bool;
    [[nodiscard]] auto import_processed() const -> int;
    [[nodiscard]] auto import_total() const -> int;
    [[nodiscard]] auto import_phase_text() const -> QString;

    Q_INVOKABLE auto create_database(QString const& dir_path, QString const& name) -> bool;
    Q_INVOKABLE auto open_database(QString const& dir_path) -> bool;
    Q_INVOKABLE auto open_scratch() -> bool;
    Q_INVOKABLE auto remove_recent(QString const& path) -> bool;
    Q_INVOKABLE void import_pgn(QString const& path);

  signals:
    void active_changed();
    void recent_changed();
    void error_occurred(QString const& message);
    void loading_changed();
    void importing_changed();
    void import_finished(int committed, int errors);
    void import_progress_changed();

  private:
    void poll_import_progress();
    void finish_import(bool success, int committed, int errors, QString const& error_message);

    database_workspace* workspace_ {nullptr};
    pgn_launch_queue const* pgn_queue_ {nullptr};
    bool is_loading_ {false};
    int loading_total_games_ {0};
    QString loading_name_;
    bool is_importing_ {false};
    int import_processed_ {0};
    int import_total_ {0};
    QString import_phase_text_;
    std::unique_ptr<motif::import::import_pipeline> pipeline_;
    QTimer* progress_timer_ {nullptr};
    QThread* open_thread_ {nullptr};
    QThread* import_thread_ {nullptr};
};

}  // namespace motif::app
