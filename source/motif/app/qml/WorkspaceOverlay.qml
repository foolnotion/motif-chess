import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Item {
    id: root

    function folder_url_to_path(folder_url) {
        if (folder_url === undefined || folder_url === null) {
            return ""
        }
        if (typeof folder_url.toLocalFile === "function") {
            return folder_url.toLocalFile()
        }
        var text = folder_url.toString()
        if (text.startsWith("file://")) {
            return decodeURIComponent(text.substring(7))
        }
        return text
    }

    // Emitted when any database activation succeeds.
    signal database_activated()

    Rectangle {
        anchors.fill: parent
        color: palette.window
    }

    // Loading overlay — shown while open_database runs on the background thread.
    Rectangle {
        anchors.fill: parent
        color: palette.window
        visible: workspace.is_loading
        z: 10

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 16
            width: Math.min(380, parent.width - 64)

            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: workspace.is_loading
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "Opening " + workspace.loading_name + "…"
                font.pointSize: 11
            }

            ProgressBar {
                Layout.fillWidth: true
                // indeterminate — DuckDB open has no intermediate progress
                from: 0
                to: 1
                value: 0
                indeterminate: true
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                visible: workspace.loading_total_games > 0
                text: Number(workspace.loading_total_games).toLocaleString(Qt.locale(), 'f', 0) + " games"
                font.pointSize: 9
                color: palette.mid
            }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 16
        width: Math.min(480, parent.width - 64)

        Label {
            text: "motif-chess"
            font.pointSize: 24
            font.bold: true
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            text: "Choose a database to get started"
            font.pointSize: 11
            color: palette.mid
            Layout.alignment: Qt.AlignHCenter
        }

        Item { implicitHeight: 8 }

        Button {
            text: "Create Database…"
            Layout.fillWidth: true
            onClicked: create_dir_dialog.open()
        }

        Button {
            text: "Open Database…"
            Layout.fillWidth: true
            onClicked: open_dir_dialog.open()
        }

        Button {
            text: "Use Scratch Database"
            Layout.fillWidth: true
            onClicked: workspace.open_scratch()
        }

        Item { implicitHeight: 8 }

        Label {
            text: "Recent Databases"
            font.bold: true
            visible: workspace.recent_databases.length > 0
        }

        ListView {
            id: recent_list
            Layout.fillWidth: true
            implicitHeight: Math.min(contentHeight, 220)
            clip: true
            visible: workspace.recent_databases.length > 0
            model: workspace.recent_databases

            delegate: ItemDelegate {
                width: recent_list.width
                enabled: modelData.available

                contentItem: ColumnLayout {
                    spacing: 2
                    Label {
                        text: modelData.name
                        font.bold: true
                        opacity: modelData.available ? 1.0 : 0.4
                    }
                    Label {
                        text: modelData.path
                        font.pointSize: 9
                        elide: Text.ElideLeft
                        opacity: modelData.available ? 0.7 : 0.3
                    }
                    Label {
                        text: "(unavailable)"
                        font.pointSize: 9
                        color: "red"
                        visible: !modelData.available
                    }
                }

                onClicked: workspace.open_database(modelData.path)

                // Right-click to remove
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: remove_menu.popup()
                }

                Menu {
                    id: remove_menu
                    MenuItem {
                        text: "Remove from recent list"
                        onTriggered: workspace.remove_recent(modelData.path)
                    }
                }
            }
        }

        Label {
            id: pgn_notice
            visible: workspace.has_queued_pgn
            text: workspace.queued_pgn_count + " PGN file(s) queued — choose a database first"
            color: palette.mid
            font.pointSize: 9
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }

    // Create: choose parent dir, then enter name
    FolderDialog {
        id: create_dir_dialog
        title: "Choose Parent Directory for New Database"
        onAccepted: {
            name_dialog.parent_dir = root.folder_url_to_path(selectedFolder)
            name_dialog.open()
        }
    }

    Dialog {
        id: name_dialog
        property string parent_dir: ""
        title: "New Database Name"
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: parent

        ColumnLayout {
            Label { text: "Database name:" }
            TextField {
                id: name_field
                Layout.preferredWidth: 300
                placeholderText: "e.g. my-games"
            }
        }

        onAccepted: {
            var trimmed = name_field.text.trim()
            if (trimmed.length === 0) return
            var bundle_path = name_dialog.parent_dir + "/" + trimmed
            workspace.create_database(bundle_path, trimmed)
        }
        onClosed: name_field.clear()
    }

    // Open: pick existing bundle directory
    FolderDialog {
        id: open_dir_dialog
        title: "Open Database Bundle"
        onAccepted: workspace.open_database(root.folder_url_to_path(selectedFolder))
    }

    Connections {
        target: workspace
        function onError_occurred(message) {
            error_dialog.error_text = message
            error_dialog.open()
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
