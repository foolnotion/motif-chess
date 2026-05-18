import QtQuick
// TitleBarBase lives in the KDDW QRC, not as a registered com.kdab.dockwidgets type.
import "qrc:/kddockwidgets/qtquick/views/qml/" as KDDW

KDDW.TitleBarBase {
    id: root

    SystemPalette { id: sysPalette }

    heightWhenVisible: 24
    color: sysPalette.button

    component TitleBarBtn: Rectangle {
        id: btn
        signal clicked()
        property string label: ""

        width: 20
        height: 20
        color: "transparent"
        radius: 3
        border {
            color: sysPalette.mid
            width: btn_mouse.containsMouse ? 1 : 0
        }

        Text {
            anchors.centerIn: parent
            text: btn.label
            color: sysPalette.buttonText
            font.pointSize: 8
        }

        MouseArea {
            id: btn_mouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: btn.clicked()
        }
    }

    Text {
        id: title_text
        text: root.title
        font.pointSize: 9
        color: sysPalette.buttonText
        anchors {
            left: parent.left
            leftMargin: 6
            verticalCenter: parent.verticalCenter
        }
        elide: Text.ElideRight
        width: parent.width - btn_row.width - 14
    }

    Row {
        id: btn_row
        anchors {
            verticalCenter: parent.verticalCenter
            right: parent.right
            rightMargin: 2
        }
        spacing: 1

        TitleBarBtn {
            visible: root.minimizeButtonVisible
            label: "─"
            onClicked: root.minimizeButtonClicked()
        }

        TitleBarBtn {
            visible: root.floatButtonVisible
            label: "⊡"
            onClicked: root.floatButtonClicked()
        }

        TitleBarBtn {
            visible: root.maximizeButtonVisible
            label: root.maximizeUsesRestoreIcon ? "⊡" : "□"
            onClicked: root.maximizeButtonClicked()
        }

        TitleBarBtn {
            enabled: root.closeButtonEnabled
            label: "✕"
            onClicked: root.closeButtonClicked()
        }
    }
}
