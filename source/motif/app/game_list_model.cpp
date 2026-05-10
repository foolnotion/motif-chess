#include <QAbstractTableModel>
#include <QModelIndex>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <Qt>
#include <QtTypes>
#include <cstddef>
#include <utility>  // NOLINT(misc-include-cleaner) — std::move; false positive from <Qt> pulling utility transitively

#include "motif/app/game_list_model.hpp"

#include <fmt/format.h>

#include "motif/app/database_workspace.hpp"
#include "motif/db/error.hpp"
#include "motif/db/types.hpp"  // NOLINT(misc-include-cleaner) — types used directly; false positive from database_workspace.hpp transitivity

namespace motif::app
{

namespace
{

constexpr int debounce_ms = 200;

}  // namespace

game_list_model::game_list_model(database_workspace* workspace, QObject* parent)
    : QAbstractTableModel(parent)
    , workspace_(workspace)
    , filter_timer_(new QTimer(this))
{
    filter_.limit = motif::db::max_search_limit;
    filter_timer_->setSingleShot(true);
    filter_timer_->setInterval(debounce_ms);
    connect(filter_timer_, &QTimer::timeout, this, &game_list_model::reload);
    reload();
}

auto game_list_model::rowCount(QModelIndex const& parent) const -> int
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(entries_.size());
}

auto game_list_model::columnCount(QModelIndex const& parent) const -> int
{
    if (parent.isValid()) {
        return 0;
    }
    return num_columns;
}

auto game_list_model::data(QModelIndex const& index, int role) const -> QVariant
{
    if (!index.isValid() || role != Qt::DisplayRole) {
        return {};
    }
    auto const row = index.row();
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return {};
    }
    auto const& entry = entries_[static_cast<std::size_t>(row)];
    switch (static_cast<column_index>(index.column())) {
        case column_index::col_white:
            return QString::fromStdString(entry.white);
        case column_index::col_black:
            return QString::fromStdString(entry.black);
        case column_index::col_result:
            return QString::fromStdString(entry.result);
        case column_index::col_event:
            return QString::fromStdString(entry.event);
        case column_index::col_date:
            return QString::fromStdString(entry.date);
        case column_index::col_eco:
            return QString::fromStdString(entry.eco);
    }
    return {};
}

auto game_list_model::headerData(int section, Qt::Orientation orientation, int role) const -> QVariant
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
        return {};
    }
    switch (static_cast<column_index>(section)) {
        case column_index::col_white:
            return QStringLiteral("White");
        case column_index::col_black:
            return QStringLiteral("Black");
        case column_index::col_result:
            return QStringLiteral("Result");
        case column_index::col_event:
            return QStringLiteral("Event");
        case column_index::col_date:
            return QStringLiteral("Date");
        case column_index::col_eco:
            return QStringLiteral("ECO");
    }
    return {};
}

auto game_list_model::canFetchMore(QModelIndex const& parent) const -> bool
{
    if (parent.isValid()) {
        return false;
    }
    auto const loaded = static_cast<qint64>(entries_.size());
    return loaded < total_count_;
}

void game_list_model::fetchMore(QModelIndex const& parent)
{
    if (parent.isValid()) {
        return;
    }
    append_page(entries_.size());
}

// NOLINTNEXTLINE(modernize-use-trailing-return-type,readability-convert-member-functions-to-static) — Q_INVOKABLE restrictions; false
// positive (accesses entries_)
quint32 game_list_model::game_id_at(int row) const
{
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return 0;
    }
    return entries_[static_cast<std::size_t>(row)].id.value;
}

void game_list_model::set_player_filter(QString const& name)
{
    auto const std_name = name.toStdString();
    // NOLINTBEGIN(bugprone-branch-clone) — false positive; reset() and assignment have different effects on the optional
    if (std_name.empty()) {
        filter_.player_name.reset();
    } else {
        filter_.player_name = std_name;
    }
    // NOLINTEND(bugprone-branch-clone)
    filter_.player_color = motif::db::player_color::either;
    filter_timer_->start();
}

void game_list_model::set_result_filter(QString const& result)
{
    auto const std_result = result.toStdString();
    // NOLINTBEGIN(bugprone-branch-clone) — false positive; reset() and assignment have different effects on the optional
    if (std_result.empty()) {
        filter_.result.reset();
    } else {
        filter_.result = std_result;
    }
    // NOLINTEND(bugprone-branch-clone)
    reload();
}

void game_list_model::refresh()
{
    filter_timer_->stop();
    reload();
}

void game_list_model::reload()
{
    filter_.offset = 0;
    filter_.limit = motif::db::max_search_limit;

    auto* mgr = workspace_ != nullptr ? workspace_->persistent_db() : nullptr;
    if (mgr == nullptr) {
        beginResetModel();
        entries_.clear();
        total_count_ = 0;
        endResetModel();
        emit total_count_changed(0);  // NOLINT(misc-include-cleaner) — emit is a Qt macro, no canonical header
        return;
    }

    auto res = mgr->store().find_games(filter_);
    beginResetModel();
    if (res) {
        entries_ = std::move(res->games);
        total_count_ = res->total_count;
    } else {
        entries_.clear();
        total_count_ = 0;
        emit error_occurred(  // NOLINT(misc-include-cleaner)
            QString::fromStdString(fmt::format("Failed to load games: {}", motif::db::to_string(res.error()))));
    }
    endResetModel();
    emit total_count_changed(total_count_);  // NOLINT(misc-include-cleaner)
}

void game_list_model::append_page(std::size_t const offset)
{
    auto* mgr = workspace_ != nullptr ? workspace_->persistent_db() : nullptr;
    if (mgr == nullptr) {
        return;
    }

    filter_.offset = offset;
    filter_.limit = motif::db::max_search_limit;

    auto res = mgr->store().find_games(filter_);
    if (!res) {
        emit error_occurred(  // NOLINT(misc-include-cleaner)
            QString::fromStdString(fmt::format("Failed to fetch more games: {}", motif::db::to_string(res.error()))));
        return;
    }

    if (res->games.empty()) {
        return;
    }

    auto const first = static_cast<int>(entries_.size());
    auto const last = first + static_cast<int>(res->games.size()) - 1;
    beginInsertRows({}, first, last);
    for (auto& entry : res->games) {
        entries_.push_back(std::move(entry));
    }
    endInsertRows();
    total_count_ = res->total_count;
    emit total_count_changed(total_count_);  // NOLINT(misc-include-cleaner)
}

}  // namespace motif::app
