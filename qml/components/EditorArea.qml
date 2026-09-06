pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../js/TimeUtils.js" as Time
import "../utils"

Rectangle {
    id: root
    color: "#1e1f22"
    radius: 10
    clip: true
    border.width: 0

    property var item: null
    property bool showWelcome: true

    readonly property color barBg:       "#1e1f22"
    readonly property color editorBg:    "#1e1f22"
    readonly property color borderColor: "#4b4d4f"
    readonly property color tabBg:       "#45484c"
    readonly property color textBright:  "#e8e8e8"
    readonly property color textMain:    "#bbbbbb"
    readonly property color textMuted:   "#7d7d7d"
    readonly property color hintKey:     "#8b929e"
    readonly property color accentColor: "#4c96d8"
    readonly property color codeColor:   "#a9b7c6"
    readonly property color imageColor:  "#d7a85b"
    readonly property color lineNumberColor: "#606366"

    IconProvider { id: icons }

    function tabTitle() { return item ? item.title : "README.md" }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // 顶部标签栏（加上顶部圆角）
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 35
            color: root.barBg
            topLeftRadius: 10
            topRightRadius: 10
            visible: !root.showWelcome

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 6
                spacing: 4

                Rectangle {
                    Layout.preferredWidth: 240
                    Layout.preferredHeight: 28
                    radius: 4
                    color: root.tabBg

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 8
                        spacing: 8

                        AppIcon {
                            provider: icons
                            kind: item && item.type === "image" ? "image" : "file"
                            tint: item && item.type === "image" ? root.imageColor : root.accentColor
                            size: 12
                        }
                        Label {
                            text: root.tabTitle()
                            color: root.textBright
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        AppIcon { provider: icons; kind: "close"; tint: root.textMuted; size: 11 }
                    }
                    MouseArea { anchors.fill: parent; hoverEnabled: true }
                }

                // Rectangle {
                //     Layout.preferredWidth: 30
                //     Layout.preferredHeight: 28
                //     radius: 4
                //     color: "transparent"
                //     AppIcon { anchors.centerIn: parent; provider: icons; kind: "plus"; tint: root.textMuted; size: 15 }
                //     MouseArea { anchors.fill: parent; hoverEnabled: true }
                // }

                Item { Layout.fillWidth: true }
                AppIcon { provider: icons; kind: "grid"; tint: root.accentColor; size: 12 }
                AppIcon { provider: icons; kind: "chevron-down"; tint: root.textMuted; size: 11 }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Qt.rgba(root.borderColor.r,
                               root.borderColor.g,
                               root.borderColor.b,
                               0.5)
            }
        }

        // 内容区（始终保持底部圆角；如果标签栏隐藏则四个角都是圆角）
        Rectangle {
            id: contentArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.editorBg
            clip: true
            radius: 10

            Column {
                visible: root.showWelcome
                anchors.centerIn: parent
                width: 460
                spacing: 18
                Repeater {
                    model: [ { t: "搜索全部内容", k: "Double Shift" }, { t: "刷新剪贴板", k: "F5" },
                             { t: "最近复制", k: "Ctrl+E" }, { t: "导航栏", k: "Alt+Home" } ]
                    delegate: RowLayout {
                        required property var modelData
                        width: parent.width
                        spacing: 12
                        Label { text: modelData.t; color: root.textMuted; font.pixelSize: 13; Layout.fillWidth: true }
                        Label { text: modelData.k; color: root.hintKey; font.pixelSize: 13 }
                    }
                }
                Label { text: "复制任意内容自动采集，点击左侧条目回填剪贴板"; color: root.textMuted; font.pixelSize: 13 }
            }

            Column {
                visible: !root.showWelcome && item && item.type === "image"
                anchors.fill: parent; anchors.margins: 20; spacing: 10
                RowLayout { width: parent.width; spacing: 8
                    AppIcon { provider: icons; kind: "image"; tint: root.imageColor; size: 14 }
                    Label { text: item ? item.title : ""; color: root.textMain; font.pixelSize: 13; font.bold: true; Layout.fillWidth: true }
                }
                Label { text: item ? Time.displayTime(item.createdAt) : ""; color: root.textMuted; font.pixelSize: 12 }
                Rectangle { width: parent.width; height: 1; color: root.borderColor }
                Item { width: parent.width; height: parent.height - 90
                    Image { anchors.fill: parent; source: item ? "file:///" + item.content.replace(/\\/g, "/") : ""
                        fillMode: Image.PreserveAspectFit; horizontalAlignment: Image.AlignHCenter; verticalAlignment: Image.AlignVCenter }
                }
            }

            RowLayout {
                visible: !root.showWelcome && item && item.type === "text"
                anchors.fill: parent; spacing: 0

                Rectangle {
                    Layout.preferredWidth: 50
                    Layout.fillHeight: true
                    color: root.editorBg
                    clip: true
                    radius: 10
                    topLeftRadius: root.showWelcome ? 10 : 0
                    topRightRadius: root.showWelcome ? 10 : 0

                    ListView {
                        id: lineNumbers
                        anchors.fill: parent
                        anchors.topMargin: 18
                        anchors.bottomMargin: 18
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        clip: true
                        interactive: false
                        model: textArea.text.length > 0 ? textArea.text.split('\n').length : 1
                        delegate: Label {
                            required property int index
                            text: (index + 1).toString()
                            color: root.lineNumberColor
                            font.family: "Consolas"
                            font.pixelSize: 13
                            horizontalAlignment: Text.AlignRight
                            width: parent.width
                            height: 20
                        }
                    }
                }

                Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: root.borderColor }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: root.editorBg
                    clip: true
                    radius: 10

                    ScrollView {
                        id: scrollView
                        anchors.fill: parent
                        clip: true
                        ScrollBar.vertical: ThinScrollBar {
                            anchors.right: parent.right
                        }

                        TextArea {
                            id: textArea
                            readOnly: true
                            text: item ? item.content : ""
                            color: root.codeColor
                            font.family: "Consolas"
                            font.pixelSize: 13
                            wrapMode: TextEdit.Wrap
                            selectByMouse: true
                            topPadding: 18
                            leftPadding: 12
                            rightPadding: 22
                            bottomPadding: 18
                            background: Rectangle {
                                color: root.editorBg
                                radius: 10
                            }
                        }
                    }
                }
            }
        }
    }
}