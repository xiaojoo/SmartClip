pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../utils"

/*
 * 窗口按钮：缩小 / 放大（已放大时显示还原）/ 关闭。
 *
 * 原先这三个按钮由 Windows 原生标题栏提供，标题栏去掉之后
 * 由这里接管，放在底部 tabbar 的最右侧。
 * 图标用 IconProvider 现画（win-min / win-max / win-restore / close），
 * 和项目里其它图标同一套笔画，不引入新的 svg 资源。
 */
RowLayout {
    id: root
    spacing: 0

    // 要操作的窗口，一般是 Main.qml 里的 ApplicationWindow
    property var host: null

    property color idleColor:  "#b4b8bf"
    property color hoverColor: "#3a3d41"
    property color closeHover: "#c42b1c"
    property color textColor:  "#ffffff"

    // 放大状态下换成“还原”图标
    readonly property bool maximized: host
        ? (host.visibility === Window.Maximized)
        : false

    IconProvider { id: icons }

    /*
     * 每个按钮等宽等高（Windows 11 是 46x32），
     * 高度撑满整条 bar，这样鼠标滑到最右边就是关闭，
     * 和原生标题栏的手感一致。
     */
    component WinButton: Rectangle {
        id: btn
        Layout.fillHeight: true
        Layout.preferredWidth: 46
        color: "transparent"

        property string kind: "win-min"
        property bool danger: false

        AppIcon {
            anchors.centerIn: parent
            provider: icons
            kind: btn.kind
            tint: btn.danger && mouse.containsMouse ? root.textColor : root.idleColor
            size: 14
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: {
                if (!root.host) return
                if (btn.kind === "win-min")        root.host.showMinimized()
                else if (btn.kind === "win-max")   root.host.showMaximized()
                else if (btn.kind === "win-restore") root.host.showNormal()
                else                               root.host.close()
            }
        }

        states: State {
            name: "hot"; when: mouse.containsMouse
            PropertyChanges {
                target: btn
                color: btn.danger ? root.closeHover : root.hoverColor
            }
        }
    }

    WinButton { kind: "win-min" }
    WinButton { kind: root.maximized ? "win-restore" : "win-max" }
    WinButton { kind: "close"; danger: true }
}
