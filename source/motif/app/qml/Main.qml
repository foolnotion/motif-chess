import QtQuick
import QtQuick.Controls
import com.kdab.dockwidgets 2.0 as KDDW

ApplicationWindow {
    id: root
    title: "motif-chess"
    width: 1280
    height: 800
    visible: true

    // Global keyboard navigation — arrow keys advance/retreat the board regardless of focus.
    Keys.onPressed: function(event) {
        if (!board || !board.game_loaded) { return }
        switch (event.key) {
        case Qt.Key_Right:
            board.advance()
            event.accepted = true
            break
        case Qt.Key_Left:
            board.retreat()
            event.accepted = true
            break
        case Qt.Key_Home:
            board.jump_to_start()
            event.accepted = true
            break
        case Qt.Key_End:
            board.jump_to_end()
            event.accepted = true
            break
        }
    }

    // Status bar label showing active database
    footer: ToolBar {
        visible: workspace.has_active
        Label {
            anchors.verticalCenter: parent.verticalCenter
            leftPadding: 8
            text: workspace.is_temporary
                  ? "⚡ " + workspace.display_name + " (temporary)"
                  : "● " + workspace.display_name + "  —  " + workspace.active_path
            font.pointSize: 9
        }
    }

    // Main docking area — shown once a database is active.
    KDDW.DockingArea {
        id: main_dock_area
        objectName: "main_dock_area"
        uniqueName: "main_dock_area"
        anchors.fill: parent
        visible: workspace.has_active

        KDDW.DockWidget {
            id: dock_chessboard
            uniqueName: "chessboard"
            title: "Board"
            ChessboardPanel {
                anchors.fill: parent
            }
        }

        KDDW.DockWidget {
            id: dock_game_list
            uniqueName: "game_list"
            title: "Games"
            Rectangle { anchors.fill: parent; color: "#2a2a2a"
                Label { anchors.centerIn: parent; text: "Game List"; color: "white" } }
        }

        KDDW.DockWidget {
            id: dock_opening_explorer
            uniqueName: "opening_explorer"
            title: "Opening Explorer"
            Rectangle { anchors.fill: parent; color: "#2a2a2a"
                Label { anchors.centerIn: parent; text: "Opening Explorer"; color: "white" } }
        }

        KDDW.DockWidget {
            id: dock_notation
            uniqueName: "notation"
            title: "Notation"
            VariationTree {
                anchors.fill: parent
            }
        }

        KDDW.DockWidget {
            id: dock_engine
            uniqueName: "engine_analysis"
            title: "Engine"
            Rectangle { anchors.fill: parent; color: "#2a2a2a"
                Label { anchors.centerIn: parent; text: "Engine Analysis"; color: "white" } }
        }

        // Layout matching the spec:
        //   +--Board--------+--Game List---------+
        //   |               |--Notation----------|
        //   +--Explorer-----+--Engine Analysis---+
        Component.onCompleted: {
            // Chessboard takes the left side
            addDockWidget(dock_chessboard, KDDW.KDDockWidgets.Location_OnLeft);
            // Game list fills the right, top
            addDockWidget(dock_game_list, KDDW.KDDockWidgets.Location_OnRight, dock_chessboard);
            // Opening explorer below chessboard
            addDockWidget(dock_opening_explorer, KDDW.KDDockWidgets.Location_OnBottom, dock_chessboard, Qt.size(0, 280));
            // Notation below game list
            addDockWidget(dock_notation, KDDW.KDDockWidgets.Location_OnBottom, dock_game_list, Qt.size(0, 220));
            // Engine analysis below notation
            addDockWidget(dock_engine, KDDW.KDDockWidgets.Location_OnBottom, dock_notation, Qt.size(0, 200));
        }
    }

    // Workspace start screen — shown when no database is active.
    WorkspaceOverlay {
        id: workspace_overlay
        anchors.fill: parent
        visible: !workspace.has_active

        onDatabase_activated: {
            // overlay hides automatically via visible binding
        }
    }
}
