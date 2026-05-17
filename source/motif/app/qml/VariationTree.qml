import QtQuick
import QtQuick.Controls

// Displays the game's main-line moves as inline paragraph-style notation.
// Moves wrap naturally like text. Current move is highlighted.
// Click any move token to jump to that ply.
Item {
    id: root

    property color text_color: "#d4d4d4"
    property color active_color: "#4fc3f7"
    property color number_color: "#777777"
    property color bg_color: "#1e1e1e"

    Rectangle {
        anchors.fill: parent
        color: bg_color

        Flickable {
            id: flick
            anchors {
                fill: parent
                margins: 8
            }
            clip: true
            contentWidth: width
            contentHeight: notation_flow.implicitHeight

            ScrollBar.vertical: ScrollBar {}

            Flow {
                id: notation_flow
                width: flick.width
                spacing: 4

                Repeater {
                    model: board ? board.move_list.length : 0

                    // Each delegate is an atomic unit: [number.]? + move.
                    // Keeping them in a Row prevents wrapping between the
                    // move number and the white half-move.
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
        }
    }
}
