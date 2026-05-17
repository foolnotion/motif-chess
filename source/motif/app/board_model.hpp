#pragma once

#include <QObject>
#include <QStringList>

#include "motif/app/database_workspace.hpp"
#include "motif/app/game_navigator.hpp"

namespace motif::app
{

// Qt-facing wrapper around game_navigator. Registered as context property "board" in main.cpp.
// All navigation logic is delegated to game_navigator; this class only handles Qt signal/property plumbing.
class board_model : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString fen READ fen NOTIFY position_changed)
    Q_PROPERTY(QString current_san READ current_san NOTIFY position_changed)
    Q_PROPERTY(int current_ply READ current_ply NOTIFY position_changed)
    Q_PROPERTY(int total_plies READ total_plies NOTIFY position_changed)
    Q_PROPERTY(bool game_loaded READ game_loaded NOTIFY game_loaded_changed)
    Q_PROPERTY(QStringList move_list READ move_list NOTIFY game_loaded_changed)
    Q_PROPERTY(QString white_name READ white_name NOTIFY game_loaded_changed)
    Q_PROPERTY(QString black_name READ black_name NOTIFY game_loaded_changed)
    Q_PROPERTY(QString game_result READ game_result NOTIFY game_loaded_changed)
    Q_PROPERTY(QString event_name READ event_name NOTIFY game_loaded_changed)
    Q_PROPERTY(QString game_date READ game_date NOTIFY game_loaded_changed)

  public:
    explicit board_model(database_workspace* workspace, QObject* parent = nullptr);

    [[nodiscard]] auto fen() const -> QString;
    [[nodiscard]] auto current_san() const -> QString;
    [[nodiscard]] auto current_ply() const -> int;
    [[nodiscard]] auto total_plies() const -> int;
    [[nodiscard]] auto game_loaded() const -> bool;
    [[nodiscard]] auto move_list() const -> QStringList;
    [[nodiscard]] auto white_name() const -> QString;
    [[nodiscard]] auto black_name() const -> QString;
    [[nodiscard]] auto game_result() const -> QString;
    [[nodiscard]] auto event_name() const -> QString;
    [[nodiscard]] auto game_date() const -> QString;

    Q_INVOKABLE void load_game(quint32 game_id);
    Q_INVOKABLE void advance();
    Q_INVOKABLE void retreat();
    Q_INVOKABLE void jump_to_start();
    Q_INVOKABLE void jump_to_end();
    Q_INVOKABLE void navigate_to(int ply);

  signals:
    void position_changed();
    void game_loaded_changed();
    void error_occurred(QString const& message);

  private:
    database_workspace* workspace_ {nullptr};
    game_navigator navigator_;
    QStringList move_list_;
    QString white_name_;
    QString black_name_;
    QString game_result_;
    QString event_name_;
    QString game_date_;
};

}  // namespace motif::app
