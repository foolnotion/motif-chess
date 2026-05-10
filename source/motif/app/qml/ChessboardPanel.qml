import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    // Returns the FEN piece character at (file, rank): K/Q/R/B/N/P for white,
    // k/q/r/b/n/p for black, "" for empty. More efficient than string expansion.
    function piece_char(fen, file, rank) {
        if (!fen) { return "" }
        var placement = fen.split(" ")[0]
        var rows = placement.split("/")
        var row_idx = 7 - rank
        if (row_idx < 0 || row_idx >= rows.length) { return "" }
        var rank_str = rows[row_idx]
        var col = 0
        for (var i = 0; i < rank_str.length; i++) {
            var ch = rank_str[i]
            if (ch >= '1' && ch <= '8') {
                col += parseInt(ch)
            } else {
                if (col === file) { return ch }
                col++
            }
        }
        return ""
    }

    // Returns the resource path for a piece SVG, or "" for an empty square.
    function piece_source(fen, file, rank) {
        var ch = piece_char(fen, file, rank)
        if (!ch) { return "" }
        var color = (ch === ch.toUpperCase()) ? "w" : "b"
        return Qt.resolvedUrl("pieces/" + color + ch.toUpperCase() + ".svg")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Board area — fills all space above the navigation bar
        Item {
            id: board_area
            Layout.fillWidth: true
            Layout.fillHeight: true

            property real label_size: 18
            property real board_size: Math.min(width - label_size, height - label_size)
            property real sq_size: board_size / 8

            // Rank labels (8..1) on the left
            Column {
                anchors.left: parent.left
                anchors.top: board_grid_area.top
                width: board_area.label_size
                Repeater {
                    model: 8
                    delegate: Item {
                        width: board_area.label_size
                        height: board_area.sq_size
                        Text {
                            anchors.centerIn: parent
                            text: (8 - index).toString()
                            color: "#aaaaaa"
                            font.pixelSize: 11
                        }
                    }
                }
            }

            // 8×8 board
            Item {
                id: board_grid_area
                anchors.left: parent.left
                anchors.leftMargin: board_area.label_size
                anchors.top: parent.top
                width: board_area.board_size
                height: board_area.board_size

                Grid {
                    columns: 8
                    anchors.fill: parent

                    Repeater {
                        model: 64
                        delegate: Rectangle {
                            id: square
                            property int sq_row: Math.floor(index / 8)
                            property int sq_col: index % 8
                            property int file: sq_col
                            property int rank: 7 - sq_row

                            width: board_area.sq_size
                            height: board_area.sq_size
                            color: (sq_row + sq_col) % 2 === 0 ? "#f0d9b5" : "#b58863"

                            Image {
                                anchors.centerIn: parent
                                width: board_area.sq_size * 0.9
                                height: board_area.sq_size * 0.9
                                sourceSize: Qt.size(board_area.sq_size * 0.9, board_area.sq_size * 0.9)
                                antialiasing: true
                                source: {
                                    var fen = board && board.game_loaded
                                        ? board.fen
                                        : "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
                                    return root.piece_source(fen, square.file, square.rank)
                                }
                            }
                        }
                    }
                }
            }

            // File labels (a..h) below the board
            Row {
                anchors.left: board_grid_area.left
                anchors.top: board_grid_area.bottom
                height: board_area.label_size
                Repeater {
                    model: 8
                    delegate: Item {
                        width: board_area.sq_size
                        height: board_area.label_size
                        Text {
                            anchors.centerIn: parent
                            text: String.fromCharCode(97 + index)  // 'a'..'h'
                            color: "#aaaaaa"
                            font.pixelSize: 11
                        }
                    }
                }
            }
        }

        // Navigation toolbar
        ToolBar {
            Layout.fillWidth: true

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 4
                spacing: 2

                ToolButton {
                    text: "⏮"
                    enabled: board && board.game_loaded
                    onClicked: board.jump_to_start()
                    ToolTip.text: "Start (Home)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                }
                ToolButton {
                    text: "◀"
                    enabled: board && board.game_loaded
                    onClicked: board.retreat()
                    ToolTip.text: "Previous move (←)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                }
                ToolButton {
                    text: "▶"
                    enabled: board && board.game_loaded
                    onClicked: board.advance()
                    ToolTip.text: "Next move (→)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                }
                ToolButton {
                    text: "⏭"
                    enabled: board && board.game_loaded
                    onClicked: board.jump_to_end()
                    ToolTip.text: "End (End)"
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                }

                Item { Layout.fillWidth: true }
            }
        }
    }
}
