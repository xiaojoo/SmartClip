pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

Rectangle {
    id: root

    required property var modelData
    property bool rowHighlight: false
    signal rowClicked()

    readonly property color selColor:    "#214283"
    readonly property color textBright:  "#e8e8e8"
    readonly property color textColor:   "#bbbbbb"
    readonly property color textMuted:   "#7d7d7d"
    readonly property color accentColor: "#4c96d8"
    readonly property color chevronDim:  "#6f767e"
    readonly property color folderColor: "#c8b74f"
    readonly property color imageColor:  "#d7a85b"

    readonly property bool isFolder: modelData && modelData.kind === "folder"

    IconProvider { id: icons }

    /*
     * 行高：分组和条目一样高（24）。
     *
     * 分组原来是 28：内容只有 16px 高的图标 + 12px 的文字，多出来的 4px 就
     * 全变成行内的空白，四个分组摞在一起时上下各空 6px、再加上列表那 2px 的
     * 行距，看着有十几 px 那么散。压到 24 之后分组之间和条目之间一样紧。
     */
    implicitHeight: 24
    radius: 3

    /*
     * 只有当前选中项有蓝色高亮，鼠标悬停不再变色。
     *
     * 原来的 hoverColor 灰底有两个问题：
     * 一是窗口被缩放 / 最小化 / 鼠标移出窗口时，containsMouse
     * 有时不会被重置，灰色的悬停块就留在列表里好几个；
     * 二是列表本身已经有蓝色选中条，再多一种底色反而乱。
     */
    color: rowHighlight ? selColor : "transparent"

    /*
     * 行内元素之间的间隙。
     *
     * 3px：折叠箭头紧贴着文件夹图标（原来这里还夹着一个 2px 的占位 Item，
     * 加上两侧各 4px 的 spacing，箭头和图标之间隔着 10px，看着像两件事）。
     * 图标和文字之间同样 3px —— 行内一律一个间距，不搞第二种。
     *
     * 图标尺寸**分级**：一级菜单（今天 / 昨天 / 近 7 天 / 更早）的文件夹图标
     * 16px，比正文字号（12）大一档 —— 13 那会儿和文字几乎一样大，图标糊在
     * 文字里分不出是图标还是字；子级条目（图片 / 文本）仍旧 13px，
     * 层级在尺寸上也分得开，不改子级那一排的观感。
     * 折叠箭头只在分组行上，跟着分组走 13px。
     */
    RowLayout {
        anchors.fill: parent; spacing: 3
        anchors.leftMargin: isFolder ? 6 : 22
        anchors.rightMargin: 6
        AppIcon {
            visible: isFolder
            provider: icons
            kind: modelData.expanded ? "chevron-down" : "chevron-right"
            tint: rowHighlight ? textBright : chevronDim
            size: 13
            Layout.alignment: Qt.AlignVCenter
        }
        AppIcon {
            provider: icons
            kind: isFolder ? "folder" : (modelData.item.type === "image" ? "image" : "file")
            tint: isFolder ? folderColor : (modelData.item.type === "image" ? imageColor : accentColor)
            size: isFolder ? 16 : 13
            Layout.alignment: Qt.AlignVCenter
        }
        Label {
            text: isFolder
                  ? modelData.label
                  : (modelData.item.type === "image" ? "图片 · " + modelData.item.title : modelData.item.title)
            color: isFolder ? textBright : textColor
            font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true
        }
        Label {
            text: isFolder ? modelData.count : ""
            color: textMuted; font.pixelSize: 11; Layout.alignment: Qt.AlignVCenter
        }
    }
    MouseArea {
        // hoverEnabled 关掉：这个委托已经没有悬停样式了，
        // 留着它只会不断产生 containsMouse 变化通知
        id: mouse; anchors.fill: parent
        onClicked: root.rowClicked()
    }
}