pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

/*
 * 深色工具提示 —— 界面里所有提示框都用它（提示框的**全局外观**都在这一个文件里）。
 *
 * 为什么不是 `ToolTip` 控件 + 附加属性：
 *   * 外观：Qt 样式（Fusion）里的 ToolTip 模板取的是 palette.toolTipBase /
 *     toolTipText，而那份调色板由**样式 / 平台主题**决定 —— 应用调色板
 *     （main.cpp 里设的）和 QML 根上的 palette 都压不住它，实测提示框一直是
 *     系统那种浅黄底 #FFFFE1；鼠标停在按钮上弹一块亮色，跟深色界面完全不搭。
 *   * 时机：附加属性那套 delay / 自动隐藏由它自己的计时器管，换成实例之后
 *     不再生效（实测把 ToolTip 当实例用，visible 置 true 也不弹）。
 * 所以这里直接用 `Popup` 自己画：底色、边框、文字、弹出时机全在自己手里，
 * 不依赖样式也不依赖调色板。
 *
 * 用法：
 *     AppToolTip {
 *         hovered: 某个 MouseArea.containsMouse
 *         text: "提示文字"
 *     }
 * 悬停超过 appearDelay 毫秒才弹，鼠标一离开立刻收。
 */
Popup {
    id: control

    /* 外观（自检读这三个，见 Main.qml 的 uiState） */
    readonly property color tipBackground: "#2b2d30"
    readonly property color tipBorder: "#4b4d4f"
    readonly property color tipTextColor: "#d6d7da"

    /* 提示文字 */
    property string text: ""
    /* 调用方绑"鼠标停在里面"；弹出 / 收起由下面那个 Timer 管 */
    property bool hovered: false
    /* 悬停多久才弹（原来附加属性的 delay 是 420） */
    property int appearDelay: 420

    padding: 6

    /*
     * 贴在父项上方居中，但**不许越过窗口左右两边**。
     *
     * 原来是一条绑定 `x: (parent.width - implicitWidth) / 2`：贴着右边缘的那些
     * 按钮（标签栏最右边那个"源码 / 预览"开关）居中之后有一半跑到窗口外面，
     * 气泡被切掉半截。
     *
     * 改成在要弹出来之前算一次（place()）而不是写绑定：夹取要看父项在**窗口**
     * 里的位置，那是 mapToItem 的量 —— 绑定不会在窗口移动 / 改大小之后自己重算，
     * 而 hover 弹出本来就是一个"此刻"的动作，在那一刻算最准也最省。
     * 每次弹出都会重算（onAboutToShow），实测窗口从 1460 拉到 700 之后第二次
     * 弹出夹的是新边界。
     *
     * 一个坑（自检因此要真 open 一次，见 Main.qml 的 tipEdgeProbeState）：
     * Popup **收着的时候 x 写了不落地** —— 实测写 -486，关着读回来还是 0，
     * open 之后才生效。所以"调一下 place() 再读属性"量不到东西。
     */
    property int edgeMargin: 6

    function place() {
        if (!parent) {
            x = 0
            return
        }
        const want = Math.round((parent.width - implicitWidth) / 2)
        /*
         * Popup 自己没有 window 属性，所以从父项那边取挂载属性 Window.window；
         * 拿不到（父项还不在任何窗口里）就退回原来的"居中"算法，别算出个 NaN。
         */
        const win = parent.Window ? parent.Window.window : null
        if (!win) {
            x = want
            return
        }
        const parentX = parent.mapToItem(null, 0, 0).x
        const low = edgeMargin - parentX
        const high = win.width - edgeMargin - implicitWidth - parentX
        /* 气泡比窗口还宽时（high < low）只能先保住左边不被切 */
        x = high < low ? low : Math.max(low, Math.min(high, want))
    }

    y: parent ? -implicitHeight - 3 : 0

    onAboutToShow: place()

    /*
     * 可见性完全由 hovered 驱动，不做属性绑定 —— 绑了之后在 Timer 里赋值会把
     * 绑定打断，来回切的时候行为就不好推了。
     */
    onHoveredChanged: {
        if (hovered) {
            showTimer.restart()
        } else {
            showTimer.stop()
            visible = false
        }
    }

    Timer {
        id: showTimer

        interval: control.appearDelay
        onTriggered: control.visible = control.text !== ""
    }

    background: Rectangle {
        color: control.tipBackground
        border.color: control.tipBorder
        border.width: 1
        radius: 4
    }

    contentItem: Text {
        text: control.text
        color: control.tipTextColor
        font: control.font
        wrapMode: Text.Wrap
    }
}
