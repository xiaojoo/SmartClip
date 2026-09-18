pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SmartClip.Globals 1.0

/*
 * 文件对比卡片（两份正文的差异，并排看）。
 *
 * 数据在 C++ 那侧（SmartClip.Globals 的 Differ，见 src/Diff.h）：它把两份正文
 * 算成一张**已经对齐好的行表**（两边等长、短的补空行），这里只负责画。
 *
 * 为什么对齐在 C++ 做：对齐要用 LCS（见 Diff.h 开头那段），QML 里写这个
 * 既慢又难测；而且算法纯函数、能单独自检（见 SelfTest 的"对比"那一节）。
 *
 * 每一行左右两栏：左边是"基准"（一般是原来的那份），右边是"改过的"。
 * 底色：
 *   del  左边有、右边没有   -> 左侧红底（右边那一格画一道斜线表示"这里没有"）
 *   add  右边有、左边没有   -> 右侧绿底
 *   mod  两边都有但不一样   -> 两边都黄底
 *   same 一样               -> 不涂
 *
 * 横向滚动：行很长时两栏各自横滚（用同一个 contentX，滚一边另一边跟着走）。
 *
 * 是独立的 Window（Qt.Tool + 无边框），不是 Popup.Window：Popup.Window 的
 * x/y 在 Windows 上写不进去，卡片会贴在左上角（见 DocCard.qml 开头那段）。
 */
