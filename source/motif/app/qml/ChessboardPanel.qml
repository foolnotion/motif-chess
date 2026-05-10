import QtQuick
import QtQuick.Controls

Item {
    id: root

    // Piece symbol lookup from FEN string.
    // file: 0=a..7=h, rank: 0=rank1..7=rank8
    function piece_symbol(fen, file, rank) {
        if (!fen) { return "" }
        var placement = fen.split(" ")[0]
        var rows = placement.split("/")
        var row_idx = 7 - rank   // FEN row 0 = rank 8
        if (row_idx < 0 || row_idx >= rows.length) { return "" }
        var rank_str = rows[row_idx]
        var expanded = ""
        for (var i = 0; i < rank_str.length; i++) {
            var ch = rank_str[i]
            if (ch >= '1' && ch <= '8') {
                for (var j = 0; j < parseInt(ch); j++) { expanded += "." }
            } else {
                expanded += ch
            }
        }
        if (file >= expanded.length) { return "" }
        var piece = expanded[file]
        if (piece === ".") { return "" }
        var table = {
            'K': '♔', 'Q': '♕', 'R': '♖',
            'B': '♗', 'N': '♘', 'P': '♙',
            'k': '♚', 'q': '♛', 'r': '♜',
            'b': '♝', 'n': '♞', 'p': '♟'
        }
        return table[piece] || ""
    }

    function is_white_piece(piece_ch) {
        return piece_ch >= '♔' && piece_ch <= '♙'
    }

    // Board occupies a square region centred vertically, leaving room for rank/file labels.
    property real label_size: 18
    property real board_size: Math.min(width - label_size, height - label_size)
    property real sq_size: board_size / 8

    // Rank labels (8..1) on the left
    Column {
        id: rank_labels
        anchors.left: parent.left
        anchors.top: board_grid_area.top
        width: root.label_size
        Repeater {
            model: 8
            delegate: Item {
                width: root.label_size
                height: root.sq_size
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
        anchors.left: rank_labels.right
        anchors.top: parent.top
        width: root.board_size
        height: root.board_size

        Grid {
            id: board_grid
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

                    width: root.sq_size
                    height: root.sq_size
                    color: (sq_row + sq_col) % 2 === 0 ? "#f0d9b5" : "#b58863"

                    Text {
                        id: piece_text
                        anchors.centerIn: parent
                        property string sym: board && board.game_loaded
                            ? root.piece_symbol(board.fen, square.file, square.rank)
                            : root.piece_symbol("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                                                square.file, square.rank)
                        text: sym
                        // White pieces rendered light; black pieces dark — both visible on either square colour.
                        color: root.is_white_piece(sym) ? "#fffff0" : "#1c1c1c"
                        style: Text.Outline
                        styleColor: root.is_white_piece(sym) ? "#3a3a3a" : "#e8e8e8"
                        font.pixelSize: root.sq_size * 0.78
                        renderType: Text.NativeRendering
                    }
                }
            }
        }
    }

    // File labels (a..h) below the board
    Row {
        id: file_labels
        anchors.left: board_grid_area.left
        anchors.top: board_grid_area.bottom
        height: root.label_size
        Repeater {
            model: 8
            delegate: Item {
                width: root.sq_size
                height: root.label_size
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
