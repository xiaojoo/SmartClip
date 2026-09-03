pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../js/TimeUtils.js" as Time
import "../utils"

// Central editor area: tab bar + welcome / image / text preview content.
Rectangle {
    id: root

    color: "#2b2b2b"; border.color: "#4b4d4f"; border.width: 1

    property var item: null
    property bool showWelcome: true

    readonly property color barBg:      "#313335"
    readonly property color editorBg:   "#2b2b2b"
    readonly property color borderColor:"#4b4d4f"
    readonly property color tabBg:      "#45484c"
    readonly property color textBright: "#e8e8e8"
    readonly property color textMain:   "#bbbbbb"
    readonly property color textMuted:  "#7d7d7d"
    readonly property color accentColor:"#4c96d8"
    readonly property color codeColor:  "#a9b7c6"
    readonly property color imageColor: "#d7a85b"

    IconProvider { id: icons }

    function tabTitle() {
        return item ? item.title : "README.md"
    }

    ColumnLayout {
        anchors.fill: parent; spacing: 0

        // tab bar
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 34; color: barBg; border.color: borderColor; border.width: 1
            RowLayout { anchors.fill: parent; anchors.leftMargin: 4; anchors.rightMargin: 6; spacing: 4
                Rectangle { Layout.preferredWidth: 240; Layout.preferredHeight: 28; radius: 4; color: tabBg
                    RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                        AppIcon { provider: icons; kind: item && item.type === "image" ? "image" : "file"
                            tint: item && item.type === "image" ? imageColor : accentColor
                            size: 12 }
                        Label { text: tabTitle(); color: textBright; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                        AppIcon { provider: icons; kind: "close"; tint: textMuted; size: 11 }
                    }
                    MouseArea { anchors.fill: parent; hoverEnabled: true }
                }
                Rectangle { Layout.preferredWidth: 30; Layout.preferredHeight: 28; radius: 4; color: "transparent"
                    AppIcon { anchors.centerIn: parent; provider: icons; kind: "plus"; tint: textMuted; size: 15 }
                    MouseArea { anchors.fill: parent; hoverEnabled: true }
                }
                Item { Layout.fillWidth: true }
                AppIcon { provider: icons; kind: "grid"; tint: accentColor; size: 12 }
                AppIcon { provider: icons; kind: "chevron-down"; tint: textMuted; size: 11 }
            }
        }

        // content
        Rectangle { Layout.fillWidth: true; Layout.fillHeight: true; color: editorBg

            // welcome / README
            Column {
                visible: root.showWelcome
                anchors.fill: parent; anchors.margins: 28; spacing: 8
                Label { text: "SmartClip — 剪贴板"; color: textBright; font.pixelSize: 22; font.bold: true }
                Label { text: "从剪贴板复制的文本和图片会自动保存为条目，点击左侧条目即可回填到剪贴板。"
                    color: textMain; font.pixelSize: 13; wrapMode: Text.Wrap; width: parent.width }
                Rectangle { width: parent.width; height: 1; color: borderColor }
                Label { text: "## 功能"; color: accentColor; font.pixelSize: 15; font.bold: true }
                Label { text: "• 自动采集：监听系统剪贴板，文本与图片自动存入本地数据库\n• 分类视图：按今天 / 昨天 / 近 7 天 / 更早 分组展示\n• 快速回填：单击任意条目即可复制回剪贴板\n• 全文搜索：按标题或内容即时过滤"
                    color: codeColor; font.pixelSize: 13; font.family: "Consolas"; lineHeight: 1.5; width: parent.width }
                Rectangle { width: parent.width; height: 1; color: borderColor }
                Label { text: "## 使用"; color: accentColor; font.pixelSize: 15; font.bold: true }
                Label { text: "1. 复制任意内容，SmartClip 自动记录\n2. 在左侧展开对应日期文件夹，点选条目\n3. 预览区显示内容，同时已复制回剪贴板"
                    color: codeColor; font.pixelSize: 13; font.family: "Consolas"; lineHeight: 1.5; width: parent.width }
            }

            // selected image preview
            Column {
                visible: !root.showWelcome && item && item.type === "image"
                anchors.fill: parent; anchors.margins: 20; spacing: 10
                RowLayout { width: parent.width; spacing: 8
                    AppIcon { provider: icons; kind: "image"; tint: imageColor; size: 14 }
                    Label { text: item ? item.title : ""; color: textMain; font.pixelSize: 13; font.bold: true; Layout.fillWidth: true }
                }
                Label { text: item ? Time.displayTime(item.createdAt) : ""; color: textMuted; font.pixelSize: 12 }
                Rectangle { width: parent.width; height: 1; color: borderColor }
                Item { width: parent.width; height: parent.height - 90
                    Image {
                        anchors.fill: parent
                        source: item ? "file:///" + item.content.replace(/\\/g, "/") : ""
                        fillMode: Image.PreserveAspectFit
                        horizontalAlignment: Image.AlignHCenter; verticalAlignment: Image.AlignVCenter
                    }
                }
            }

            // selected text preview (code editor look)
            ScrollView {
                visible: !root.showWelcome && item && item.type === "text"
                anchors.fill: parent; clip: true
                contentItem.boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThinScrollBar {}
                TextArea {
                    readOnly: true
                    text: item ? item.content : ""
                    color: codeColor; font.family: "Consolas"; font.pixelSize: 13
                    wrapMode: TextEdit.Wrap; selectByMouse: true
                    topPadding: 18; leftPadding: 22; rightPadding: 22; bottomPadding: 18
                    background: Rectangle { color: editorBg }
                }
            }
        }
    }
}
