pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SmartClip.Globals 1.0

/*
 * 校验结果卡片（"中文用词 / 代码语法"那一份结果清单）。
 *
 * 触发：编辑区右键 → "校验中文 / 代码"，或菜单"编辑 → 校验当前文件"。
 * 数据在 C++ 那侧（SmartClip.Globals 的 Check，见 src/Checker.h）——
 * 本地规则那部分是同步就有的，大模型那部分晚一步回来（信号 checked）。
 *
 * 和 AskCard 一样，本卡片不用 popupType: Popup.Window，而是一个**独立的
 * Window**（Qt.Tool + FramelessWindowHint）：理由见 DocCard.qml 开头那段
 * （Popup.Window 的 x/y 在 Windows 上写不进去，卡片会贴在左上角）。
 * 它比 AskCard 需要更多东西：要能拖、要能滚、要能选中一行跳到正文，
 * 所以自己管这块窗口更省事。
 *
 * 位置摆主窗口中间偏右上（不挡编辑区左下角那块，"跳过去"之后还能看见上下文）。
 */
Window {
    id: root

    /* 关闭时告诉 Main.qml（把窗口指针清掉 / 收起按钮状态） */
    signal closed()

    readonly property color cardColor: "#2b2d30"
    readonly property color borderColor: "#4b4d4f"
    readonly property color textColor: "#e8e8e8"
    readonly property color mutedColor: "#9aa0a6"
    readonly property color accentColor: "#4c96d8"
    readonly property color errorColor: "#e06c75"
    readonly property color warnColor: "#d7a85b"
    readonly property color infoColor: "#7fa8d0"

    /* 正在看的是哪一份文件（标题栏显示） */
    property string docTitle: ""

    width: 460
    height: 470

    flags: Qt.Tool | Qt.FramelessWindowHint
    color: "transparent"

    /* 整块卡片本体（圆角 + 边框 + 深色底） */
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

        /* ---------- 标题栏（按住可拖） ---------- */
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            color: "transparent"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 8
                spacing: 8

                Text {
                    text: "校验"
                    color: root.textColor
                    font.pixelSize: 14
                    font.bold: true
                }

                Text {
                    Layout.fillWidth: true
                    text: root.docTitle
                    color: root.mutedColor
                    font.pixelSize: 12
                    elide: Text.ElideMiddle
                }

                /* 再跑一次（改了正文之后） */
                Rectangle {
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    radius: 4
                    color: reHit.containsMouse ? "#3a3d41" : "transparent"
                    visible: !Check.busy

                    Text {
                        anchors.centerIn: parent
                        text: "↻"
                        color: reHit.containsMouse ? root.textColor : root.mutedColor
                        font.pixelSize: 14
                    }

                    MouseArea {
                        id: reHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.rerunRequested()
                    }
                }

                /* 关掉 */
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

            /* 标题栏拖动：交给窗口管理器（和 TopBar 那种做法一致） */
            MouseArea {
                anchors.fill: parent
                anchors.rightMargin: 60
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

        /* ---------- 状态那一行 ---------- */
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 10
            text: Check.status !== "" ? Check.status : Check.resultSummary()
            color: Check.issues.length === 0 && !Check.busy ? "#7bc47f" : root.mutedColor
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        /* 大模型那条支路没配好时说清楚（本地规则照样出结果） */
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            visible: Check.enabled && !Check.llmReady
            text: "大模型还没配好（设置 → 模型）：现在只跑本地规则。"
            color: root.warnColor
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }

        /* 转圈那条（模型那部分还在路上） */
        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 8
            Layout.preferredHeight: 3
            radius: 2
            visible: Check.busy
            color: "#3a3d41"

            Rectangle {
                id: busyPulse
                width: parent.width * 0.3
                height: parent.height
                radius: 2
                color: root.accentColor

                SequentialAnimation on x {
                    running: Check.busy
                    loops: Animation.Infinite
                    NumberAnimation {
                        from: 0
                        to: busyPulse.parent.width - busyPulse.width
                        duration: 900
                        easing: Easing.InOutQuad
                    }
                    NumberAnimation {
                        from: busyPulse.parent.width - busyPulse.width
                        to: 0
                        duration: 900
                        easing: Easing.InOutQuad
                    }
                }
            }
        }

        /* ---------- 问题清单 ---------- */
        ListView {
            id: list

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: 10
            Layout.leftMargin: 6
            Layout.rightMargin: 6

            clip: true
            model: Check.issues
            spacing: 2
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            flickableDirection: Flickable.VerticalFlick

            delegate: Rectangle {
                id: issueRow

                required property var modelData
                required property int index

                readonly property color sevColor: modelData.severity === "error"
                                                  ? root.errorColor
                                                  : (modelData.severity === "warn"
                                                     ? root.warnColor : root.infoColor)

                width: list.width
                height: issueColumn.implicitHeight + 14
                radius: 5
                color: issueHit.containsMouse ? "#34373a" : "transparent"

                Column {
                    id: issueColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 3

                    Row {
                        width: parent.width
                        spacing: 6

                        /* 严重度那个小点 */
                        Rectangle {
                            width: 6
                            height: 6
                            radius: 3
                            anchors.verticalCenter: parent.verticalCenter
                            color: issueRow.sevColor
                        }

                        Text {
                            text: "第 " + issueRow.modelData.row + " 行"
                                  + (issueRow.modelData.source === "llm" ? "（模型）" : "")
                            color: issueRow.sevColor
                            font.pixelSize: 11
                        }

                        Text {
                            width: parent.width - x
                            text: issueRow.modelData.message
                            color: root.textColor
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                    }

                    /* 出问题的那一小段原文（等宽字体，一眼看出是哪几个字） */
                    Text {
                        visible: issueRow.modelData.snippet !== ""
                        width: parent.width
                        text: "「" + issueRow.modelData.snippet + "」"
                              + (issueRow.modelData.suggestion !== ""
                                 ? "  →  " + issueRow.modelData.suggestion : "")
                        color: root.mutedColor
                        font.family: "Consolas"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    id: issueHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    /* 点一条：跳到正文那一行（Main.qml 接住） */
                    onClicked: root.jumpRequested(issueRow.modelData.row,
                                                  issueRow.modelData.col,
                                                  issueRow.modelData.endCol)
                }
            }

            /* 一条都没有时的空状态 */
            Text {
                anchors.centerIn: parent
                width: list.width - 40
                visible: list.count === 0
                text: Check.busy ? "正在检查…"
                                 : (Check.status !== "" ? "没有发现问题"
                                                        : "还没校验")
                color: root.mutedColor
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }

        /* ---------- 底部两个动作 ---------- */
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.bottomMargin: 12
            Layout.topMargin: 8
            spacing: 8

            Item { Layout.fillWidth: true }

            Rectangle {
                Layout.preferredWidth: 96
                Layout.preferredHeight: 28
                radius: 4
                color: copyHit.containsMouse ? "#3a3d41" : "transparent"
                border.color: root.borderColor

                Text {
                    anchors.centerIn: parent
                    text: "复制结果"
                    color: root.textColor
                    font.pixelSize: 12
                }

                MouseArea {
                    id: copyHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.copyRequested()
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

    /* Esc 关掉 */
    Shortcut {
        sequence: "Esc"
        enabled: root.visible
        onActivated: root.closeCard()
    }

    /* 跳正文（Main.qml 接住：选到那一行那一列） */
    signal jumpRequested(int row, int col, int endCol)
    /* 重新校验 */
    signal rerunRequested()
    /* 把结果复制成文字（Main.qml 接住：写系统剪贴板） */
    signal copyRequested()

    /*
     * 收起来。
     *
     * 名字**不能叫 close()**：Window 自己有一个 close()（而且是个可调用的
     * 成员），同名会把那个盖掉 —— QML 直接报
     * "Unable to assign a function to a property of any type other than var"，
     * 整张卡片加载失败（实测踩过）。
     */
    function closeCard() {
        visible = false
        closed()
    }

    /*
     * 摆到主窗口右上方（parent 是主窗口那个 QQuickWidget 的内容层，
     * 见 Main.qml 里怎么给它 parent）。
     */
    function placeInParent() {
        if (!parentTransient)
            return
        x = Math.max(20, Math.round(parentTransient.width - width - 40))
        y = 90
    }

    /* 由 Main.qml 推过来的"宿主窗口"（用它算摆放位置，见 placeInParent） */
    property var parentTransient: null
}
