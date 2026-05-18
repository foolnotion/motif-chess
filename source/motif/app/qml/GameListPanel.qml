import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Game list panel: filterable, keyboard-navigable list of games in the active database.
// Context properties used: game_list (game_list_model), board (board_model),
//                          position_search (position_search_model).
Item {
    id: root

    signal game_activated(int game_id)

    // ── Non-visual helpers ────────────────────────────────────────────────────

    FileDialog {
        id: pgn_file_dialog
        title: "Select PGN File"
        nameFilters: ["PGN files (*.pgn)", "All files (*)"]
        onAccepted: workspace.import_pgn(selectedFile.toString().replace("file://", ""))
    }

    Connections {
        target: game_list
        function onError_occurred(message) {
            error_dialog.error_text = message
            error_dialog.open()
        }
    }

    Connections {
        target: workspace
        function onImport_finished(committed, errors) {
            game_list.refresh()
            import_toast.committed = committed
            import_toast.errors = errors
            import_toast.visible = true
            import_toast_timer.restart()
        }
    }

    // ── Main layout ───────────────────────────────────────────────────────────

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TabBar {
            id: tab_bar
            Layout.fillWidth: true
            TabButton { text: "Games" }
            TabButton { text: "Matching" }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tab_bar.currentIndex

            // ── Tab 0: All games ──────────────────────────────────────────────
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    ToolBar {
                        Layout.fillWidth: true

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 6
                            spacing: 6

                            TextField {
                                id: player_filter
                                Layout.fillWidth: true
                                placeholderText: "Filter by player name…"
                                onTextChanged: game_list.set_player_filter(text)
                                KeyNavigation.tab: result_combo
                                KeyNavigation.down: table_view
                                activeFocusOnTab: true
                            }

                            ComboBox {
                                id: result_combo
                                model: ["All results", "1-0", "0-1", "1/2-1/2"]
                                implicitWidth: 120
                                onCurrentIndexChanged: {
                                    game_list.set_result_filter(currentIndex > 0 ? currentText : "")
                                }
                                KeyNavigation.tab: table_view
                                activeFocusOnTab: true
                            }

                            Label {
                                text: game_list.total_count + " games"
                                color: palette.mid
                            }

                            ToolButton {
                                text: "Import PGN…"
                                enabled: !workspace.is_importing
                                onClicked: pgn_file_dialog.open()
                                ToolTip.text: workspace.is_importing ? "Import in progress…" : "Import a PGN file into this database"
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                            }
                        }
                    }

                    // Import progress — shown while a PGN import is running.
                    Rectangle {
                        Layout.fillWidth: true
                        visible: workspace.is_importing
                        color: palette.window
                        implicitHeight: import_col.implicitHeight + 16

                        ColumnLayout {
                            id: import_col
                            anchors {
                                left: parent.left; right: parent.right
                                verticalCenter: parent.verticalCenter
                                leftMargin: 10; rightMargin: 10
                            }
                            spacing: 4

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: workspace.import_phase_text
                                    font.bold: true
                                }
                                Item { Layout.fillWidth: true }
                                Label {
                                    text: workspace.import_total > 0
                                        ? workspace.import_processed + " / " + workspace.import_total + " games"
                                        : workspace.import_processed + " games"
                                    color: palette.mid
                                    font.pointSize: 9
                                }
                            }

                            ProgressBar {
                                Layout.fillWidth: true
                                indeterminate: workspace.import_total === 0
                                from: 0; to: 1
                                value: workspace.import_total > 0
                                    ? workspace.import_processed / workspace.import_total
                                    : 0
                            }
                        }
                    }

                    HorizontalHeaderView {
                        id: header_view
                        syncView: table_view
                        Layout.fillWidth: true
                        clip: true
                    }

                    TableView {
                        id: table_view
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: game_list

                        selectionBehavior: TableView.SelectRows
                        selectionModel: ItemSelectionModel {
                            id: selection_model
                            model: game_list
                        }

                        activeFocusOnTab: true
                        focus: true

                        columnWidthProvider: function (column) {
                            switch (column) {
                            case 0: return 150  // White
                            case 1: return 150  // Black
                            case 2: return 60   // Result
                            case 3: return 180  // Event
                            case 4: return 75   // Date
                            case 5: return 46   // ECO
                            default: return 100
                            }
                        }

                        delegate: Rectangle {
                            id: cell_rect
                            required property bool selected
                            required property int row
                            required property int column
                            required property var display

                            implicitHeight: 24
                            color: selected
                                ? palette.highlight
                                : (row % 2 === 0 ? palette.base : palette.alternateBase)

                            Label {
                                anchors.fill: parent
                                leftPadding: 4
                                rightPadding: 2
                                verticalAlignment: Text.AlignVCenter
                                text: cell_rect.display ?? ""
                                color: cell_rect.selected ? palette.highlightedText : palette.text
                                elide: Text.ElideRight
                            }

                            TapHandler {
                                acceptedButtons: Qt.LeftButton
                                onDoubleTapped: {
                                    var gid = game_list.game_id_at(cell_rect.row)
                                    if (gid > 0) { root.game_activated(gid) }
                                }
                                onSingleTapped: {
                                    selection_model.setCurrentIndex(
                                        game_list.index(cell_rect.row, 0),
                                        ItemSelectionModel.ClearAndSelect | ItemSelectionModel.Rows)
                                }
                            }
                        }

                        Keys.onReturnPressed: load_selected()
                        Keys.onEnterPressed:  load_selected()

                        function load_selected() {
                            var idx = selection_model.currentIndex
                            if (!idx.valid) { return }
                            var gid = game_list.game_id_at(idx.row)
                            if (gid > 0) { root.game_activated(gid) }
                        }
                    }
                }
            }

            // ── Tab 1: Matching games (position search) ───────────────────────
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    ToolBar {
                        Layout.fillWidth: true

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 6
                            spacing: 6

                            Label {
                                text: position_search.total_count + " matching games"
                                color: palette.mid
                            }

                            Item { Layout.fillWidth: true }

                            BusyIndicator {
                                running: position_search.searching
                                visible: running
                                Layout.preferredHeight: 20
                                Layout.preferredWidth: 20
                            }
                        }
                    }

                    HorizontalHeaderView {
                        id: match_header
                        syncView: match_table
                        Layout.fillWidth: true
                        clip: true
                    }

                    TableView {
                        id: match_table
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: position_search

                        columnWidthProvider: function (column) {
                            switch (column) {
                            case 0: return 120  // White
                            case 1: return 120  // Black
                            case 2: return 55   // Result
                            case 3: return 50   // Move
                            case 4: return 65   // WhiteElo
                            case 5: return 65   // BlackElo
                            default: return 80
                            }
                        }

                        delegate: Rectangle {
                            id: match_cell
                            required property bool selected
                            required property int row
                            required property int column
                            required property var display

                            implicitHeight: 22
                            color: selected
                                ? palette.highlight
                                : (row % 2 === 0 ? palette.base : palette.alternateBase)

                            Label {
                                anchors.fill: parent
                                leftPadding: 4
                                rightPadding: 2
                                verticalAlignment: Text.AlignVCenter
                                text: match_cell.display ?? ""
                                color: match_cell.selected ? palette.highlightedText : palette.text
                                elide: Text.ElideRight
                                font.pixelSize: 11
                            }

                            TapHandler {
                                acceptedButtons: Qt.LeftButton
                                onDoubleTapped: {
                                    var gid = position_search.game_id_at(match_cell.row)
                                    var ply = position_search.ply_at(match_cell.row)
                                    if (gid > 0) {
                                        board.load_game(gid)
                                        board.navigate_to(ply)
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        visible: match_table.rows === 0
                        Layout.fillWidth: true
                        Layout.topMargin: 12
                        horizontalAlignment: Text.AlignHCenter
                        text: workspace && workspace.has_active
                              ? "No matching games found for this position"
                              : "Open a database to search positions"
                        color: palette.mid
                        font.italic: true
                    }
                }
            }
        }
    }

    // ── Overlay items (not in layout) ─────────────────────────────────────────

    Rectangle {
        id: import_toast
        property int committed: 0
        property int errors: 0
        visible: false
        z: 5
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 8
        radius: 6
        color: palette.highlight
        implicitWidth: toast_label.implicitWidth + 24
        implicitHeight: toast_label.implicitHeight + 12

        Label {
            id: toast_label
            anchors.centerIn: parent
            text: "Imported " + import_toast.committed + " games" +
                  (import_toast.errors > 0 ? " (" + import_toast.errors + " errors)" : "")
            color: palette.highlightedText
        }

        Timer {
            id: import_toast_timer
            interval: 4000
            onTriggered: import_toast.visible = false
        }
    }

    Dialog {
        id: error_dialog
        property string error_text: ""
        title: "Error"
        standardButtons: Dialog.Ok
        anchors.centerIn: parent

        Label {
            text: error_dialog.error_text
            wrapMode: Text.WordWrap
            width: 320
        }
    }
}
