pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../utils"
import "../components"

/*
 * 识别结果卡片：框选之后让大模型把图上的字读出来（要翻译就一起翻）。
 *
 * 它由 qml/screenshot/CaptureOverlay.qml 实例化，画在**选区窗口自己的场景里**
 * （不是另开一个原生窗口）—— 理由和 TranslateCard 那份注释里说的一样：
 * 选区窗口本来就铺满整块屏、还置顶，另开一个顶层窗口要跟它抢层级，弹出来
 * 经常被压在下面；画在自己场景里就没这回事，而且它必须跟着选区一起走
 * （选区一复位卡片就收起来，见 CaptureOverlay 的 resetForCapture）。
 *
 * 数据怎么来的：**这里不发请求**。发请求、认 token 都在 CaptureOverlay 里
 * （它手里才有选区那块图），这里只负责把拿到的结果摆出来：
 *   showResult(原文, 译文, 是不是译过的)  摆一份新结果
 *   showBusy(文案)                        转圈（等模型那几秒）
 *   showError(原因)                       出错
 * 卡片上那几个按钮的动作（复制 / 转成标注 / 重翻）也交给外面做 —— 它们要动的
 * 东西（剪贴板、标注模型、选区）都不在这个组件里。
 *
 * 语言下拉用 QtQuick.Controls 的 Popup.Item（场景内浮层），不是界面里共用那份
 * DropdownMenu —— 后者是独立原生弹窗，在置顶窗口里点几次就再也弹不出来
 * （TranslateCard.qml 里把这个坑写得很细，这里照它的做法）。
 */
