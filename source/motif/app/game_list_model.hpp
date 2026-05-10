#pragma once

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <cstdint>
#include <vector>

#include "motif/app/database_workspace.hpp"
#include "motif/db/types.hpp"

namespace motif::app
{

// QAbstractTableModel that exposes a paginated, filterable game list from the active database.
// Columns (left to right): White, Black, Result, Event, Date, ECO.
// Lazy-loads via canFetchMore/fetchMore (500 rows per page, max_search_limit).
// Player filter input is debounced (200 ms) to avoid flooding SQLite on every keystroke.
class game_list_model : public QAbstractTableModel
{
    Q_OBJECT

    Q_PROPERTY(qint64 total_count READ total_count NOTIFY total_count_changed)

  public:
    static constexpr int num_columns = 6;

    enum class column_index : std::uint8_t
    {
        col_white = 0,
        col_black = 1,
        col_result = 2,
        col_event = 3,
        col_date = 4,
        col_eco = 5,
    };

    explicit game_list_model(database_workspace* workspace, QObject* parent = nullptr);

    // QAbstractTableModel overrides — default arguments are on the base class
    [[nodiscard]] auto rowCount(QModelIndex const& parent) const -> int override;
    [[nodiscard]] auto columnCount(QModelIndex const& parent) const -> int override;
    [[nodiscard]] auto data(QModelIndex const& index, int role) const -> QVariant override;
    [[nodiscard]] auto headerData(int section, Qt::Orientation orientation, int role) const -> QVariant override;
    [[nodiscard]] auto canFetchMore(QModelIndex const& parent) const -> bool override;
    void fetchMore(QModelIndex const& parent) override;

    [[nodiscard]] auto total_count() const noexcept -> qint64 { return total_count_; }

    // Returns the game_id for the given row, or 0 if the row is out of range.
    // NOLINTNEXTLINE(modernize-use-trailing-return-type) — Q_INVOKABLE does not support trailing return type
    Q_INVOKABLE quint32 game_id_at(int row) const;

    // Slots — update filter and/or reload the game list from the active database.
    // NOLINTNEXTLINE(readability-redundant-access-specifiers)
    Q_SLOT void set_player_filter(QString const& name);
    // NOLINTNEXTLINE(readability-redundant-access-specifiers)
    Q_SLOT void set_result_filter(QString const& result);
    // NOLINTNEXTLINE(readability-redundant-access-specifiers)
    Q_SLOT void refresh();

  signals:
    void total_count_changed(qint64 total);
    void error_occurred(QString const& message);

  private:
    database_workspace* workspace_ {nullptr};
    motif::db::search_filter filter_;
    std::vector<motif::db::game_list_entry> entries_;
    qint64 total_count_ {0};
    QTimer* filter_timer_ {nullptr};

    void reload();
    void append_page(std::size_t offset);
};

}  // namespace motif::app
