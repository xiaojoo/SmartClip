pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../utils"
import SmartClip.Globals 1.0

/*
 * 窗口按钮：缩小 / 放大（已放大时显示还原）/ 关闭。
 *
 * 原先这三个按钮由 Windows 原生标题栏提供，标题栏去掉之后
 * 由这里接管，放在顶部那一行的最右侧。
 * 图标用 IconProvider 现画（win-min / win-max / win-restore / close），
 * 和项目里其它图标同一套笔画，不引入新的 svg 资源。
 */

RowLayout {
    id: root
    spacing: 0

    // 要操作的窗口，一般是 Main.qml 里的 ApplicationWindow
    property var host: null

    property color idleColor:  "#b4b8bf"

    /*
     * 悬停底色：项目里的强调蓝（和左侧图标条选中态、
     * 编辑区强调色同一个值），三个按钮统一用它。
     *
     * 关闭键原来单独用红色（#c42b1c）表示危险，
     * 现在按需求改成和另外两个一致：都是蓝色底 + 白色图标。
     */
    property color hoverColor: "#4c96d8"
    property color textColor:  "#ffffff"

    /*
     * 放大状态下换成“还原”图标。
     *
     * 这里看的也是 winHelper（Main.qml 里同一份真值）：
     * 展开 / 收拢动画一启动图标就换过来，不用等动画跑完，
     * 而且用户拖到屏幕顶端吸附最大化时图标也会跟着变。
     * 万一这个组件被单独跑起来、拿不到 helper，就退回窗口状态。
     */
    readonly property bool maximized: Win.maximized

    IconProvider { id: icons }

    /*
     * 每个按钮等宽（Windows 11 是 46x32），高度撑满整条 bar，
     * 这样鼠标滑到最右边就是关闭，和原生标题栏的手感一致。
     *
     * 现在这一组不再带右边距（见 TopBar.qml 的说明），
     * 所以最右边的关闭键是真正贴着窗口右上角的：
     * 顶到角上按下去就是关闭，右上角那点圆角由整窗遮罩统一修掉。
     *
     * 悬停态直接用 color 绑定表达（原来是 states/PropertyChanges，
     * 三个按钮的规则一样，写成绑定少一层状态机）：
     * 底色变强调蓝，图标同时转白，压在蓝底上才看得清。
     */
    component WinButton: Rectangle {
        id: btn
        Layout.fillHeight: true
        Layout.preferredWidth: 46
        color: mouse.containsMouse ? root.hoverColor : "transparent"

        property string kind: "win-min"
        property int iconSize: 14

        AppIcon {
            anchors.centerIn: parent
            provider: icons
            kind: btn.kind
            tint: mouse.containsMouse ? root.textColor : root.idleColor
            /*
             * 关闭键的图标给得比另外两个大一点。
             *
             * 三个按钮一样大，但 − 和 □ 是横向铺开的，
             * × 是斜着的一撇一捺，同样尺寸下看着明显偏小、
             * 右边空出一块。放大到 16 才和另外两个视觉等重。
             */
            size: btn.iconSize
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            /*
             * 主窗口已经不由 QML 承担（根元素是 Rectangle，见 Main.qml），
             * 所以三个按钮统一交给 WinHelper 去操作真正的窗口。
             */
            onClicked: {
                if (btn.kind === "win-min")
                    Win.minimizeWindow()
                else if (btn.kind === "win-max" || btn.kind === "win-restore")
                    Win.toggleMaximize()
                else
                    Win.closeWindow()
            }
        }
    }

    WinButton { kind: "win-min" }
    WinButton { kind: root.maximized ? "win-restore" : "win-max" }
    WinButton { kind: "close"; iconSize: 16 }
}
