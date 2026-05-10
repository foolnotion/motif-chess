#include <QObject>
#include <QString>
#include <QStringList>
#include <cstddef>

#include "motif/app/board_model.hpp"

#include <fmt/format.h>

#include "motif/app/database_workspace.hpp"
#include "motif/db/types.hpp"

namespace motif::app
{

board_model::board_model(database_workspace* workspace, QObject* parent)
    : QObject(parent)
    , workspace_(workspace)
{
}

auto board_model::fen() const -> QString
{
    return QString::fromStdString(navigator_.current_fen());
}

auto board_model::current_san() const -> QString
{
    return QString::fromStdString(navigator_.current_san());
}

auto board_model::current_ply() const -> int
{
    return static_cast<int>(navigator_.current_ply());
}

auto board_model::total_plies() const -> int
{
    return static_cast<int>(navigator_.total_plies());
}

auto board_model::game_loaded() const -> bool
{
    return navigator_.has_game();
}

auto board_model::move_list() const -> QStringList
{
    auto const moves = navigator_.move_list();
    auto result = QStringList {};
    for (auto const& san : moves) {
        result << QString::fromStdString(san);
    }
    return result;
}

void board_model::load_game(quint32 const game_id)
{
    auto* mgr = workspace_ != nullptr ? workspace_->persistent_db() : nullptr;
    if (mgr == nullptr) {
        emit error_occurred(QStringLiteral("No active persistent database"));
        return;
    }

    auto game_result = mgr->store().get(motif::db::game_id {game_id});
    if (!game_result) {
        emit error_occurred(QString::fromStdString(fmt::format("Game {} not found", game_id)));
        return;
    }

    navigator_.load(*game_result);
    emit game_loaded_changed();
    emit position_changed();
}

void board_model::advance()
{
    navigator_.advance();
    emit position_changed();
}

void board_model::retreat()
{
    navigator_.retreat();
    emit position_changed();
}

void board_model::jump_to_start()
{
    navigator_.jump_to_start();
    emit position_changed();
}

void board_model::jump_to_end()
{
    navigator_.jump_to_end();
    emit position_changed();
}

void board_model::navigate_to(int const ply)
{
    navigator_.navigate_to(ply < 0 ? std::size_t {0} : static_cast<std::size_t>(ply));
    emit position_changed();
}

}  // namespace motif::app
