pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"
import SmartClip.Globals 1.0

/*
 * 工具栏按钮。
 *
 * 主流编辑器工具栏的按钮就三件事：图标、悬停高亮、工具提示带快捷键。
 * 再多的（文字标签 / 下拉箭头 / 选中态）都是可选的开关，这里都留了属性。
 *
 * 注意 enabled 用的是 Item 自带的那个 —— 子项会一起禁用，MouseArea
 * 收不到点击，光标也自动回到箭头。
 */
Rectangle {
    id: root

    property var provider: null
    property string kind: ""
    property string label: ""
    property string tip: ""
    property string shortcut: ""
    property bool checked: false
    property bool showArrow: false
    /*
     * primary = 实心蓝主按钮（查找栏那颗「替换」：样例里它是唯一一颗实心的）。
     * framed  = 常驻描边 + 淡底（上一个 / 下一个 / 全部替换：样例里它们不是
     *           "悬停才亮"的扁按钮，而是三颗看得出边界的次级按钮）。
     * 两个都默认关 —— 顶栏和左侧树那些按钮的样子一个字都不动。
     */
    property bool primary: false
    property bool framed: false

    /*
     * 图标边长。默认 16（查找栏那排就是这个尺寸）；
     * 左侧项目树标题栏那一排要小一档，见 FolderTree.toolIconSize。
     */
    property int iconSize: 16

    signal clicked()

    readonly property bool hot: hit.containsMouse && root.enabled
    readonly property color idleIcon: root.enabled ? Theme.c("#9aa0a8", Theme.rev) : Theme.c("#5c6066", Theme.rev)
    /* 实心那颗上面只该有一种颜色：白 */
    readonly property color hotIcon: root.primary ? "#ffffff"
                                     : root.checked ? "#ffffff"
                                                    : Theme.c("#e8e8e8", Theme.rev)

    implicitWidth: content.implicitWidth + 16
    implicitHeight: 28
    radius: 4

    /*
     * 蓝用的是这扇应用自己的那一颗（checked 态一直是 #3d78b8，悬停亮一档到 #4c96d8），
     * 没有照抄参考图上的 #2c87fc —— 抄过来会变成"查找栏的蓝和别处不一样"。
     */
    color: !root.enabled ? (root.framed ? Theme.c("#2f3234", Theme.rev) : "transparent")
                          : root.primary ? (root.hot ? "#4c96d8" : "#3d78b8")
                          : root.checked ? Theme.c("#3d78b8", Theme.rev)
                          : root.framed ? (root.hot ? Theme.c("#45484c", Theme.rev)
                                                    : Theme.c("#3a3d41", Theme.rev))
                          : (root.hot ? Theme.c("#45484c", Theme.rev) : "transparent")
    border.width: root.framed && !root.primary ? 1 : 0
    border.color: Theme.c("#4b4d4f", Theme.rev)

    RowLayout {
        id: content

        anchors.centerIn: parent
        spacing: 5

        AppIcon {
            visible: root.kind !== ""
            provider: root.provider
            kind: root.kind
            size: root.iconSize
            tint: (root.hot || root.checked || root.primary) ? root.hotIcon : root.idleIcon
        }

        Label {
            visible: root.label !== ""
            text: root.label
            font.pixelSize: 11
            color: (root.hot || root.checked || root.primary) ? root.hotIcon : Theme.c("#b4b8bf", Theme.rev)
        }

        AppIcon {
            visible: root.showArrow
            provider: root.provider
            kind: "chevron-down"
            size: 9
            tint: (root.hot || root.checked || root.primary) ? root.hotIcon : Theme.c("#7d838c", Theme.rev)
        }
    }

    MouseArea {
        id: hit
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: root.clicked()
    }

    /* 深色提示框：外观和弹出时机都在 AppToolTip.qml 里 */
    AppToolTip {
        hovered: hit.containsMouse && root.tip !== ""
        text: root.shortcut !== "" ? root.tip + "  (" + root.shortcut + ")" : root.tip
    }
}
