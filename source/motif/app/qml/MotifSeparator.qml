import QtQuick
import com.kdab.dockwidgets 2.0

Rectangle {
    anchors.fill: parent
    color: palette.mid

    readonly property SeparatorView kddwSeparator: parent // qmllint disable incompatible-type

    MouseArea {
        cursorShape: kddwSeparator
            ? (kddwSeparator.isVertical ? Qt.SizeVerCursor : Qt.SizeHorCursor)
            : Qt.SizeHorCursor
        anchors.fill: parent
        onPressed:          kddwSeparator.onMousePressed()
        onReleased:         kddwSeparator.onMouseReleased()
        onPositionChanged:  mouse => kddwSeparator.onMouseMoved(Qt.point(mouse.x, mouse.y))
        onDoubleClicked:    kddwSeparator.onMouseDoubleClicked()
    }
}
