import QtQuick
import QtQuick.Controls

// Displays game header (players, event, date) and main-line moves as
// inline paragraph-style notation. Current move is highlighted.
// Click any move token to jump to that ply.
Item {
    id: root

    property color text_color: "#d4d4d4"
    property color active_color: "#4fc3f7"
    property color number_color: "#777777"
    property color meta_color: "#999999"
    property color bg_color: "#1e1e1e"

    Rectangle {
        anchors.fill: parent
        color: bg_color

        Column {
            anchors {
                fill: parent
                margins: 8
            }
            spacing: 6

            // ── Header ───────────────────────────────────────────────────
            Column {
                visible: board && board.game_loaded
                width: parent.width
                spacing: 2

                Text {
                    width: parent.width
                    text: board ? (board.white_name + " – " + board.black_name) : ""
                    color: root.text_color
                    font.pixelSize: 13
                    font.bold: true
                    elide: Text.ElideRight
                }

                Text {
                    visible: text.length > 0
                    width: parent.width
                    text: {
                        if (!board) { return "" }
                        var parts = []
                        if (board.event_name) { parts.push(board.event_name) }
                        if (board.game_date)  { parts.push(board.game_date) }
                        return parts.join("  ·  ")
                    }
                    color: root.meta_color
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }

                Rectangle { width: parent.width; height: 1; color: "#333333" }
            }

            // ── Moves ─────────────────────────────────────────────────────
            Flickable {
                id: flick
                width: parent.width
                height: parent.height - (board && board.game_loaded ? header_col.implicitHeight + 6 : 0)
                clip: true
                contentWidth: width
                contentHeight: notation_flow.implicitHeight + (result_text.visible ? result_text.implicitHeight + 4 : 0)

                ScrollBar.vertical: ScrollBar {}

                Flow {
                    id: notation_flow
                    width: flick.width
                    spacing: 4

                    Repeater {
                        model: board ? board.move_list.length : 0

                        // Atomic unit: [number.]? + move token.
                        // Row prevents wrapping between move number and white half-move.
                        delegate: Row {
                            spacing: 2

                            Text {
                                visible: index % 2 === 0
                                text: (Math.floor(index / 2) + 1) + "."
                                color: root.number_color
                                font.pixelSize: 13
                                anchors.verticalCenter: parent.verticalCenter
                            }

                            Rectangle {
                                property bool is_current: board && board.current_ply === index + 1
                                color: is_current ? Qt.rgba(0.3, 0.76, 0.97, 0.2) : "transparent"
                                radius: 3
                                height: move_text.implicitHeight + 4
                                width: move_text.implicitWidth + 8

                                Text {
                                    id: move_text
                                    anchors.centerIn: parent
                                    text: board ? board.move_list[index] : ""
                                    color: parent.is_current ? root.active_color : root.text_color
                                    font.pixelSize: 13
                                    font.bold: parent.is_current
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (board) { board.navigate_to(index + 1) }
                                    }
                                }
                            }
                        }
                    }
                }

                Text {
                    id: result_text
                    visible: board && board.game_loaded && board.game_result.length > 0
                    anchors.top: notation_flow.bottom
                    anchors.topMargin: 4
                    text: board ? board.game_result : ""
                    color: root.meta_color
                    font.pixelSize: 13
                    font.bold: true
                }
            }
        }
    }

    // Invisible item used only to measure header height for Flickable sizing.
    Column {
        id: header_col
        visible: false
        spacing: 2
        Text { font.pixelSize: 13; font.bold: true; text: "x" }
        Text { font.pixelSize: 11; text: "x" }
        Rectangle { width: 1; height: 1 }
    }
}