Window {
    id: root

    signal closed()

    readonly property color cardColor: "#2b2d30"
    readonly property color borderColor: "#4b4d4f"
    readonly property color textColor: "#d6d7da"
    readonly property color mutedColor: "#9aa0a6"
    readonly property color accentColor: "#4c96d8"
    readonly property color delBg: "#3a2224"
    readonly property color addBg: "#1e3524"
    readonly property color modBg: "#3a3320"
    readonly property color gutterBg: "#26282c"

    width: 980
    height: 600

    flags: Qt.Tool | Qt.FramelessWindowHint
    color: "transparent"

    /* 行高：和正文那套一致（12px 字号 → 18px 行高左右） */
    readonly property int rowHeight: 19
    readonly property int gutterWidth: 46
    /* 两栏各自滚到哪（共用一个值，滚一边两边一起动） */
    property real sharedX: 0

    Rectangle {
        anchors.fill: parent
        color: root.cardColor
        radius: 8
        border.width: 1
        border.color: root.borderColor
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 1
        spacing: 0

        /* ---------- 标题栏 ---------- */
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            color: "transparent"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 8
                spacing: 10

                Text {
                    text: "文件对比"
                    color: root.textColor
                    font.pixelSize: 14
                    font.bold: true
                }

                Text {
                    Layout.fillWidth: true
                    text: Differ.leftTitle + "   ↔   " + Differ.rightTitle
                    color: root.mutedColor
                    font.pixelSize: 12
                    elide: Text.ElideMiddle
                }

                /* 导出成补丁（统一格式，能存成 .patch） */
                Rectangle {
                    Layout.preferredWidth: 92
                    Layout.preferredHeight: 24
                    radius: 4
                    color: patchHit.containsMouse ? "#3a3d41" : "transparent"
                    border.color: root.borderColor

                    Text {
                        anchors.centerIn: parent
                        text: "复制补丁"
                        color: root.textColor
                        font.pixelSize: 12
                    }

                    MouseArea {
                        id: patchHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.copyPatchRequested()
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    radius: 4
                    color: closeHit.containsMouse ? "#5a3d40" : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "✕"
                        color: closeHit.containsMouse ? "#ffffff" : root.mutedColor
                        font.pixelSize: 13
                    }

                    MouseArea {
                        id: closeHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.closeCard()
                    }
                }
            }

            MouseArea {
                anchors.fill: parent
                anchors.rightMargin: 130
                acceptedButtons: Qt.LeftButton
                onPressed: (mouse) => {
                    if (root.startSystemMove)
                        root.startSystemMove()
                    mouse.accepted = true
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: root.borderColor
            opacity: 0.6
        }

        /* ---------- 概况一行 ---------- */
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 8
            text: Differ.lastError !== "" ? Differ.lastError : Differ.summary
            color: Differ.lastError !== "" ? "#e06c75" : root.mutedColor
            font.pixelSize: 12
            elide: Text.ElideRight
        }

        /* ---------- 两栏表头 ---------- */
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            Layout.topMargin: 8
            Layout.preferredHeight: 24
            spacing: 4

            Rectangle {
                Layout.preferredWidth: root.gutterWidth
                Layout.fillHeight: true
                color: root.gutterBg
                radius: 3

                Text {
                    anchors.centerIn: parent
                    text: "行"
                    color: root.mutedColor
                    font.pixelSize: 11
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: root.gutterBg
                radius: 3

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: Differ.leftTitle
                    color: root.mutedColor
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    width: parent.width - 16
                }
            }

            Rectangle {
                Layout.preferredWidth: root.gutterWidth
                Layout.fillHeight: true
                color: root.gutterBg
                radius: 3

                Text {
                    anchors.centerIn: parent
                    text: "行"
                    color: root.mutedColor
                    font.pixelSize: 11
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: root.gutterBg
                radius: 3

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: Differ.rightTitle
                    color: root.mutedColor
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    width: parent.width - 16
                }
            }
        }

        /* ---------- 差异行 ---------- */
        ListView {
            id: rowList

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            Layout.bottomMargin: 8
            Layout.topMargin: 4

            clip: true
            model: Differ.rows
            spacing: 0
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            cacheBuffer: 400
            /* 行高固定，省掉逐项测量（几千行也滚得动） */
            reuseItems: true

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            /*
             * 横向：Shift + 滚轮（和一堆编辑器一致；QML 的 Flickable 只会
             * 竖着滚，横着那一份得自己接）。
             */
            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: (event) => {
                    if (!(event.modifiers & Qt.ShiftModifier) || root.maxScrollX <= 0)
                        return
                    root.sharedX = Math.max(0, Math.min(root.maxScrollX,
                                                       root.sharedX - event.angleDelta.y / 3))
                }
            }

            delegate: Item {
                id: rowItem

                required property var modelData
                required property int index

                readonly property string kind: modelData.kind

                width: rowList.width
                height: root.rowHeight

                Row {
                    anchors.fill: parent
                    spacing: 4

                    /* ---- 左行号 ---- */
                    Rectangle {
                        width: root.gutterWidth
                        height: parent.height
                        color: rowItem.kind === "del" ? root.delBg
                             : rowItem.kind === "mod" ? root.modBg : root.gutterBg

                        Text {
                            anchors.centerIn: parent
                            text: rowItem.modelData.leftNo > 0 ? rowItem.modelData.leftNo : ""
                            color: rowItem.modelData.leftNo > 0 ? root.mutedColor : "#4b4d4f"
                            font.pixelSize: 11
                        }
                    }

                    /* ---- 左侧正文 ---- */
                    Rectangle {
                        width: Math.max(60, rowList.width / 2 - root.gutterWidth - 6)
                        height: parent.height
                        clip: true
                        color: rowItem.kind === "del" ? root.delBg
                             : rowItem.kind === "mod" ? root.modBg : "transparent"

                        Text {
                            x: -root.sharedX + 6
                            height: parent.height
                            text: rowItem.kind === "add" ? ""
                                  : rowItem.modelData.left
                            color: root.textColor
                            font.family: "Consolas"
                            font.pixelSize: 12
                            verticalAlignment: Text.AlignVCenter
                            /*
                             * 不换行、不加省略号：diff 里最要紧的是"这一行到底哪里不一样"，
                             * 省略号会正好吃掉不一样的那一段。横向滚动由 sharedX 管。
                             */
                            wrapMode: Text.NoWrap
                        }

                        /* 右侧多出来的行：左边这一格画一道提示（这里没有对应行） */
                        Text {
                            anchors.centerIn: parent
                            visible: rowItem.kind === "add"
                            text: "＋"
                            color: "#3f6b48"
                            font.pixelSize: 11
                        }
                    }

                    /* ---- 右行号 ---- */
                    Rectangle {
                        width: root.gutterWidth
                        height: parent.height
                        color: rowItem.kind === "add" ? root.addBg
                             : rowItem.kind === "mod" ? root.modBg : root.gutterBg

                        Text {
                            anchors.centerIn: parent
                            text: rowItem.modelData.rightNo > 0 ? rowItem.modelData.rightNo : ""
                            color: rowItem.modelData.rightNo > 0 ? root.mutedColor : "#4b4d4f"
                            font.pixelSize: 11
                        }
                    }

                    /* ---- 右侧正文 ---- */
                    Rectangle {
                        width: Math.max(60, rowList.width / 2 - root.gutterWidth - 6)
                        height: parent.height
                        clip: true
                        color: rowItem.kind === "add" ? root.addBg
                             : rowItem.kind === "mod" ? root.modBg : "transparent"

                        Text {
                            x: -root.sharedX + 6
                            height: parent.height
                            text: rowItem.kind === "del" ? ""
                                  : rowItem.modelData.right
                            color: root.textColor
                            font.family: "Consolas"
                            font.pixelSize: 12
                            verticalAlignment: Text.AlignVCenter
                            wrapMode: Text.NoWrap
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: rowItem.kind === "del"
                            text: "－"
                            color: "#7a4448"
                            font.pixelSize: 11
                        }
                    }
                }

                /*
                 * 点一行 -> 选中它（"上一个 / 下一个差异"是另一条路）。
                 * 只是记住当前位置，不滚动 —— 用户点一行多半是想看这一行，
                 * 把它挪到中间反而会跳一下。
                 */
                MouseArea {
                    anchors.fill: parent
                    onClicked: rowList.currentIndex = rowItem.index
                }
            }
        }

        /* ---------- 横向滚动条（两边一起动，见 sharedX） ---------- */
        Item {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            Layout.preferredHeight: 10
            visible: root.maxScrollX > 0

            Rectangle {
                anchors.fill: parent
                anchors.verticalCenter: parent.verticalCenter
                height: 4
                radius: 2
                color: "#3a3d41"

                Rectangle {
                    /* 滑块宽度按"看得见多少"给，位置按 sharedX 在这段里占的比例 */
                    x: root.maxScrollX > 0
                       ? (root.sharedX / root.maxScrollX) * (parent.width - width)
                       : 0
                    width: Math.max(28, parent.width * (root.paneWidth / root.lineWidth))
                    height: parent.height
                    radius: 2
                    color: root.mutedColor
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onPositionChanged: (mouse) => {
                    if (!pressed || root.maxScrollX <= 0)
                        return
                    var t = Math.max(0, Math.min(1, mouse.x / width))
                    root.sharedX = t * root.maxScrollX
                }
                onClicked: (mouse) => {
                    if (root.maxScrollX <= 0)
                        return
                    var t = Math.max(0, Math.min(1, mouse.x / width))
                    root.sharedX = t * root.maxScrollX
                }
            }
        }

        /* ---------- 底部：上一个 / 下一个差异 ---------- */
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            Layout.bottomMargin: 12
            spacing: 8

            Text {
                text: "共 " + Differ.rows.length + " 行"
                color: root.mutedColor
                font.pixelSize: 11
            }

            Item { Layout.fillWidth: true }

            Rectangle {
                Layout.preferredWidth: 88
                Layout.preferredHeight: 28
                radius: 4
                color: prevHit.containsMouse ? "#3a3d41" : "transparent"
                border.color: root.borderColor

                Text {
                    anchors.centerIn: parent
                    text: "上一个差异"
                    color: root.textColor
                    font.pixelSize: 12
                }

                MouseArea {
                    id: prevHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.gotoChange(-1)
                }
            }

            Rectangle {
                Layout.preferredWidth: 88
                Layout.preferredHeight: 28
                radius: 4
                color: nextHit.containsMouse ? "#3a3d41" : "transparent"
                border.color: root.borderColor

                Text {
                    anchors.centerIn: parent
                    text: "下一个差异"
                    color: root.textColor
                    font.pixelSize: 12
                }

                MouseArea {
                    id: nextHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.gotoChange(1)
                }
            }

            Rectangle {
                Layout.preferredWidth: 72
                Layout.preferredHeight: 28
                radius: 4
                color: okHit.containsMouse ? "#3a3d41" : "transparent"

                Text {
                    anchors.centerIn: parent
                    text: "关闭"
                    color: root.textColor
                    font.pixelSize: 12
                }

                MouseArea {
                    id: okHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.closeCard()
                }
            }
        }
    }

    Shortcut {
        sequence: "Esc"
        enabled: root.visible
        onActivated: root.closeCard()
    }

    /* 把补丁复制到剪贴板（Main.qml 接住：Differ.unifiedDiff() -> 剪贴板） */
    signal copyPatchRequested()

    function closeCard() {
        visible = false
        closed()
    }

    /*
     * 跳到上 / 下一个差异块。
     *
     * dir = -1 往前、1 往后。判据是"这一行不是 same"—— 差异块的第一行和
     * 用户想看的"下一处"是同一件事，所以不做块归组。
     */
    function gotoChange(dir) {
        var rows = Differ.rows
        if (rows.length === 0)
            return
        var from = rowList.currentIndex < 0 ? (dir > 0 ? -1 : rows.length) : rowList.currentIndex
        var i = from + dir
        while (i >= 0 && i < rows.length) {
            if (rows[i].kind !== "same") {
                rowList.currentIndex = i
                rowList.positionViewAtIndex(i, ListView.Center)
                return
            }
            i += dir
        }
        /* 到头了：绕回另一端（省得用户以为按钮坏了） */
        i = dir > 0 ? 0 : rows.length - 1
        while (i >= 0 && i < rows.length) {
            if (rows[i].kind !== "same") {
                rowList.currentIndex = i
                rowList.positionViewAtIndex(i, ListView.Center)
                return
            }
            i += dir
        }
    }

    /* 由 Main.qml 推过来的宿主窗口（摆位用） */
    property var parentTransient: null

    function placeInParent() {
        if (!parentTransient)
            return
        x = Math.max(20, Math.round((parentTransient.width - width) / 2))
        y = Math.max(20, Math.round((parentTransient.height - height) / 2) - 20)
    }

    /*
     * 最长那一行有多长（用于横向滚动的范围）。
     *
     * 逐字符宽度在等宽字体下是常数，所以直接用**最长行的字符数 × 单字宽**
     * 估算就够 —— 不需要真的去测量每一行（几千行的时候那是几千次文本测量）。
     */
    readonly property int maxLineChars: {
        var best = 0
        var rows = Differ.rows
        for (var i = 0; i < rows.length; ++i) {
            if (rows[i].left && rows[i].left.length > best)
                best = rows[i].left.length
            if (rows[i].right && rows[i].right.length > best)
                best = rows[i].right.length
        }
        return best
    }

    /* 估算出来的"一整行有多宽"（12px Consolas 的单字宽约 7px） */
    readonly property real lineWidth: Math.max(60, maxLineChars * 7 + 12)
    /* 两栏各自的可视宽（减去行号栏和间距） */
    readonly property real paneWidth: Math.max(60, width / 2 - gutterWidth - 6)
    readonly property real maxScrollX: Math.max(0, lineWidth - paneWidth)
}
