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

    /* 和 Qt 自带提示框一样：贴在父项上方居中 */
    x: parent ? Math.round((parent.width - implicitWidth) / 2) : 0
    y: parent ? -implicitHeight - 3 : 0

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