Rectangle {
    id: card

    /* ---- 外面塞进来的：语言清单和当前的语言选择 ---- */
    property var sourceLanguages: []
    property var targetLanguages: []
    property string sourceLang: "自动检测"
    /* 空串 = 只要原文，不翻译（见底下语言下拉第一条"只识别"） */
    property string targetLang: "中文（简体）"

    /*
     * 正等着模型（外面置上 / 清掉）。卡片自己分不清"等模型"和"等界面把结果
     * 摆上来"，这两件事只有调用方知道。
     */
    property bool busy: false
    /* 状态行那句话：转圈时是进展，出错时是原因 */
    property string status: ""
    property color statusColor: card.mutedColor

    property string originalText: ""
    property string translatedText: ""

    readonly property bool translated: translatedText !== ""
    readonly property bool hasText: originalText !== "" || translatedText !== ""
    readonly property bool hasOriginal: originalText !== ""
    /*
     * 摆出来的那份结果该不该显示。**不是**直接看 hasText：识别完把卡片关掉
     * 之后 hasText 还是真的，直接绑它卡片会自己又冒出来。
     */
    property bool hasResult: false

    readonly property color cardColor:   "#2b2d30"
    readonly property color headerColor: "#33363a"
    readonly property color borderColor: "#4b4d4f"
    readonly property color textColor:   "#c8ccd1"
    readonly property color brightColor: "#e8e8e8"
    readonly property color mutedColor:  "#8a9098"
    readonly property color hoverColor:  "#3a3d41"
    readonly property color accentColor: "#4c96d8"
    readonly property color warnColor:   "#c8503c"

    /* ---- 卡片尺寸（正文那两栏是算出来的，见里面 originalHeight / translatedHeight） ---- */
    readonly property real maxBodyHeight: 200
    readonly property real originalHeight:
        Math.min(140, Math.max(26, originalTextItem.contentHeight + 2))
    readonly property real translatedHeight:
        Math.min(140, Math.max(26, translatedTextItem.contentHeight + 2))
    readonly property real bodyHeight:
        Math.min(maxBodyHeight, originalHeight + (translated ? translatedHeight + 11 : 0))
    /* 两个框相对正文顶部的偏移（不用 Column 的 margin 属性：那是 Qt5 那套，
       现在放进去会报 unknown property） */
    readonly property real originalTop: 7
    readonly property real translatedTop: originalTop + originalHeight
                                          + (originalText !== "" ? 10 : 0)

    /* 外面要做的三件事（见文件头） */
    signal copyRequested()
    signal annotateRequested()
    /* 用户在下拉里换了目标语言（空串 = 只识别）：外面据此重发一次 */
    signal targetPicked(string lang)
    /*
     * 用户按了右上角那个 ✕。
     *
     * 这一下**只喊一声，不自己动 visible** —— 这里踩过一个坑：外面是拿
     * `visible: hasResult` 绑在这个组件上的，组件里一句 `card.visible = false`
     * 会把那条绑定**打断**，之后 hasResult 再怎么变真，卡片也永远不出来了
     * （贴图那边从不显式置 true，于是"关过一次之后点「翻译」再也没反应"）。
     * 摆不摆由外面决定：截图那边显式置 visible，贴图那边清 hasResult。
     */
    signal closeRequested()

    function languageEntries(list, current) {
        var out = []
        for (var i = 0; i < (list ? list.length : 0); ++i)
            out.push({ label: list[i], checked: list[i] === current })
        return out
    }

    /* 摆一份新结果进来 */
    function showResult(original, translated) {
        card.busy = false
        card.status = ""
        card.statusColor = card.mutedColor
        card.originalText = original
        card.translatedText = translated
        card.hasResult = true
    }

    function showBusy(what) {
        card.busy = true
        card.status = what
        card.statusColor = card.mutedColor
        card.hasResult = true
    }

    function showError(reason) {
        card.busy = false
        card.status = reason
        card.statusColor = card.warnColor
        card.hasResult = true
    }

    /* 重来一次（重新识别）之前把旧的清掉，免得旧结果看着像新的 */
    function clearText() {
        card.originalText = ""
        card.translatedText = ""
        card.status = ""
        card.statusColor = card.mutedColor
        card.busy = false
    }

    width: 460
    height: header.height + body.height + footer.height + 2
    radius: 8
    color: card.cardColor
    border.color: card.borderColor
    border.width: 1
    /* 挡住底下的截图：它是贴着选区弹出来的一块面板，不能跟画面混在一起 */
    clip: true
    visible: false

    IconProvider { id: icons }

    Column {
        anchors.fill: parent
        anchors.margins: 1
        spacing: 0

        /* ---------------- 标题栏 ---------------- */
        Rectangle {
            id: header

            width: parent.width
            height: 30
            color: card.headerColor
            topLeftRadius: 7
            topRightRadius: 7

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "识别结果"
                color: card.brightColor
                font.pixelSize: 12
                font.bold: true
            }

            /* 转圈：两个点来回亮，表明"在等模型" */
            Row {
                anchors.left: parent.left
                anchors.leftMargin: 76
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3
                visible: card.busy

                Repeater {
                    model: 2

                    delegate: Rectangle {
                        required property int index

                        width: 4
                        height: 4
                        radius: 2
                        color: card.accentColor
                        opacity: 0.3

                        SequentialAnimation on opacity {
                            running: card.busy
                            loops: Animation.Infinite
                            PauseAnimation { duration: index * 150 }
                            NumberAnimation { to: 1.0; duration: 260 }
                            NumberAnimation { to: 0.3; duration: 260 }
                            PauseAnimation { duration: (1 - index) * 150 }
                        }
                    }
                }
            }

            /* 收起来（结果留着；再点一次"识别"就又出来） */
            Rectangle {
                width: 22; height: 22; radius: 4
                anchors.right: parent.right
                anchors.rightMargin: 5
                anchors.verticalCenter: parent.verticalCenter
                color: closeHit.containsMouse ? "#7a3a34" : "transparent"

                AppIcon {
                    anchors.centerIn: parent
                    provider: icons
                    kind: "close"
                    size: 12
                    tint: closeHit.containsMouse ? "#ffffff" : card.mutedColor
                }
                MouseArea {
                    id: closeHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: card.closeRequested()
                }
            }
        }

        /* ---------------- 正文：原文 / 译文 ---------------- */
        Column {
            id: body

            width: parent.width
            height: card.bodyHeight + 14

            Text {
                width: parent.width - 20
                x: 10
                text: card.busy ? "正在识别…"
                                : (card.hasText ? "" : "框选区域里没读到文字")
                color: card.mutedColor
                font.pixelSize: 11
                visible: !card.hasText
                elide: Text.ElideRight
            }

            /* 原文 */
            Flickable {
                id: originalView

                x: 8
                y: card.originalTop
                width: parent.width - 16
                height: card.hasText ? card.originalHeight : 0
                visible: height > 0
                clip: true
                contentWidth: width
                contentHeight: originalTextItem.contentHeight + 2
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThinScrollBar { }

                TextEdit {
                    id: originalTextItem

                    width: originalView.width
                    text: card.originalText
                    color: card.textColor
                    font.pixelSize: 12
                    wrapMode: TextEdit.Wrap
                    readOnly: true
                    selectByMouse: true
                    selectionColor: card.accentColor
                    selectedTextColor: "#ffffff"
                }
            }

            /* 中间那条线：有译文时才画（一眼看得出上下两段） */
            Rectangle {
                x: 8
                y: originalView.y + originalView.height + 4
                width: parent.width - 16
                height: 1
                color: card.borderColor
                visible: card.translated && card.originalText !== ""
            }

            /* 译文 */
            Flickable {
                id: translatedView

                x: 8
                y: card.translatedTop
                width: parent.width - 16
                height: card.translated ? card.translatedHeight : 0
                visible: height > 0
                clip: true
                contentWidth: width
                contentHeight: translatedTextItem.contentHeight + 2
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThinScrollBar { }

                TextEdit {
                    id: translatedTextItem

                    width: translatedView.width
                    text: card.translatedText
                    color: card.brightColor
                    font.pixelSize: 12
                    wrapMode: TextEdit.Wrap
                    readOnly: true
                    selectByMouse: true
                    selectionColor: card.accentColor
                    selectedTextColor: "#ffffff"
                }
            }
        }

        /* ---------------- 底下那排：语言 + 动作 ---------------- */
        Rectangle {
            id: footer

            width: parent.width
            height: 36
            color: card.headerColor
            bottomLeftRadius: 7
            bottomRightRadius: 7

            /* 目标语言：换一个就重发一次识别（带着翻译） */
            Rectangle {
                id: langBtn

                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 104
                height: 22
                radius: 4
                color: langHit.containsMouse ? card.hoverColor : "transparent"
                border.width: 1
                border.color: card.borderColor

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 7
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: card.targetLang === "" ? "只识别（不翻译）" : card.targetLang
                    color: card.textColor
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                AppIcon {
                    anchors.right: parent.right
                    anchors.rightMargin: 3
                    anchors.verticalCenter: parent.verticalCenter
                    provider: icons
                    kind: "chevron-down"
                    size: 11
                    tint: card.mutedColor
                }
                MouseArea {
                    id: langHit
                    objectName: "recognitionLanguage"
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: langPopup.openAt(langBtn)
                }
                AppToolTip {
                    hovered: langHit.containsMouse
                    text: "选目标语言 = 认出来顺手翻一遍；选「只识别」就只要原文"
                }
            }

            /* 状态行：转圈时的进展 / 出错原因 */
            Text {
                id: statusText

                anchors.left: langBtn.right
                anchors.leftMargin: 8
                anchors.right: actionRow.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: card.status
                color: card.statusColor
                font.pixelSize: 11
                elide: Text.ElideRight
            }

            Row {
                id: actionRow

                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                /* 有原文才给"翻译"（只有译文时没什么可翻的） */
                Rectangle {
                    width: 48
                    height: 22
                    radius: 4
                    visible: card.hasOriginal && card.targetLang !== ""
                    color: transHit.containsMouse ? card.hoverColor : "transparent"
                    border.width: 1
                    border.color: card.borderColor

                    Text {
                        anchors.centerIn: parent
                        text: "翻译"
                        color: transHit.containsMouse ? card.brightColor : card.textColor
                        font.pixelSize: 11
                    }
                    MouseArea {
                        id: transHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: card.targetPicked(card.targetLang)
                    }
                    AppToolTip { hovered: transHit.containsMouse; text: "把原文翻成上面那个语言" }
                }

                /* 把识别出来的字**当成一条文字标注**落在选区里（还能再改字号 / 拖动） */
                Rectangle {
                    width: 64
                    height: 22
                    radius: 4
                    visible: card.hasText
                    color: annotHit.containsMouse ? card.hoverColor : "transparent"
                    border.width: 1
                    border.color: card.borderColor

                    Text {
                        anchors.centerIn: parent
                        text: "加到图上"
                        color: annotHit.containsMouse ? card.brightColor : card.textColor
                        font.pixelSize: 11
                    }
                    MouseArea {
                        id: annotHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: card.annotateRequested()
                    }
                    AppToolTip { hovered: annotHit.containsMouse; text: "把这段文字作为标注贴到截图上" }
                }

                Rectangle {
                    width: 48
                    height: 22
                    radius: 4
                    visible: card.hasText
                    color: copyHit.containsMouse ? card.hoverColor : "transparent"
                    border.width: 1
                    border.color: card.borderColor

                    Text {
                        anchors.centerIn: parent
                        text: "复制"
                        color: copyHit.containsMouse ? card.brightColor : card.textColor
                        font.pixelSize: 11
                    }
                    MouseArea {
                        id: copyHit
                        objectName: "recognitionCopy"
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: card.copyRequested()
                    }
                    AppToolTip { hovered: copyHit.containsMouse; text: "复制识别出来的文字" }
                }
            }
        }
    }

    /* 目标语言下拉：场景内浮层（理由见文件头） */
    Popup {
        id: langPopup

        width: 170
        height: Math.min(langList.contentHeight + 8, 280)
        padding: 4
        modal: false
        focus: true
        popupType: Popup.Item
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        /*
         * 第一条是"只识别"：不翻译，只要图上那点字。
         * 用空串当"没有目标语言"（和 LlmClient::recognize 的约定一致）。
         */
        readonly property var entries: [{ label: "只识别（不翻译）",
                                         checked: card.targetLang === "" }]
                                       .concat(card.languageEntries(card.targetLanguages,
                                                                    card.targetLang))

        function openAt(anchor) {
            var p = anchor.mapToItem(card, 0, anchor.height + 4)
            x = Math.max(4, Math.min(p.x, Math.max(4, card.width - width - 4)))
            /* 底下放不下就翻到按钮上面去；两边都放不下就贴着上边 */
            var above = anchor.mapToItem(card, 0, 0).y - height - 4
            y = (p.y + height <= card.height - 4) ? p.y : Math.max(4, above)
            open()
        }

        background: Rectangle {
            color: "#3c3f41"
            border.color: card.borderColor
            border.width: 1
            radius: 6
        }

        contentItem: ListView {
            id: langList

            clip: true
            model: langPopup.entries
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar { }

            delegate: Rectangle {
                id: langRow
                required property var modelData

                width: ListView.view.width
                height: 24
                radius: 4
                color: langRow.modelData.checked ? "#2f3a44"
                                                 : (langRowHit.containsMouse ? card.hoverColor
                                                                             : "transparent")

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: langRow.modelData.label
                    color: langRow.modelData.checked ? card.accentColor : card.textColor
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }

                MouseArea {
                    id: langRowHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        var picked = langRow.modelData.label
                        langPopup.close()
                        /* "只识别" = 没有目标语言（空串，见 entries 的说明） */
                        card.targetPicked(picked === "只识别（不翻译）" ? "" : picked)
                    }
                }
            }
        }
    }
}
