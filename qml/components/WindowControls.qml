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

    property color idleColor:  Theme.c("#b4b8bf", Theme.rev)

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

        /*
         * 这颗按钮要不要在**按下**时预热"最大化"（只有中间那颗要）。
         *
         * 为什么在这儿报：从按下到松开有一百来毫秒，那段时间窗口还是卡片大小，
         * 而 C++ 那边一次最大化要花的 222ms 里有 165ms 是"按 4K 渲染一帧"
         * （每帧都要交的面积税，改不掉，只能挪时机，见 src/WindowHelper.h 的
         * prewarmMaximize）。把这段利用起来，松手就几乎立刻到位。
         * 已经最大化时这颗是"还原"，预热函数自己会直接返回。
         *
         * **为什么不是悬停（onEntered）**：试过，实测点不动。预热会把内容控件摆成
         * "最大化那一版"布局，而 QML 的命中测试跟着**布局**走、不跟着屏幕上的像素走：
         * 指针停在放大按钮上，那颗按钮在 4K 布局里已经跑到右边别处去了，点下去落在顶栏
         * 中间那块能拖窗口的区域上 —— 日志里连 maximize() 都没进来（build\ab-on.txt）。
         *
         * 按下之后再摆没有"点不到"的问题（MouseArea 按下即把鼠标抓走，松开一定回到它
         * 自己身上），但**布局变了之后 clicked 不再发**（Qt 那条"松手位置还在本 Item
         * 里"的条件成立不了）。所以这一颗的动作改挂在 onReleased 上，由 C++ 那边
         * prewarmRelease() 认这一发（见 src/WindowHelper.h）。
         */
        property bool prewarmOnPress: false

        /* 这一发是不是已经在松开那一步办过了（办过就别再走 clicked，否则会翻回去） */
        property bool handledOnRelease: false

        function activate() {
            if (btn.kind === "win-min")
                Win.minimizeWindow()
            else if (btn.kind === "win-max" || btn.kind === "win-restore")
                Win.toggleMaximize()
            else
                /*
                 * 关闭键先问一句：完全退出，还是收进托盘（见 WindowHelper::askQuit）。
                 * 那个框是**非模态**的 —— 程序照常响应，不会像上一版那样看着卡住。
                 */
                Win.askQuit()
        }

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
             *
             * 预热那颗按钮的三步（见上面 btn.prewarmOnPress 那段）：
             *   按下 → 先花 165ms 把 4K 那一帧渲染好（这一段窗口还是卡片大小）；
             *   松开 → C++ 认这一发并当场最大化；
             *   clicked → 只有"没预热成"的那几次（探针关掉、条件不符）才会走到这儿。
             */
            onPressed: {
                btn.handledOnRelease = false
                if (btn.prewarmOnPress)
                    Win.prewarmMaximize()
            }
            onReleased: {
                if (btn.prewarmOnPress && Win.prewarmRelease())
                    btn.handledOnRelease = true
            }
            onClicked: {
                if (btn.handledOnRelease) {
                    btn.handledOnRelease = false
                    return
                }
                btn.activate()
            }
        }
    }

    WinButton { kind: "win-min" }
    /*
     * 中间这颗带预热。它**只**在按下时预热一下，动作还是"松开才办"（onReleased），
     * 所以在这儿不需要再 cancelPrewarm —— 收尾统一在 C++ 的 prewarmRelease 里。
     */
    WinButton { kind: root.maximized ? "win-restore" : "win-max"; prewarmOnPress: true }
    WinButton { kind: "close"; iconSize: 16 }
}
