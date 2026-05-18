import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ToolBar {
            Layout.fillWidth: true

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 6

                Label {
                    text: opening_stats_model.total_games > 0
                          ? opening_stats_model.total_games + " games"
                          : ""
                    color: palette.mid
                    font.pixelSize: 11
                }

                Item { Layout.fillWidth: true }

                BusyIndicator {
                    running: opening_stats_model.searching
                    visible: running
                    Layout.preferredHeight: 18
                    Layout.preferredWidth: 18
                }
            }
        }

        // Column header
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 20
            color: palette.window

            Row {
                anchors.fill: parent
                anchors.leftMargin: 8

                Label {
                    width: 46
                    height: parent.height
                    text: "Move"
                    font.pixelSize: 10
                    font.bold: true
                    color: palette.mid
                    verticalAlignment: Text.AlignVCenter
                }
                Label {
                    width: 52
                    height: parent.height
                    text: "Games"
                    font.pixelSize: 10
                    font.bold: true
                    color: palette.mid
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                }
                Label {
                    width: 40
                    height: parent.height
                    text: "W%"
                    font.pixelSize: 10
                    font.bold: true
                    color: palette.mid
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                }
                Label {
                    width: 40
                    height: parent.height
                    text: "D%"
                    font.pixelSize: 10
                    font.bold: true
                    color: palette.mid
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                }
                Label {
                    width: 40
                    height: parent.height
                    text: "L%"
                    font.pixelSize: 10
                    font.bold: true
                    color: palette.mid
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: palette.mid
            opacity: 0.3
        }

        ListView {
            id: stats_list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: opening_stats_model

            delegate: Rectangle {
                width: stats_list.width
                height: (model.eco || model.opening_name) ? 34 : 24
                color: index % 2 === 0 ? palette.base : palette.alternateBase

                Column {
                    anchors.fill: parent
                    anchors.leftMargin: 8

                    Row {
                        height: 24
                        spacing: 0

                        Label {
                            width: 46
                            height: parent.height
                            text: model.san
                            font.bold: true
                            font.pixelSize: 12
                            verticalAlignment: Text.AlignVCenter
                            color: palette.text
                        }

                        Label {
                            width: 52
                            height: parent.height
                            text: model.frequency
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                            color: palette.text
                        }

                        Label {
                            width: 40
                            height: parent.height
                            text: model.frequency > 0
                                  ? Math.round(model.white_wins / model.frequency * 100) + "%"
                                  : "–"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                            color: palette.text
                        }

                        Label {
                            width: 40
                            height: parent.height
                            text: model.frequency > 0
                                  ? Math.round(model.draws / model.frequency * 100) + "%"
                                  : "–"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                            color: palette.text
                        }

                        Label {
                            width: 40
                            height: parent.height
                            text: model.frequency > 0
                                  ? Math.round(model.black_wins / model.frequency * 100) + "%"
                                  : "–"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                            color: palette.text
                        }
                    }

                    Label {
                        visible: model.eco || model.opening_name
                        width: parent.width - 8
                        height: 10
                        font.pixelSize: 9
                        color: palette.mid
                        elide: Text.ElideRight
                        text: {
                            var parts = []
                            if (model.eco) parts.push(model.eco)
                            if (model.opening_name) parts.push(model.opening_name)
                            return parts.join(" — ")
                        }
                    }
                }
            }

            Label {
                visible: stats_list.count === 0
                anchors.fill: parent
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: workspace && workspace.has_active
                      ? "No continuations found for this position"
                      : "Open a database to view opening stats"
                color: palette.mid
                font.italic: true
            }
        }
    }
}
