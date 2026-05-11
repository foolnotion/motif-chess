import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Game list panel: filterable, keyboard-navigable list of games in the active database.
// Context properties used: game_list (game_list_model), board (board_model).
Item {
    id: root

    signal game_activated(int game_id)

    Connections {
        target: game_list
        function onError_occurred(message) {
            error_dialog.error_text = message
            error_dialog.open()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Filter bar
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

        FileDialog {
            id: pgn_file_dialog
            title: "Select PGN File"
            nameFilters: ["PGN files (*.pgn)", "All files (*)"]
            onAccepted: workspace.import_pgn(selectedFile.toString().replace("file://", ""))
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

        // Toast notification shown after import completes
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

                // Double-click: load the game into the board
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

            // Enter key loads the currently selected game
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
