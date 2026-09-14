pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../components"
import "../utils"
import SmartClip.Globals 1.0

/*
 * 桌面翻译卡片（由 src/Translate.cpp 的 TranslateCard 用 QQuickWidget 装进一个
 * 置顶无边框小窗，和便签窗口同一个套路）。
 *
 * 布局就是用户要的那两格：上面输入、下面译文；中间一排"源语言 ⇄ 目标语言"
 * 和一个「翻译」按钮。点翻译时直接调 Llm 这个单例（设置面板里配的模型就是
 * 它），回来的结果按 token 认领 —— 用户连点两次时不会串行。
 *
 * 两份状态的分工（见 TranslateCard::pushToQml 的说明）：
 *   * 界面在跑的时候，这份 QML 里的属性就是界面上的那一份；
 *   * C++ 那份是落盘用的存档，改一下（cardWin.xxx = …）就会写回存档；
 *   * 恢复时方向相反：C++ 把存档推回根对象的属性，onXxxChanged 再把界面
 *     上的控件同步过去（输入框不能用绑定 —— 用户一敲字绑定就断了）。
 */
Item {
    id: root

    /* ---- 由 C++ 塞进来的状态（setInitialProperties，见 TranslateCard） ---- */
    property var cardWin: null
    property string textIn: ""
    property string textOut: ""
    property string sourceLang: "自动检测"
    property string targetLang: "中文（简体）"
    property bool pinned: true
    property int cardNumber: 1

    /* ---- 界面自己的临时状态（不落盘） ---- */
    property bool busy: false
    /* 状态行文案：翻译中 / 出错了 / 已复制 */
    property string hint: ""
    property string hintColor: root.mutedColor
    /* 这次等待的是哪个请求（对不上就说明是上一次那口，丢掉） */
    property string pendingToken: ""

    readonly property color cardColor:   "#2b2d30"
    readonly property color headerColor: "#33363a"
    readonly property color fieldColor:  "#232528"
    readonly property color borderColor: "#4b4d4f"
    readonly property color textColor:   "#c8ccd1"
    readonly property color brightColor: "#e8e8e8"
    readonly property color mutedColor:  "#8a9098"
    readonly property color hoverColor:  "#3a3d41"
    readonly property color accentColor: "#4c96d8"
    readonly property color warnColor:   "#c8503c"

    /*
     * 两个框的高度：上面输入、下面译文各占一半。
     * 固定那几行是标题栏 34 + 语言栏 36 + 按钮行 34 + 两处 8px 缝。
     */
    readonly property real boxHeight: Math.max(64, Math.floor((height - 34 - 36 - 34 - 16) / 2))

    /* ---- 状态同步（界面 -> 存档 / 存档 -> 界面） ---- */

    function editInput(text) {
        if (textIn !== text)
            textIn = text
        if (cardWin)
            cardWin.textIn = text
    }

    function editOutput(text) {
        if (textOut !== text)
            textOut = text
        if (cardWin)
            cardWin.textOut = text
    }

    function setSource(lang) {
        sourceLang = lang
        if (cardWin)
            cardWin.sourceLang = lang
    }

    function setTarget(lang) {
        targetLang = lang
        if (cardWin)
            cardWin.targetLang = lang
        /*
         * 顺手把"默认目标语言"也改成这个：下次新卡片（或者换台机器之后）
         * 用的就是用户最近一次挑的语言。设置面板里不再单开一项。
         */
        if (lang)
            Llm.defaultTarget = lang
    }

    /* 存档推回来的时候，把输入框里的字也换掉（输入框没有绑定，见文件头） */
    onTextInChanged: {
        if (input && input.text !== textIn)
            input.text = textIn
    }
    onSourceLangChanged: if (cardWin) cardWin.sourceLang = sourceLang
    onTargetLangChanged: if (cardWin) cardWin.targetLang = targetLang

    /* 语言下拉的条目：[ { label, checked } ]（给下面那个浮层用） */
    function languageEntries(list, current) {
        var out = []
        for (var i = 0; i < (list ? list.length : 0); ++i)
            out.push({ label: list[i], checked: list[i] === current })
        return out
    }

    /* ---- 翻译 ---- */

    function startTranslate() {
        if (busy)
            return
        if (input.text.trim() === "") {
            hint = "先在上面输入要翻译的内容"
            hintColor = root.warnColor
            return
        }
        hint = ""
        hintColor = root.mutedColor
        busy = true
        pendingToken = Llm.translate(input.text, targetLang, sourceLang)
        if (pendingToken === "") {
            /* 理论上不会发生（translate 一定给 token），兜底免得一直转圈 */
            busy = false
            hint = "翻译请求没能发出去"
            hintColor = root.warnColor
        }
    }

    /* LLM 的回调按 token 认领，见 pendingToken 的说明 */
    Connections {
        target: Llm

        function onFinished(token, text) {
            if (token !== root.pendingToken)
                return
            root.pendingToken = ""
            root.busy = false
            root.editOutput(text)
            root.hint = ""
        }

        function onFailed(token, error) {
            if (token !== root.pendingToken)
                return
            root.pendingToken = ""
            root.busy = false
            root.hint = error
            root.hintColor = root.warnColor
        }
    }

    Rectangle {
        id: card
        anchors.fill: parent
        color: root.cardColor
        border.color: root.borderColor
        border.width: 1
        radius: 10

        Column {
            anchors.fill: parent
            anchors.margins: 1
            spacing: 0

            /* ---------------- 标题栏（按住就是拖窗口） ---------------- */
            Rectangle {
                id: header
                width: parent.width
                height: 34
                color: root.headerColor
                topLeftRadius: 9
                topRightRadius: 9

                /* 拖动区先声明：后面的按钮盖在它上面，点按钮不会变成拖窗口 */
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.SizeAllCursor
                    onPressed: if (root.cardWin) root.cardWin.beginDrag()
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: "翻译 " + root.cardNumber
                    color: root.brightColor
                    font.pixelSize: 13
                    font.bold: true
                }

                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    /* 置顶（固定在桌面上）：亮着 = 一直在最前面 */
                    Rectangle {
                        width: 24; height: 24; radius: 5
                        color: pinHit.containsMouse ? root.hoverColor : "transparent"
                        AppIcon {
                            anchors.centerIn: parent
                            provider: icons
                            kind: "pin"
                            size: 14
                            tint: root.pinned ? root.accentColor
                                              : (pinHit.containsMouse ? root.brightColor
                                                                      : root.mutedColor)
                        }
                        MouseArea {
                            id: pinHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (root.cardWin) root.cardWin.togglePinned()
                        }
                        AppToolTip { hovered: pinHit.containsMouse; text: root.pinned ? "取消置顶" : "固定在桌面上（置顶）" }
                    }

                    /* 复制译文 */
                    Rectangle {
                        width: 24; height: 24; radius: 5
                        color: copyHit.containsMouse ? root.hoverColor : "transparent"
                        AppIcon {
                            anchors.centerIn: parent
                            provider: icons
                            kind: "copy"
                            size: 14
                            tint: copyHit.containsMouse ? root.brightColor : root.mutedColor
                        }
                        MouseArea {
                            id: copyHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (root.cardWin) root.cardWin.copyResult()
                        }
                        AppToolTip { hovered: copyHit.containsMouse; text: "复制译文" }
                    }

                    /* 收起来（数据留着，图标条 / 托盘能再叫出来） */
                    Rectangle {
                        width: 24; height: 24; radius: 5
                        color: closeHit.containsMouse ? "#7a3a34" : "transparent"
                        AppIcon {
                            anchors.centerIn: parent
                            provider: icons
                            kind: "close"
                            size: 13
                            tint: closeHit.containsMouse ? "#ffffff" : root.mutedColor
                        }
                        MouseArea {
                            id: closeHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (root.cardWin) root.cardWin.closeCard()
                        }
                    }
                }
            }

            /* ---------------- 语言那一排 ---------------- */
            Item {
                width: parent.width
                height: 36

                Rectangle {
                    id: srcBtn
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: (parent.width - 16 - 28) / 2
                    height: 24
                    radius: 4
                    color: srcHit.containsMouse ? root.hoverColor : "transparent"
                    border.width: 1
                    border.color: root.borderColor

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.right: parent.right
                        anchors.rightMargin: 18
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.sourceLang
                        color: root.textColor
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    AppIcon {
                        anchors.right: parent.right
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        provider: icons
                        kind: "chevron-down"
                        size: 12
                        tint: root.mutedColor
                    }
                    MouseArea {
                        id: srcHit
                        objectName: "translateSource"
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: langPopup.openAt("src",
                                                   root.languageEntries(Llm.languages, root.sourceLang),
                                                   srcBtn)
                    }
                }

                /* 对调源 / 目标语言 */
                Rectangle {
                    id: swapBtn
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24; height: 24; radius: 4
                    color: swapHit.containsMouse ? root.hoverColor : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: "⇄"
                        color: swapHit.containsMouse ? root.brightColor : root.mutedColor
                        font.pixelSize: 14
                    }
                    MouseArea {
                        id: swapHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (root.cardWin) root.cardWin.swapLanguages()
                    }
                    AppToolTip { hovered: swapHit.containsMouse; text: "对调源语言 / 目标语言" }
                }

                Rectangle {
                    id: tgtBtn
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: srcBtn.width
                    height: 24
                    radius: 4
                    color: tgtHit.containsMouse ? root.hoverColor : "transparent"
                    border.width: 1
                    border.color: root.borderColor

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.right: parent.right
                        anchors.rightMargin: 18
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.targetLang
                        color: root.brightColor
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    AppIcon {
                        anchors.right: parent.right
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        provider: icons
                        kind: "chevron-down"
                        size: 12
                        tint: root.mutedColor
                    }
                    MouseArea {
                        id: tgtHit
                        objectName: "translateTarget"
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: langPopup.openAt("tgt",
                                                   root.languageEntries(Llm.targetLanguages, root.targetLang),
                                                   tgtBtn)
                    }
                }
            }

            /* ---------------- 上面那个框：输入 ---------------- */
            Rectangle {
                width: parent.width - 16
                x: 8
                height: root.boxHeight
                radius: 6
                color: root.fieldColor
                border.width: 1
                border.color: input.activeFocus ? root.accentColor : root.borderColor

                Flickable {
                    anchors.fill: parent
                    anchors.margins: 8
                    clip: true
                    contentWidth: width
                    contentHeight: input.contentHeight
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThinScrollBar {}

                    TextArea.flickable: TextArea {
                        id: input
                        /* 自检按这个名字找它（--translate-test，见 src/SelfTestTranslate.cpp） */
                        objectName: "translateInput"
                        width: parent.width
                        placeholderText: "在这里输入要翻译的内容（Ctrl+Enter 直接翻译）"
                        placeholderTextColor: root.mutedColor
                        color: root.textColor
                        font.pixelSize: 13
                        wrapMode: TextArea.Wrap
                        selectByMouse: true
                        background: null
                        padding: 0
                        onTextChanged: root.editInput(text)
                        Keys.onPressed: (event) => {
                            if (event.key === Qt.Key_Return && (event.modifiers & Qt.ControlModifier)) {
                                event.accepted = true
                                root.startTranslate()
                            }
                        }
                    }
                }
            }

            /* ---------------- 中间：翻译按钮 + 状态 ---------------- */
            Item {
                width: parent.width
                height: 34

                Rectangle {
                    id: runBtn
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 92
                    height: 24
                    radius: 4
                    color: runHit.pressed ? "#3a6f9e"
                                         : (runHit.containsMouse ? "#5aa4e6" : root.accentColor)
                    opacity: root.busy ? 0.6 : 1.0

                    Text {
                        anchors.centerIn: parent
                        text: root.busy ? "翻译中…" : "翻译"
                        color: "#ffffff"
                        font.pixelSize: 12
                        font.bold: true
                    }
                    MouseArea {
                        id: runHit
                        objectName: "translateRun"
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.startTranslate()
                    }
                }

                Rectangle {
                    id: clearBtn
                    anchors.left: runBtn.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 60
                    height: 24
                    radius: 4
                    color: clearHit.containsMouse ? root.hoverColor : "transparent"
                    border.width: 1
                    border.color: root.borderColor

                    Text {
                        anchors.centerIn: parent
                        text: "清空"
                        color: clearHit.containsMouse ? root.brightColor : root.textColor
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: clearHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (root.cardWin) root.cardWin.clearAll()
                    }
                }

                Text {
                    anchors.left: clearBtn.right
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.hint
                    color: root.hintColor
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }

            /* ---------------- 下面那个框：译文 ---------------- */
            Rectangle {
                width: parent.width - 16
                x: 8
                height: root.boxHeight
                radius: 6
                color: root.fieldColor
                border.width: 1
                border.color: root.borderColor

                Flickable {
                    anchors.fill: parent
                    anchors.margins: 8
                    clip: true
                    contentWidth: width
                    contentHeight: output.contentHeight
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ThinScrollBar {}

                    TextArea.flickable: TextArea {
                        id: output
                        objectName: "translateOutput"
                        width: parent.width
                        text: root.textOut
                        placeholderText: "译文会显示在这里"
                        placeholderTextColor: root.mutedColor
                        color: root.brightColor
                        font.pixelSize: 13
                        wrapMode: TextArea.Wrap
                        readOnly: true
                        selectByMouse: true
                        background: null
                        padding: 0
                    }
                }
            }

            Item { width: 1; height: 8 }
        }

        /* 右下角：按住改大小（和便签一样交给窗口管理器） */
        MouseArea {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: 16
            height: 16
            cursorShape: Qt.SizeFDiagCursor
            onPressed: if (root.cardWin) root.cardWin.beginResize()
        }
    }

    IconProvider { id: icons }

    /*
     * 语言下拉：**场景内的浮层**（Popup.Item），不是界面里那份共用的 DropdownMenu。
     *
     * 为什么不用共用那份：它是 popupType: Popup.Window —— 一块独立原生弹窗，
     * 而卡片自己就是"始终置顶"的顶层窗口，两块都待在置顶带里。点按钮那一下，
     * 系统激活并抬到最上面的是**卡片**，菜单就被压在它下面；这么来几回之后菜单
     * 干脆再也点不出来（用户报的"这两个选项多次点击之后点不动"就是这个）。
     * 便签那边（StickyNoteWindow::raisePopupWindow）踩的是同一个坑，它是靠反复
     * raise 弹窗绕过去的；卡片里没有原生子窗口（和主窗口的编辑区不一样），
     * 语言清单直接画在自己的场景里就够了，也就没有抢层级这回事。
     */
    Popup {
        id: langPopup

        /* 这次开的是源语言还是目标语言（决定选完写给谁） */
        property string kind: "src"
        property var items: []

        width: 190
        height: Math.min(langList.contentHeight + 8, 300)
        padding: 4
        modal: false
        focus: true
        popupType: Popup.Item
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        /* 选中一条：kind 是 "src" / "tgt"，lang 是语言名 */
        signal picked(string kind, string lang)

        /* 挂在按钮底下弹出来（放不下就往回收，绝不出卡片） */
        function openAt(kindName, list, anchor) {
            kind = kindName
            items = list
            var p = anchor.mapToItem(root, 0, anchor.height + 4)
            x = Math.max(4, Math.min(p.x, Math.max(4, root.width - width - 4)))
            y = Math.max(4, Math.min(p.y, Math.max(4, root.height - height - 4)))
            open()
        }

        onPicked: (kind, lang) => {
            if (kind === "src")
                root.setSource(lang)
            else
                root.setTarget(lang)
        }

        background: Rectangle {
            color: "#3c3f41"
            border.color: root.borderColor
            border.width: 1
            radius: 6
        }

        contentItem: ListView {
            id: langList

            clip: true
            model: langPopup.items
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar { }

            delegate: Rectangle {
                id: langRow
                required property var modelData

                width: ListView.view.width
                height: 26
                radius: 4
                color: langRow.modelData.checked ? "#2f3a44"
                                                 : (langHit.containsMouse ? root.hoverColor
                                                                          : "transparent")

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: langRow.modelData.label
                    color: langRow.modelData.checked ? root.accentColor : root.textColor
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }

                MouseArea {
                    id: langHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        /* 先收起再往外报：回调里会改语言（界面跟着重画） */
                        var which = langPopup.kind
                        var picked = langRow.modelData.label
                        langPopup.close()
                        langPopup.picked(which, picked)
                    }
                }
            }
        }
    }

    /* 打开就把光标放在输入框里，省得再点一下 */
    Component.onCompleted: {
        input.text = root.textIn
        input.forceActiveFocus()
    }
}
