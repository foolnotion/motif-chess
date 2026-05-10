import QtQuick
import QtQuick.Controls

// Displays the game's main-line moves as a scrollable notation list.
// Current move is highlighted. Click a move to jump to that ply.
// Variations are not stored in the database (import pipeline stores main line only);
// this component is architected to support branching when variation storage is added.
Item {
    id: root

    property color text_color: "#d4d4d4"
    property color active_color: "#4fc3f7"
    property color number_color: "#777777"
    property color bg_color: "#1e1e1e"

    Rectangle {
        anchors.fill: parent
        color: bg_color

        ListView {
            id: moves_view
            anchors {
                fill: parent
                margins: 6
            }
            clip: true
            spacing: 2

            // Each row covers one full move (white + black half-moves).
            model: board ? Math.ceil(board.move_list.length / 2) : 0

            delegate: Row {
                id: move_row
                spacing: 4
                height: 22

                property int move_num: index + 1
                property int white_ply: index * 2      // 0-based index into move_list
                property int black_ply: index * 2 + 1
                property var moves: board ? board.move_list : []
                property int current: board ? board.current_ply : 0

                // Move number
                Text {
                    text: move_row.move_num + "."
                    color: root.number_color
                    font.pixelSize: 13
                    verticalAlignment: Text.AlignVCenter
                    height: move_row.height
                }

                // White half-move
                Rectangle {
                    visible: move_row.white_ply < move_row.moves.length
                    color: move_row.current === move_row.white_ply + 1
                           ? Qt.rgba(0.3, 0.76, 0.97, 0.25)
                           : "transparent"
                    radius: 3
                    height: move_row.height
                    width: white_text.implicitWidth + 8

                    Text {
                        id: white_text
                        anchors.centerIn: parent
                        text: move_row.white_ply < move_row.moves.length
                              ? move_row.moves[move_row.white_ply]
                              : ""
                        color: move_row.current === move_row.white_ply + 1
                               ? root.active_color
                               : root.text_color
                        font.pixelSize: 13
                        font.bold: move_row.current === move_row.white_ply + 1
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (board) { board.navigate_to(move_row.white_ply + 1) }
                        }
                    }
                }

                // Black half-move
                Rectangle {
                    visible: move_row.black_ply < move_row.moves.length
                    color: move_row.current === move_row.black_ply + 1
                           ? Qt.rgba(0.3, 0.76, 0.97, 0.25)
                           : "transparent"
                    radius: 3
                    height: move_row.height
                    width: black_text.implicitWidth + 8

                    Text {
                        id: black_text
                        anchors.centerIn: parent
                        text: move_row.black_ply < move_row.moves.length
                              ? move_row.moves[move_row.black_ply]
                              : ""
                        color: move_row.current === move_row.black_ply + 1
                               ? root.active_color
                               : root.text_color
                        font.pixelSize: 13
                        font.bold: move_row.current === move_row.black_ply + 1
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (board) { board.navigate_to(move_row.black_ply + 1) }
                        }
                    }
                }
            }

            ScrollBar.vertical: ScrollBar {}
        }
    }
}
