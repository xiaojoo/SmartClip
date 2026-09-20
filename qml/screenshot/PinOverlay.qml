pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import "../utils"
import SmartClip.Globals 1.0

/*
 * 贴图窗口（"固定到桌面"之后那块浮在桌面上的图）的界面。
 *
 * 由 src/Screenshot.cpp 里的 PinWindow 用 QQuickWidget 装进一个置顶无边框
 * 原生窗口里 —— 和截图选区窗口（qml/screenshot/CaptureOverlay.qml）是同一个
 * 套路：窗口标志位、拖动、缩放这些都在 C++ 手里，QML 只画内容、只报交互。
 *
 * 为什么要从"一张静态图 + 右键菜单"改成这样：用户要的是**贴上去之后还能改**
 * —— 能划重点、能把上面的字认出来翻译掉，而静态图那套做不到。
 *
 * ---- 坐标（两套，别混） ----
 *   * 根坐标 = 窗口坐标（逻辑像素）：mouse.x/y 就是它，工具条的摆位也是它。
 *   * 贴图坐标（下面统一叫 pin 坐标，同时也是设备像素）：**图像自己的像素**。
 *     底图按 100% 显示时窗口尺寸就等于图像的逻辑尺寸，缩放 1 时两者一像素对一像素。
 *     标注的每个点都存这套坐标 —— 好处是放大缩小不会把标注甩出去，而且"存图 /
 *     复制"时直接按原图尺寸画，跟屏幕上看到的是同一份数据。
 *   两套之间只差一个 zoom：根坐标 = pin 坐标 × zoom（图像从 (0,0) 铺起、没有留边），
 *   见下面 px() / py() 两个换算函数。
 *
 * ---- 渲染必须和 C++ 合成一致 ----
 * 预览画在 Canvas 上，成品图是 C++ 用 QPainter 画的（见 Screenshot.cpp 里的
 * paintAnnotation / PinWindow::composedImage），两边的线宽、振幅、字号、折行
 * 规则**必须是同一套数** —— 不然"预览里看着好好的，存出来不一样"。约定是：
 *   * 文字：字号 size、框宽 w（0 = 跟着文字长）、框高 h、绕框中心转 rot 度；
 *   * 荧光笔 / 直线 / 波浪线 / 删除线：都当"从 (x1,y1) 到 (x2,y2) 的一条线"，
 *     荧光笔的线宽 max(3, stroke) 还要 ×2.4 且半透明（它是在"涂一层"，不是画一条线）；
 *     **另外三种（直线 / 波浪线 / 删除线）线宽 = max(1.2, 0.6×stroke)** ——
 *     细一点（3px 摆在下划线/删除线那个位置太粗，像拿马克笔划的）；
 *     波浪振幅 = min(4, 1.2×线宽)，波长 8。
 */
Rectangle {
    id: root

    /* ---- C++ 塞进来的状态（setInitialProperties，见 PinWindow 的构造函数） ---- */
    /* 这个贴图窗口（复制 / 另存 / 缩放 / 关闭都调它） */
    property var pinWin: null
    /* 底图在 image://pin/ 下的 id（带序号，绕开 QQuickPixmapCache） */
    property string imageId: ""
    /* 逻辑尺寸：窗口按它摆、标注按它裁 */
    property real viewWidth: 320
    property real viewHeight: 240
    /*
     * 外边距 / 工具栏条（C++ 的 shownSize 用同一组常量算出来的窗口尺寸推下来）：
     * 底图不再是"铺满整个窗口"，而是内缩到 (padSide, padSide, viewWidth, viewHeight)
     * 这一块"图片矩形"里；四周的 padSide 留给外投影，底下 padBottom 那条留给常驻工具栏。
     */
    property real padSide: 30
    property real padBottom: 88
    property real barGap: 10
    property real barH: 48

    /* ---- 标注 ---- */
    /*
     * 一条标注 = 一行数据。形状和文字共用一套字段（见文件头那两条约定）：
     *   { kind, x, y, x1, y1, x2, y2, w, h, rot, size, stroke, color, text }
     * kind ∈ ""（空位）/ "highlight" / "wavy" / "line" / "strike" / "text"
     *
     * 用**普通 JS 数组 + 整体重新赋值**，不用 ListModel：文字那层要靠它出委托，
     * 而 ListModel 的动态角色存不了这种"字段随 kind 变"的数据（截图那边铅笔的
     * 点列就栽过这个坑，见 CaptureOverlay 里 shapes 的说明）。
     */
    property var annotations: []
    property int selected: -1
    property int editing: -1
    /* 选中工具下，文字框里选中的那段（翻译"只翻这段"要用） */
    property string selectionText: ""

    /* ---- 图上选字（底图上认出来的字，见 PinWindow::startOcr / src/PinOcr.h） ----
     *
     * ocrLines 一行一条 { text, x, y, w, h }，坐标是**归一化**的（0~1，相对底图）：
     * 贴图能缩放，归一化之后乘一下显示尺寸就行，放大缩小都不用换坐标。
     *
     * 选中用"锚点 + 拖到的那一点"表示（selA/selAt 与 selB/selBt），和文本编辑器
     * 一个路子：行内位置用 0~1 的比例记，选中哪几个字按比例乘文字长度取整 ——
     * 这样"看到的"和"复制到的"用的是同一份算法（见 ocrSelectionRanges）。
     */
    property var ocrLines: root.pinWin ? root.pinWin.ocrLines : []
    property string ocrPick: ""          /* 现在选中的那段字（「翻这段」/ Ctrl+C 用） */
    property bool textDragging: false    /* 正按着左键拖选 */
    property int selA: -1                /* 锚点行；-1 = 没有选中 */
    property real selAt: 0
    property int selB: -1
    property real selBt: 0
    property bool textHover: false       /* 鼠标压在某行字上（光标变 I 形） */
    /* 现在该画的高亮矩形（根坐标；选中变了 / 窗口尺寸变了就重算，见 refreshOcrPick） */
    property var hlRanges: []
    /*
     * 按着 Ctrl / Alt 拖 = 挪贴图，哪怕压在字上（见 pointerDown 里那段）。
     * 「选中」是默认工具，贴图上大部分拖动都落在字上 —— 没有这个口子的话，
     * 一张全是字的截图就再也拖不动了（用户报过"固定的图片不能拖动"）。
     */
    property bool panKey: false

    /*
     * ---- 选字引擎：两个选项（工具条上「认字」那个菜单） ----
     *
     *   windows 本机 Windows 自带 OCR：离线、不用配，装上就能用（默认）
     *   ppocr   跑本机那个 PP-OCR 程序（RapidOCR / PaddleOCR，命令在设置里）
     *
     * 选哪个记在设置里（Llm.pinOcrEngine），下次贴图还是它；菜单里点一下 =
     * "换引擎 + 立刻认一遍"（点当前那个 = 重认一遍）。
     *
     * （原来还有第三项"视觉模型"，撤了：那条路回来的框是模型估的、位置会偏，
     * 而"选字"要的就是位置准 —— 留着只会让人选错。）
     */
    readonly property var ocrEngineList: [
        { key: "windows", label: "Windows 自带", tip: "离线、不用配，装上就能用" },
        { key: "ppocr",   label: "PP-OCRv6",     tip: "跑本机那个 OCR 程序（RapidOCR / PaddleOCR）" }
    ]
    property bool ocrMenuOpen: false
    readonly property bool ocrRunning: root.pinWin ? root.pinWin.ocrBusy : false
    /* 认字这条路的提示（"图上没认出字" / "PATH 里找不到 python" 这类），提示条上显示 */
    readonly property string ocrStatus: root.pinWin ? root.pinWin.ocrMessage : ""

    /* ---- 当前工具和它的样式 ---- */
    /*
     * ""/"select" = 只选中 / 改字 / 拖窗口；其余四种是在图上划。
     *
     * 默认给 "select" 而不是 ""：这张贴图上的字多半是要**选来改、选来翻**的
     * （用户报的"不能选中"就是默认什么都没有、点上去毫无反应）。划线那几个
     * 工具是"想划的时候才去点"。
     *
     * 原来还有一个 "text"（在图上落文字），**撤了**（用户说不要这个功能）：
     * 贴图上的文字标注现在只有一条来路 —— 识别卡片上的「加到图上」（认出来的字
     * 落成一条文字标注，接着能改字号 / 拖动）。kind 仍然是 "text"，渲染和编辑
     * 那套一个字没动。
     */
    property string tool: "select"
    property int fontSize: 16
    property int strokeWidth: 3
    property color annotColor: "#ff3b30"
    readonly property bool shapeTool: tool === "highlight" || tool === "wavy"
                                       || tool === "line" || tool === "strike"

    /* ---- 正在拖的那一笔 ---- */
    property bool drawing: false
    property string drawKind: ""
    property real drawX1: 0
    property real drawY1: 0
    property real drawX2: 0
    property real drawY2: 0

    /* 拖动已有的文字框（选中工具下按住左上角那个小圆点） */
    property bool moving: false
    property int moveIndex: -1
    property real moveDX: 0
    property real moveDY: 0

    /*
     * 拖整张贴图（没拿工具时的默认动作，见 pointerDown / pointerMove）。
     * pressX/Y 是按下那一刻的根坐标，用来判"是拖了还是随手点了一下"。
     */
    property bool panning: false
    property real pressX: 0
    property real pressY: 0
    /*
     * 鼠标**左键**这会儿按着吗 —— 拖动（拖整张贴图 / 拖文字框）只认它。
     *
     * 为什么单立一个：hand 那个 MouseArea 开着 hoverEnabled，它的 positionChanged
     * 在"没按键、只是从图上划过"时**也会发**。原来 pointerMove 只看"鼠标挪了多远"，
     * 于是鼠标一移动就被当成"在拖整张贴图"，贴图跟着鼠标乱跑（用户报的）。
     * 现在按下左键才算、松开就清掉，鼠标光划过什么都不动。
     */
    property bool leftDown: false

    /* ---- 缩放 / 界面 ---- */
    property real zoom: 1.0
    property bool ocrBusy: false
    property string ocrTarget: Llm.defaultTarget
    readonly property var ocrTargets: Llm.targetLanguages
    /* 等模型的那口（对不上号就是上一次的，丢掉） */
    property string ocrToken: ""
    /*
     * 鼠标在不在贴图上（HoverHandler 喂它，见下面）。
     *
     * 现在工具条常驻、不再靠 hover 淡入淡出，hovered 只留着给光标形状判断用。
     */
    property bool hovered: false

    readonly property color accent: "#4c96d8"
    readonly property color borderColor: "#4b4d4f"

    color: "transparent"
    /* 鼠标进没进来（决定工具条淡不淡） */
    HoverHandler { onHoveredChanged: root.hovered = hovered }

    /* ---- pin 坐标 <-> 根坐标 ---- */
    function px(pinX) { return pinX * root.zoom }
    function py(pinY) { return pinY * root.zoom }
    function pinX(rootX) { return rootX / Math.max(0.05, root.zoom) }
    function pinY(rootY) { return rootY / Math.max(0.05, root.zoom) }

    /* 标注数组改一下：整体重新赋值（原地改不触发绑定，见文件头） */
    function setAnnotations(list) {
        root.annotations = list.slice(0)
        canvas.repaint()
    }

    function addAnnotation(entry) {
        const list = root.annotations.slice(0)
        list.push(entry)
        root.setAnnotations(list)
        return list.length - 1
    }

    function removeAt(index) {
        if (index < 0 || index >= root.annotations.length)
            return
        const list = root.annotations.slice(0)
        list.splice(index, 1)
        if (root.editing === index)
            root.editing = -1
        else if (root.editing > index)
            root.editing--
        if (root.selected === index)
            root.selected = -1
        else if (root.selected > index)
            root.selected--
        root.setAnnotations(list)
    }

    /* 文字改了：整条换掉（普通数组没法原地改某一项还让绑定重算） */
    function setTextAt(index, text) {
        if (index < 0 || index >= root.annotations.length)
            return
        const list = root.annotations.slice(0)
        const item = list[index]
        if (!item || item.text === text)
            return
        list[index] = Object.assign({}, item, { text: text })
        root.annotations = list
    }

    /* 文字框挪了 / 改尺寸了：同上，整条换掉 */
    function patchAt(index, patch) {
        if (index < 0 || index >= root.annotations.length)
            return
        const list = root.annotations.slice(0)
        list[index] = Object.assign({}, list[index], patch)
        root.annotations = list
        canvas.repaint()
    }

    function undo() {
        if (root.annotations.length === 0)
            return
        root.removeAt(root.annotations.length - 1)
    }

    /*
     * 选工具：再点同一个 = 收起（回到"选中"）。
     * 切走之前先把正在编辑的那条收掉，免得切了工具还在打字。
     *
     * **图上选着字的时候例外**：这四个形状工具不切工具，而是把这种标注直接贴到
     * 选中的那几段字上（见 applyShapeToSelection）—— 用户要的"划词即标注"：
     * 选中了行，点一下荧光笔/波浪线/直线/删除线，就加在那一行上，不用自己去拖。
     */
    function pickTool(name) {
        root.editing = -1
        if (root.applyShapeToSelection(name))
            return
        root.tool = (root.tool === name) ? "" : name
        root.closeMenus()
    }

    /*
     * 把一种标注贴到"图上选中的那几段字"上：一段一条，左右到选中的两端（和蓝色那层
     * 划词高亮**同一段区间**，所以"看到的"就是"划上的"）。
     *
     * 位置按工具分：
     *   荧光笔         —— 行框那么高的半透明色带，盖着字（0.35，还看得清）
     *   波浪线 / 直线  —— 贴着行框**底边**（下划线）
     *   删除线         —— 穿过行框**中间**
     * 颜色 / 粗细仍用工具现在的那套（和手拖出来的一模一样）。
     *
     * "带子多高"借 stroke 表达：画的时候带子高 = stroke * 2.4（见 canvas.paintShape），
     * 所以要让带子正好等于行高，stroke 就得填 行高 / 2.4 —— 标注的字段不用加新的，
     * 存 / 撤销 / 复制那条路也一个字不用改。
     *
     * 贴完**把选中收掉**：蓝色那层高亮和刚贴的标注叠在一起看不清；收掉之后工具也没
     * 被切走（还能接着选字 / 拖一笔），更不会出现"再点一下又贴一遍"。
     *
     * 返回 true = 贴上了（调用方就不用再走"切工具"那条路）。
     */
    function applyShapeToSelection(name) {
        if (name !== "highlight" && name !== "wavy" && name !== "line" && name !== "strike")
            return false
        const ranges = root.ocrSelectionRanges()
        if (ranges.length === 0)
            return false
        const color = root.annotColor.toString()
        const stroke = root.strokeWidth
        for (let i = 0; i < ranges.length; ++i) {
            const r = ranges[i]
            /* 高亮那层是根坐标，标注存的是图坐标（画布会按缩放再放一次） */
            const x0 = root.pinX(r.x0)
            const x1 = root.pinX(r.x1)
            const y0 = root.pinY(r.y0)
            const y1 = root.pinY(r.y1)
            const rowH = Math.max(6, y1 - y0)
            const middle = (y0 + y1) / 2
            let y = y1                 /* 波浪线 / 直线：贴行底边 */
            let width = stroke
            if (name === "highlight") {
                y = middle             /* 色带居中，盖住整个行框 */
                width = rowH / 2.4     /* 见上：这样画出来正好是行框那么高 */
            } else if (name === "strike") {
                y = middle             /* 删除线：穿行中间 */
            }
            root.addAnnotation({ kind: name, x1: x0, y1: y, x2: x1, y2: y,
                                 stroke: width, color: color })
        }
        root.clearOcrPick()
        return true
    }

    /*
     * 「翻这段」/ Ctrl+C 该用哪段字：**图上选中的（OCR）优先**，没有才是文字框里
     * 选中的那段（那是手打的标注字）。所以这个属性是"两路合一路"的口子。
     */
    readonly property string pickedText: root.ocrPick !== "" ? root.ocrPick : root.selectionText

    /* 这张贴图上有选中的文字吗（工具条上「翻这段」的可用性看它） */
    readonly property bool hasSelection: root.pickedText.trim() !== ""

    /* ================= 鼠标：画形状 / 落文字 / 拖窗口 / 选中 ================= */

    /*
     * 鼠标按下。
     *
     * **没拿工具的时候不是"什么都不做"**，而是"准备拖这张贴图"（用户报的"固定的
     * 图片不能拖动"就是少了这一段）：按住**左键**拖动 = 把贴图挪到别处（整块一起
     * 挪，见 pointerMove）。判据是"按下到松开挪了几像素"（见 panning / pressX），
     * 不是定时器。
     *
     * 单击（不拖）**什么都不做**，左键 / 右键都一样：单击只当"摸一下这张图"，
     * 不当"收工"。原来这里写的是"随手点一下就把贴图关掉"（这类贴图工具的老习惯），
     * 但用户点上去多半只是想选中 / 改字 / 碰一下看看，误关的代价太大（用户报的
     * "怎么左右键点一下就关掉了"）。关掉走工具条上那个「关闭」或者 Esc。
     */
    function pointerDown(x, y, right, mods) {
        /* 「认字」菜单开着时，点别处 = 收起菜单（这一下不干别的） */
        if (root.ocrMenuOpen) {
            root.ocrMenuOpen = false
            return
        }
        root.leftDown = !right        /* 拖动只认左键，见 leftDown */
        root.panKey = mods !== undefined
                      && ((mods & Qt.ControlModifier) !== 0 || (mods & Qt.AltModifier) !== 0)
        root.pressX = x
        root.pressY = y
        const wx = root.pinX(x)
        const wy = root.pinY(y)

        if (right) {
            /* 右键：收起当前工具，顺带把选中（标注的 + 图上选的字）都取消掉 */
            root.tool = ""
            root.selected = -1
            root.clearOcrPick()
            return
        }

        if (root.shapeTool) {
            root.commitEditing()
            root.drawKind = root.tool
            root.drawX1 = wx
            root.drawY1 = wy
            root.drawX2 = wx
            root.drawY2 = wy
            root.drawing = true
            return
        }

        /*
         * 图上选字（见上面那一节）：拿「选中」/ 没拿工具时，按在字上就交给文字层
         * 拖选，不再挪窗口 —— 顺手把标注的选中收掉（点的是图上的字，不是标注）。
         *
         * 想挪贴图：拖**空白处**（行缝、图边上没字的地方），或者按住 Ctrl/Alt 拖
         * （panKey）—— 一张全是字的截图否则就再也拖不动了。
         */
        if (!root.panKey && root.ocrSelectStart(x, y)) {
            root.commitEditing()
            root.selected = -1
            return
        }

        /* 选中 / 没拿工具：点空白处取消选中（图上选的字也一起清掉） */
        root.commitEditing()
        root.selected = -1
        root.clearOcrPick()
    }

    function pointerMove(x, y) {
        /*
         * 左键没按着（鼠标只是从图上划过 / 按的是右键）：什么都不做。
         * 默认不动 —— 见 leftDown 那段说明。
         */
        if (!root.leftDown)
            return
        /* 正拖着选字：只更新"拖到的那一点" */
        if (root.textDragging) {
            root.ocrSelectMove(x, y)
            return
        }
        if (root.shapeDrag) {
            root.drawX2 = root.pinX(x)
            root.drawY2 = root.pinY(y)
            canvas.repaint()
            return
        }
        if (root.moving) {
            const item = root.annotations[root.moveIndex]
            if (!item)
                return
            root.patchAt(root.moveIndex, { x: root.pinX(x) - root.moveDX,
                                           y: root.pinY(y) - root.moveDY })
            return
        }
        /* 拖着整张贴图走（没拿工具时的默认动作） */
        if (!root.panning) {
            if (Math.hypot(x - root.pressX, y - root.pressY) < 4)
                return          /* 还没挪够：先不当成拖动，免得手一抖就移位 */
            root.panning = true
            if (root.pinWin)
                root.pinWin.beginDrag()
            return
        }
        /*
         * 已经在拖了：每一步都把**鼠标的全局坐标**喂给 C++。
         *
         * 传坐标而不是让 C++ 自己读 QCursor::pos()：那个是**设备像素**，
         * move() / pos() 是**逻辑像素**，高 DPI 下差一个缩放比 —— 照它算位移
         * 窗口要么不动要么乱跳（实测 150% 下算出来位移是 0，就是用户报的
         * "还是不能拖动"）。mapToGlobal 出来的就是对得上的那一套。
         */
        if (root.pinWin) {
            const at = hand.mapToGlobal(x, y)
            root.pinWin.dragMoveTo(at.x, at.y)
        }
    }

    function pointerUp() {
        root.leftDown = false
        /* 松手 = 选完了：高亮和选中的字都留着（点空白 / 右键才清） */
        if (root.textDragging) {
            root.textDragging = false
            return
        }
        if (root.drawing) {
            root.drawing = false
            root.commitShape()
            return
        }
        /*
         * 没在划线：把"在拖整张贴图"那套状态收掉，**到此为止**。
         *
         * 单击（不拖）不关窗口 —— 左键 / 右键都一样，见 pointerDown 那段说明。
         * 关掉只走工具条上那个「关闭」或者 Esc。
         */
        root.panning = false
        root.moving = false
        root.moveIndex = -1
        if (root.pinWin)
            root.pinWin.endDrag()
    }

    readonly property bool shapeDrag: drawing

    /*
     * 落一笔形状。太短的（手抖点一下）丢掉 —— 免得留下一道看不见的划痕。
     * 荧光笔 / 直线 / 波浪线 / 删除线都是"两个点定一条线"，所以这里只有一种形状。
     */
    function commitShape() {
        const dx = root.drawX2 - root.drawX1
        const dy = root.drawY2 - root.drawY1
        if (Math.hypot(dx, dy) >= 6) {
            root.addAnnotation({ kind: root.drawKind, x1: root.drawX1, y1: root.drawY1,
                                 x2: root.drawX2, y2: root.drawY2, stroke: root.strokeWidth,
                                 color: root.annotColor.toString() })
        }
        root.drawKind = ""
        canvas.repaint()
    }

    /* 松开左键时如果只是点了一下（没拖），把空框收掉（落字那条路留下的空框） */
    function commitEditing() {
        if (root.editing < 0)
            return
        const index = root.editing
        root.editing = -1
        const item = root.annotations[index]
        if (item && item.kind === "text" && (item.text === undefined || item.text === ""))
            root.removeAt(index)
    }

    /* 拖动整条文字框（选中工具下按住框身空白处，见 beginMove） */
    function beginMove(index, rootX, rootY) {
        if (index < 0 || index >= root.annotations.length)
            return
        const item = root.annotations[index]
        root.selected = index
        root.moving = true
        root.moveIndex = index
        root.leftDown = true        /* 拖文字框也是"按住左键"，见 leftDown */
        /*
         * 记的是"按下点相对框左上角的偏移"：这样拖起来不跳（不记的话，
         * 一按下去框就窜到鼠标底下）。
         */
        root.moveDX = root.pinX(rootX) - item.x
        root.moveDY = root.pinY(rootY) - item.y
    }

    /* 一张贴图上有没有选中的文字（「翻这段」的可用性看它；由文字层的 TextEdit 上报） */
    function reportSelection(text) {
        root.selectionText = text ? text : ""
    }

    /* ================= 图上选字（OCR 文字层，见 src/PinOcr.h） ================= */

    /*
     * 归一化坐标 -> 根坐标。
     * viewWidth/viewHeight 已经是"底图 × 缩放"了，所以乘一下就落在窗口坐标系里，
     * 放大缩小、改窗口大小都不用重算（行本来就是归一化的）。
     */
    function ocrPx(nx) { return nx * root.viewWidth }
    function ocrPy(ny) { return ny * root.viewHeight }

    /*
     * 这个点压在哪一行字上（没有就是 -1）。
     *   near=true：竖直方向在"行与行的缝里"时，按最近的那一行算 —— 拖选时鼠标常常
     *   落在缝里，按最近行走才不会选到一半就断。
     */
    function ocrLineAt(x, y, near) {
        const n = root.ocrLines.length
        if (n === 0 || root.viewWidth <= 1 || root.viewHeight <= 1)
            return -1
        const vx = x / root.viewWidth
        const vy = y / root.viewHeight
        let best = -1
        let bestGap = 1e9
        for (let i = 0; i < n; ++i) {
            const line = root.ocrLines[i]
            if (!line)
                continue
            const inX = vx >= line.x - 0.006 && vx <= line.x + line.w + 0.006
            const inY = vy >= line.y && vy <= line.y + line.h
            if (inX && inY)
                return i
            if (near && inX) {
                const gap = vy < line.y ? (line.y - vy) : (vy - (line.y + line.h))
                if (gap >= 0 && gap < bestGap) {
                    bestGap = gap
                    best = i
                }
            }
        }
        /* 缝里也别太远：超过一个半行高就不算压着字 */
        if (best >= 0) {
            const h = root.ocrLines[best].h || 0.02
            if (bestGap <= h * 1.5)
                return best
        }
        return -1
    }

    /* 这个点落在这一行的哪个比例上（0~1，左端 0、右端 1） */
    function ocrT(index, x) {
        const line = root.ocrLines[index]
        if (!line || line.w <= 1e-6)
            return 0
        const vx = x / Math.max(1, root.viewWidth)
        return Math.max(0, Math.min(1, (vx - line.x) / line.w))
    }

    /*
     * 选中的那几段（**根坐标**的矩形）：首尾行按比例切，中间整行。
     *
     * 高亮（ocrCanvas 画的就是它）和"选出来的字"（ocrSelectionText）共用这一段
     * 区间 —— 分成两份写的话，迟早出现"看到的和复制到的不一样"。
     */
    function ocrSelectionRanges() {
        const out = []
        const n = root.ocrLines.length
        if (root.selA < 0 || root.selB < 0 || n === 0)
            return out
        let a = root.selA, b = root.selB, at = root.selAt, bt = root.selBt
        if (a > b || (a === b && at > bt)) {
            const li = a; a = b; b = li
            const lt = at; at = bt; bt = lt
        }
        for (let i = Math.max(0, a); i <= b && i < n; ++i) {
            const line = root.ocrLines[i]
            if (!line)
                continue
            const x0 = root.ocrPx(line.x + ((i === a) ? at : 0) * line.w)
            const x1 = root.ocrPx(line.x + ((i === b) ? bt : 1) * line.w)
            out.push({ x0: Math.min(x0, x1), x1: Math.max(x0, x1),
                       y0: root.ocrPy(line.y), y1: root.ocrPy(line.y + line.h) })
        }
        return out
    }

    /* 按行把选中的字拼出来（首尾行按比例切，中间整行；行与行之间用换行接） */
    function ocrSelectionText() {
        const n = root.ocrLines.length
        if (root.selA < 0 || root.selB < 0 || n === 0)
            return ""
        let a = root.selA, b = root.selB, at = root.selAt, bt = root.selBt
        if (a > b || (a === b && at > bt)) {
            const li = a; a = b; b = li
            const lt = at; at = bt; bt = lt
        }
        const parts = []
        for (let i = Math.max(0, a); i <= b && i < n; ++i) {
            const item = root.ocrLines[i]
            const text = (item && item.text) ? item.text : ""
            const from = (i === a) ? Math.round(at * text.length) : 0
            const to = (i === b) ? Math.round(bt * text.length) : text.length
            parts.push(text.slice(Math.min(from, to), Math.max(from, to)))
        }
        return parts.join("\n")
    }

    /* 选中变了：更新 ocrPick（给「翻这段」/ Ctrl+C）和高亮矩形（hlRanges） */
    function refreshOcrPick() {
        root.ocrPick = root.ocrSelectionText()
        root.hlRanges = root.ocrSelectionRanges()
    }

    /* 清掉选中（点空白 / 右键 / 重新认字 / 换了工具） */
    function clearOcrPick() {
        if (root.selA < 0 && root.ocrPick === "")
            return
        root.selA = -1
        root.selB = -1
        root.selAt = 0
        root.selBt = 0
        root.textDragging = false
        root.refreshOcrPick()
    }

    /* 按下：这一下压在字上就开始拖选（返回 true = 这一下归文字层，不挪窗口） */
    function ocrSelectStart(x, y) {
        if (root.ocrLines.length === 0)
            return false
        const i = root.ocrLineAt(x, y, false)
        if (i < 0)
            return false
        root.selA = i
        root.selAt = root.ocrT(i, x)
        root.selB = i
        root.selBt = root.selAt
        root.textDragging = true
        root.refreshOcrPick()
        return true
    }

    /* 拖着：把"拖到的那一点"更新掉 */
    function ocrSelectMove(x, y) {
        if (!root.textDragging)
            return
        const i = root.ocrLineAt(x, y, true)
        if (i < 0)
            return
        root.selB = i
        root.selBt = root.ocrT(i, x)
        root.refreshOcrPick()
    }

    /* 鼠标只是划过：只为把光标换成 I 形（不选、也不动别的） */
    function updateTextHover(x, y) {
        const on = root.ocrLineAt(x, y, false) >= 0
        if (on !== root.textHover)
            root.textHover = on
    }

    /* ================= 选字引擎（两个选项，见上面 ocrEngineList） ================= */

    /* 这个引擎现在能不能用：空串 = 能用；否则一句"为什么不能" */
    function ocrEngineProblem(key) {
        if (key === "ppocr")
            return root.pinWin ? root.pinWin.ocrRunnerProblem(Llm.pinOcrRunner)
                               : "贴图窗口还没起来"
        return (root.pinWin && root.pinWin.ocrAvailable)
               ? "" : "这台机器上没有 Windows OCR 语言包"
    }

    /*
     * 认一遍：按引擎分两条路（都交给 C++：一个进程内、一个跑本机程序）。
     *
     * **顺手把选择记下来**（Llm.pinOcrEngine）：这里以前只拿新引擎跑一遍、
     * 不写设置，于是菜单里那个勾号不动、下次开贴图又回老引擎 —— 看起来就是
     * "这两行点了没反应，只有设置里能换"。点菜单 = 选它 + 用它认一遍。
     */
    function runOcr(key) {
        const engine = (key === undefined || key === null || key === "")
                       ? Llm.pinOcrEngine : String(key)
        root.ocrMenuOpen = false
        root.clearOcrPick()
        if (Llm.pinOcrEngine !== engine)
            Llm.pinOcrEngine = engine
        if (!root.pinWin)
            return
        root.pinWin.startOcr(engine, Llm.pinOcrRunner)
    }

    /* 自检：认字菜单开没开 / 换引擎 */
    function testOcrMenu(open) {
        root.ocrMenuOpen = open === undefined ? true : !!open
        return root.ocrMenuOpen
    }
    /* 自检：这条"引擎能不能用"的判据（空串 = 能用） */
    function testEngineProblem(key) { return root.ocrEngineProblem(key) }
    /* 自检：菜单里那两个引擎（key / 名字 / 能不能用），界面画的就是这一份数据 */
    function testOcrEngines() {
        const out = []
        for (let i = 0; i < root.ocrEngineList.length; ++i) {
            const item = root.ocrEngineList[i]
            out.push({ key: item.key, label: item.label,
                       problem: root.ocrEngineProblem(item.key) })
        }
        return out
    }
    /* 自检：菜单里真画出来几行（标题 + 两个引擎各一行） */
    function testOcrMenuRows() { return ocrMenuColumn.children.length }

    /*
     * 自检：菜单里某个引擎那一行**在根坐标里的中心点**（自检要往那儿送一次真鼠标
     * 点击，量"点得到点不到"）。
     *
     * 走 Repeater::itemAt —— 界面自己那份数据，比在外面按 objectName 满地找控件可靠
     * （delegate 里的 objectName 在这里根本查不到，试过了）。
     */
    function testOcrRowCenter(key) {
        for (let i = 0; i < ocrEngineRepeater.count; ++i) {
            const item = ocrEngineRepeater.itemAt(i)
            if (!item || root.ocrEngineList[i].key !== key)
                continue
            const p = item.mapToItem(root, item.width / 2, item.height / 2)
            return { found: true, x: p.x, y: p.y, w: item.width, h: item.height,
                     menuOpen: root.ocrMenuOpen }
        }
        return { found: false }
    }

    /*
     * 自检：在图上从 (x1,y1) 拖到 (x2,y2)，返回选出来的那段字。
     *
     * 走的是界面上那条路（pointerDown -> pointerMove -> pointerUp），不是直接调
     * ocrSelectStart —— 直接调就量不到"鼠标到底接上没有"。
     */
    function testOcrDrag(x1, y1, x2, y2) {
        root.pointerDown(x1, y1, false)
        root.pointerMove(x2, y2)
        root.pointerUp()
        return root.ocrPick
    }

    /* 自检：现在选中的那段字（没有就是空串） */
    function testOcrPick() { return root.ocrPick }

    /* 自检：现在该画的那几个高亮矩形（根坐标）—— 量"高亮算得出来没有" */
    function testOcrRanges() { return root.hlRanges }

    /* 第一次显示就把字认掉（用设置里选的那个引擎）。认完 QML 这边的 ocrLines
       自己就来了（绑在 pinWin.ocrLines 上）。两个引擎都不能用的机器上什么都不
       发生，界面照旧。 */
    Component.onCompleted: root.runOcr(Llm.pinOcrEngine)

    /* 行变了（刚认完 / 重新认过）：旧的选中作废（行号对不上了） */
    onOcrLinesChanged: root.clearOcrPick()
    /* 缩放 / 窗口尺寸变了：高亮是按根坐标算的，得重算（行本身是归一化的） */
    onViewWidthChanged: root.hlRanges = root.ocrSelectionRanges()
    onViewHeightChanged: root.hlRanges = root.ocrSelectionRanges()

    /* ================= 识别 / 翻译 ================= */

    /*
     * 翻译整张贴图：把**成品图**（底图 + 已经在上面划的那些）交给视觉模型，
     * 认出来的字和译文摆进卡片。走的是和截图识别同一条路（Llm.recognize）。
     */
    function translateAll() {
        if (root.ocrBusy)
            return
        const image = root.pinWin ? root.pinWin.composedImageUrl() : ""
        if (image === "") {
            recognitionCard.showError("这张贴图取不到图")
            return
        }
        root.commitEditing()
        recognitionCard.clearText()
        recognitionCard.showBusy(root.ocrTarget === "" ? "正在识别…" : "正在识别并翻译…")
        recognitionCard.targetLang = root.ocrTarget
        root.ocrBusy = true
        root.ocrToken = Llm.recognize(image, root.ocrTarget, "自动检测")
    }

    /* 只翻"选中的那段文字"（工具条上「翻这段」）：文字已经在手里了，不用再认图 */
    function translateSelection() {
        const text = root.pickedText.trim()
        if (text === "" || root.ocrBusy)
            return
        recognitionCard.clearText()
        recognitionCard.showBusy("正在翻译选中的那段…")
        recognitionCard.targetLang = root.ocrTarget === "" ? Llm.defaultTarget : root.ocrTarget
        root.ocrBusy = true
        root.ocrToken = Llm.translate(text, recognitionCard.targetLang, "自动检测")
    }

    /* 换目标语言：有原文就重来一次（整张图那条路重发图；只翻选段那条重翻文字） */
    function retranslate(lang) {
        root.ocrTarget = lang
        if (lang !== "")
            Llm.defaultTarget = lang
        recognitionCard.targetLang = lang
        if (root.lastWasSelection)
            root.translateSelection()
        else
            root.translateAll()
    }
    /* 上一次请求是哪条路来的（决定"换语言"该重发什么） */
    property bool lastWasSelection: false

    /* 模型的回复到了：摆进卡片（带翻译的回来是"原文 ---- 译文"，抠出原文那一栏） */
    function applyRecognition(text, wantedTranslate) {
        root.ocrBusy = false
        root.ocrToken = ""
        const body = String(text === undefined ? "" : text)
        if (body === "") {
            recognitionCard.showError("模型没有认出文字")
            return
        }
        if (!wantedTranslate) {
            recognitionCard.showResult(body, "")
            return
        }
        const original = Llm.ocrOriginal(body)
        recognitionCard.showResult(original, original === body ? "" : body)
    }

    function copyRecognition() {
        const card = recognitionCard
        if (!card.hasText)
            return
        const text = (card.translated && card.originalText !== "")
                         ? card.originalText + "\n\n" + card.translatedText
                         : card.originalText
        if (root.pinWin)
            root.pinWin.copyTextToClipboard(text)
    }

    /*
     * Ctrl+C：选了字（图上选的，或文字框里选的）就复制那段字；没选就复制整张贴图
     * （成品图，和工具条上那个「复制」同一个）。
     */
    function copyPicked() {
        if (!root.pinWin)
            return
        if (root.pickedText.trim() !== "") {
            root.pinWin.copyTextToClipboard(root.pickedText)
            return
        }
        root.pinWin.copyResult()
    }

    /*
     * 识别出来的字落到贴图上：落成一条**可编辑**的文字标注（贴图上唯一还能产生
     * 文字标注的口子 —— 原来工具条上那个「文字」按钮撤了，见 tool 那段说明）。
     * 落下来就能选中、改字号、拖动、编辑。
     */
    function annotateRecognition() {
        const card = recognitionCard
        const text = card.originalText !== "" ? card.originalText : card.translatedText
        if (text === "")
            return
        const index = root.addAnnotation({ kind: "text", x: 12, y: 12, w: 0, h: 0, rot: 0,
                                           size: root.fontSize,
                                           color: root.annotColor.toString(), text: text })
        root.selected = index
        root.editing = -1
        root.tool = "select"
    }

    function closeMenus() { /* 贴图这边没有弹出面板；留着是为了和选区窗口同名同义 */ }

    /* ================= 自检入口（见 src/SelfTest.cpp） ================= */
    function testTool(name) { root.pickTool(name); return root.tool === name }

    /*
     * 自检：**没拿工具时在图上点一下**。
     *   - 挪了几像素 -> 是"拖整张贴图"；返回 "pan"
     *   - 原地不挪   -> 什么都不做（不关窗口）；返回 "idle"
     * C++ 那边拿它钉"固定的图片能拖动"和"点一下不关"这两条（用户报过
     * "固定的图片不能拖动"，也报过"怎么点一下就关掉了"）。
     */
    function testTap(dx, dy) {
        root.pointerDown(30, 30, false)
        const drew = root.drawing
        root.pointerMove(30 + dx, 30 + dy)
        const panned = root.panning
        root.pointerUp()
        return drew ? "draw" : (panned ? "pan" : "idle")
    }

    /*
     * 自检：**右键**在图上点一下（不拖）。
     *
     * 以前这一下也会把贴图关掉 —— pointerUp 里那条"随手点一下收工"没分左右键。
     * 现在左右一样：只收起工具、取消选中，窗口留着（见 pointerDown 的说明）。
     */
    function testRightTap() {
        root.pointerDown(30, 30, true)
        root.pointerMove(30, 30)
        root.pointerUp()
        return root.tool
    }

    /*
     * 自检：鼠标**只是从图上划过**（没按任何键）。
     *
     * 一条都不该发生：不该被当成"在拖整张贴图"，窗口也不该动。
     * 用户报的"鼠标在固定的图上移动，图就胡乱移动"就是这么来的 —— hand 那个
     * MouseArea 开着 hoverEnabled，划过时的 positionChanged 一样会进来。
     */
    function testHoverMove(dx, dy) {
        root.pointerMove(30, 30)
        root.pointerMove(30 + dx, 30 + dy)
        const panned = root.panning
        /* 只是模拟"划过"，本来就没有按下那一下；把状态摆平就行 */
        if (panned && root.pinWin)
            root.pinWin.endDrag()
        root.panning = false
        return panned
    }

    /*
     * 自检：走一遍**真正的拖动**（按下 -> 拖 -> 松开）。
     *
     * 和 testTap 的区别：那个只量判定（是拖还是点），这个真把三个回调喂下去
     * —— C++ 那边 dragMove 会按鼠标位移搬窗口。少了后面那一步就是"判定说在拖，
     * 窗口一动不动"（用户报的"还是不能拖动"就是这个）。
     */
    function testDragBy(dx, dy) {
        root.pointerDown(30, 30, false)
        root.pointerMove(30 + dx, 30 + dy)
        root.pointerMove(30 + dx * 2, 30 + dy * 2)   /* 再挪一步，像真的在拖 */
        root.pointerUp()
        return true
    }

    /*
     * 自检：只走 **C++ 自己搬窗口** 那一步（不碰窗口管理器那个 startSystemMove）。
     *
     * 为什么要单独一个口：拖动有两条路（窗口管理器接手 / 自己搬），而"自己搬"
     * 这条是**唯一能在自检里量到的**。真跑 startSystemMove 的话，它要么把窗口
     * 交给系统（进程内量不到），要么返回成功却什么都不做 —— 后者正是用户碰到的
     * 情况，这条测试能把两条路分开看。
     *
     * 喂的是**全局坐标**（和界面上 mapToGlobal 那一套一致）：先记锚点，再挪
     * 30,20 —— 和真拖一下等价。
     */
    function testManualDragOnly(dx, dy) {
        if (!root.pinWin)
            return false
        root.panning = true
        root.pinWin.beginDrag()
        const anchor = root.pinWin.x + 30
        const anchorY = root.pinWin.y + 30
        root.pinWin.dragMoveTo(anchor, anchorY)
        root.pinWin.dragMoveTo(anchor + 1, anchorY + 1)      /* 先挪 1px 把锚点钉住 */
        root.pinWin.dragMoveTo(anchor + dx, anchorY + dy)
        root.pinWin.endDrag()
        root.panning = false
        return true
    }

    /*
     * 划一笔。**必须带上工具名**：pickTool 是"再点一下同一个就收起"的语义，
     * 自检里连着划两笔而不换工具的话，第二笔会把工具收掉、什么也划不出来
     * （自检第一版就栽在这儿）。
     */
    function testDraw(name, x1, y1, x2, y2) {
        root.tool = name
        const before = root.annotations.length
        root.pointerDown(x1, y1, false)
        root.pointerMove(x2, y2)
        root.pointerUp()
        root.tool = ""
        return root.annotations.length > before
    }
    function testAddText(x, y, text) {
        const index = root.addAnnotation({ kind: "text", x: x, y: y, w: 0, h: 0, rot: 0,
                                           size: root.fontSize,
                                           color: root.annotColor.toString(), text: text })
        return index
    }
    function testEditText(index, text) { root.setTextAt(index, text); return true }
    function testSelect(index) { root.selected = index; return root.selected === index }
    /*
     * 工具条几何的自检口（调排版时量它：宽高不对就说明里面折行 / 绑成环了）。
     * 实测过一次 59×492 —— 所有键竖成一列、整条工具条跑到窗口外，见 bar.width 的说明。
     */
    function barState() {
        return { x: bar.x, y: bar.y, width: bar.width, height: bar.height,
                 flowWidth: barFlow.width, flowHeight: barFlow.implicitHeight,
                 singleRowWidth: barFlow.singleRowWidth,
                 /* 工具条现在常驻、摆在图片下沿外面（这条 imageBottom 给自检判"在图外"） */
                 imageBottom: root.padSide + root.viewHeight,
                 hovered: root.hovered, shown: true,
                 opacity: bar.opacity, enabled: bar.enabled, visible: bar.visible }
    }

    /* 自检：最后一条标注的原始字段 + **实际画出来的粗细**（"细一点"那条要看它） */
    function testLastShape() {
        if (root.annotations.length === 0)
            return null
        const last = root.annotations[root.annotations.length - 1]
        return { kind: last.kind, x1: last.x1, y1: last.y1, x2: last.x2, y2: last.y2,
                 stroke: last.stroke, color: last.color,
                 width: canvas.strokeOf(last), amp: canvas.ampOf(last) }
    }

    function testState() {
        const last = root.annotations.length > 0
                     ? root.annotations[root.annotations.length - 1] : null
        return { count: root.annotations.length, tool: root.tool, zoom: root.zoom,
                 selected: root.selected, kind: last ? last.kind : "",
                 text0: last && last.text !== undefined ? last.text : "",
                 x0: last ? last.x : -1, selection: root.selectionText,
                 ocrBusy: root.ocrBusy,
                 /* 提示条现在写的字（只有认字那条路的话；那句用法说明撤了，见 hint） */
                 hintText: hintText.text, hintShown: hint.visible,
                 /* 图上选字：认出来几行、现在选中的那段是什么（picked = 「翻这段」
                    和 Ctrl+C 真正用的那个口子，见 pickedText） */
                 ocrLines: root.ocrLines.length, ocrPick: root.ocrPick,
                 picked: root.pickedText,
                 /* 现在用的是哪个引擎 / 认字这条路有什么话说（菜单和提示条看它） */
                 ocrEngine: Llm.pinOcrEngine, ocrStatus: root.ocrStatus,
                 ocrRunning: root.ocrRunning, ocrMenuOpen: root.ocrMenuOpen,
                 /* 工具条的几何（调排版时量它：宽高不对就说明里面折行 / 环了） */
                 barX: bar.x, barY: bar.y, barW: bar.width, barH: bar.height }
    }

    /*
     * 自检：识别卡片现在什么状态。
     *
     * 用户报的"点「翻译」什么都没发生"量的就是这个 —— 光看 root.ocrBusy 不够
     * （那是"在等模型"），得看卡片**到底摆没摆出来**。
     */
    function testCardState() {
        return { cardOk: recognitionCard !== null,
                 visible: recognitionCard.visible, hasResult: recognitionCard.hasResult,
                 busy: recognitionCard.busy, status: recognitionCard.status,
                 original: recognitionCard.originalText,
                 translated: recognitionCard.translatedText,
                 ocrBusy: root.ocrBusy }
    }

    /* 自检：替用户按一下卡片右上角那个 ✕（走的就是那个键的同一条路） */
    function testCardClose() { recognitionCard.closeRequested() }

    /*
     * 模型的回复按 token 认领（和截图那边同一套）：用户换语言连点两下时，
     * 先回来的那一口不能把后一口的结果盖掉。
     */
    Connections {
        target: Llm

        function onFinished(token, text) {
            if (token !== root.ocrToken)
                return
            root.applyRecognition(text, recognitionCard.targetLang !== "")
        }

        function onFailed(token, error) {
            if (token !== root.ocrToken)
                return
            root.ocrBusy = false
            root.ocrToken = ""
            recognitionCard.showError(error)
        }
    }

    /*
     * 缩放：**窗口尺寸跟着图走**，两边必须同一份值。
     *
     * 缩放按钮调的是 C++（PinWindow::zoomBy），C++ 那边同时改自己的 zoom 和窗口
     * 大小，再把 zoom 推回这里；这里一收到就更新画面比例和底图的显示尺寸。
     * 反过来只在这边改 zoom 的话，窗口大小不变、图会溢出窗口。
     */
    Connections {
        target: root.pinWin

        function onZoomChanged() {
            if (!root.pinWin)
                return
            root.zoom = root.pinWin.zoom
            root.viewWidth = root.pinWin.viewWidth
            root.viewHeight = root.pinWin.viewHeight
            canvas.repaint()
        }
    }

    /*
     * 标注一改就告诉 C++ 一声（PinWindow::annotationsChanged）。
     *
     * 这一步**不能省**：复制 / 保存 / 交给模型识别用的那张成品图是 C++ 用
     * QPainter 重画的（PinWindow::composedImage），它不知道 QML 这边数组里
     * 现在是什么。少了这一条，屏幕上看得见划的重点，存出来却没有。
     */
    onAnnotationsChanged: if (root.pinWin) root.pinWin.annotationsChanged(root.annotations)

    /* ================= 底图 / 标注层 / 工具条 ================= */

    /*
     * 外发光：把浮在桌面上的贴图"抬起来"，好和桌面区分开。
     *
     * 只画在屏幕上 —— 复制 / 保存走的是 C++ composedImage()（只含底图 + 标注），
     * 这圈光根本不在那条路里，所以成品图干净、不带发光（用户要求）。
     *
     * 用一张和图片同尺寸、同位置的实心矩形当"投影源"，MultiEffect 只在它四周
     * 晕出一圈**四边等距**的淡蓝白辉光（offset 都取 0）；实心那块正好被压在底图
     * 下面看不见，于是屏幕上只剩这圈光。
     */
    Rectangle {
        id: shadowCaster

        x: root.padSide
        y: root.padSide
        width: root.viewWidth
        height: root.viewHeight
        radius: 2
        color: "#ff000000"
        visible: root.viewWidth > 0 && root.viewHeight > 0

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: "#cfe6ff"          /* 白 + 蓝：偏白的淡蓝 */
            shadowOpacity: 0.9              /* 加深 */
            shadowBlur: 0.45
            shadowScale: 1.0
            shadowVerticalOffset: 0         /* 四边等距：不做下偏 */
            shadowHorizontalOffset: 0
            /* 开自动补边：不然光被裁在图片矩形里，散不到四周那条留白上 */
            autoPaddingEnabled: true
        }
    }

    Image {
        id: shotImage

        x: root.padSide
        y: root.padSide
        width: root.viewWidth
        height: root.viewHeight
        source: root.imageId !== "" ? "image://pin/" + root.imageId : ""
        fillMode: Image.Stretch
        /* 不进 QQuickPixmapCache（id 里那个序号已经绕开了，这条是双保险） */
        cache: false
    }

    /*
     * 图上选字的高亮层：画在底图上、标注层**下面**（选中的底色不该盖住后来划的重点）。
     *
     * 一行一个矩形（Repeater），**不用 Canvas**：Canvas 那边要 requestPaint() + 等
     * 下一帧才画，在这套 QQuickWidget 上实测"拖选完立刻抓图"抓不到高亮（自检和
     * demo 都踩到了）。矩形是"数组一变就跟着重画"，没有这一帧的时差。
     *
     * 这一层不吃鼠标事件（普通 Item 不接事件）：按下 / 拖动还是走 hand 那只手，
     * 见 pointerDown 里那个 ocrSelectStart。
     */
    Item {
        id: ocrHighlight

        x: root.padSide
        y: root.padSide
        width: root.viewWidth
        height: root.viewHeight
        visible: root.hlRanges.length > 0

        Repeater {
            model: root.hlRanges

            delegate: Rectangle {
                required property var modelData

                x: modelData.x0
                y: modelData.y0
                width: Math.max(1, modelData.x1 - modelData.x0)
                height: Math.max(1, modelData.y1 - modelData.y0)
                /* 0.40：看着像"选中的字"了，又不至于把底下的字盖住（0.30 实测太淡） */
                color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.40)
            }
        }
    }

    /* 标注层：形状画在 Canvas 上，文字是可编辑的 TextEdit（见下面 textLayer） */
    Item {
        id: pinLayer

        x: root.padSide
        y: root.padSide
        width: root.viewWidth
        height: root.viewHeight

        Canvas {
            id: canvas

            anchors.fill: parent
            antialiasing: true
            renderTarget: Canvas.Image

            function repaint() { requestPaint() }

            /* ---- 这几个数必须和 C++ 那边一字不差（见文件头） ---- */
            /*
             * 直线 / 波浪线 / 删除线的粗细：**细一点**（0.6 倍，最细 1.2）。
             * 用户嫌原来的 3px 太粗 —— 下划线、删除线那个位置本来就是一根细线，
             * 3px 加上圆头，看着像拿马克笔划的。
             */
            function lineWidthOf(stroke) { return Math.max(1.2, stroke * 0.6) }
            /*
             * 实际用来画的"线宽"。荧光笔不算线（它是"涂一层"），那条走 stroke×2.4
             * 当带子的高矮，所以不乘上面那个系数 —— 不然"贴到行上"那招按行高算出来
             * 的带子就白算了（见 applyShapeToSelection）。
             */
            function strokeOf(entry) {
                return entry.kind === "highlight" ? Math.max(3, entry.stroke)
                                                  : canvas.lineWidthOf(entry.stroke)
            }
            function ampOf(entry) { return Math.min(4, 1.2 * canvas.strokeOf(entry)) }

            /* 波浪线：从 a 到 b，沿法线方向一来一回地摆 */
            function pathWavy(ctx2d, a, b, amp) {
                const dx = b.x - a.x
                const dy = b.y - a.y
                const length = Math.hypot(dx, dy)
                if (length < 1)
                    return
                const steps = Math.max(2, Math.round(length / 8))
                const ux = dx / length
                const uy = dy / length
                const nx = -uy
                const ny = ux
                ctx2d.beginPath()
                ctx2d.moveTo(a.x, a.y)
                for (let i = 1; i <= steps; ++i) {
                    const t = i / steps
                    const side = (i % 2 === 0) ? 1 : -1
                    const cxp = a.x + dx * (t - 0.5 / steps) + nx * amp * side
                    const cyp = a.y + dy * (t - 0.5 / steps) + ny * amp * side
                    const exp = a.x + dx * t + nx * amp * side
                    const eyp = a.y + dy * t + ny * amp * side
                    ctx2d.quadraticCurveTo(cxp, cyp, exp, eyp)
                }
                ctx2d.stroke()
            }

            /* 一个形状（pin 坐标进、pin 坐标画；缩放由调用方统一 scale） */
            function paintShape(ctx2d, entry) {
                if (!entry || entry.kind === "text")
                    return
                const a = { x: entry.x1, y: entry.y1 }
                const b = { x: entry.x2, y: entry.y2 }
                const stroke = canvas.strokeOf(entry)
                ctx2d.lineCap = "round"
                ctx2d.lineJoin = "round"
                ctx2d.globalAlpha = 1.0

                if (entry.kind === "highlight") {
                    /* 荧光笔：沿这条线涂一道粗的、半透明的带子 */
                    const band = stroke * 2.4
                    const dx = b.x - a.x
                    const dy = b.y - a.y
                    const length = Math.hypot(dx, dy)
                    if (length < 1)
                        return
                    ctx2d.save()
                    ctx2d.translate(a.x, a.y)
                    ctx2d.rotate(Math.atan2(dy, dx))
                    ctx2d.fillStyle = entry.color
                    ctx2d.globalAlpha = 0.35
                    ctx2d.fillRect(0, -band / 2, length, band)
                    ctx2d.restore()
                    return
                }

                ctx2d.strokeStyle = entry.color
                ctx2d.lineWidth = stroke

                if (entry.kind === "wavy") {
                    canvas.pathWavy(ctx2d, a, b, canvas.ampOf(entry))
                    return
                }
                /* 直线 / 删除线：都是按下点到松开点的一条直线，只是用法不同 */
                ctx2d.beginPath()
                ctx2d.moveTo(a.x, a.y)
                ctx2d.lineTo(b.x, b.y)
                ctx2d.stroke()
            }

            onPaint: {
                const ctx2d = getContext("2d")
                ctx2d.setTransform(1, 0, 0, 1, 0, 0)
                ctx2d.clearRect(0, 0, width, height)
                const s = Math.max(0.05, root.zoom)
                ctx2d.scale(s, s)
                for (let i = 0; i < root.annotations.length; ++i)
                    canvas.paintShape(ctx2d, root.annotations[i])
                /* 正在拖的那一笔（还没进数组） */
                if (root.drawKind !== "") {
                    canvas.paintShape(ctx2d, { kind: root.drawKind, x1: root.drawX1,
                                               y1: root.drawY1, x2: root.drawX2,
                                               y2: root.drawY2, stroke: root.strokeWidth,
                                               color: root.annotColor.toString() })
                }
            }

            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            Component.onCompleted: requestPaint()
        }

        /*
         * 划线 / 拖窗口的那只手。
         *
         * **必须声明在文字层之前**（同一个 Item 里后声明的在上）：压在文字层上面
         * 的话，它会把所有点击都吃掉 —— 文字框拿不到焦点，光标出不来、字选不中、
         * 也改不了（用户报的"不能选中"就是这么来的）。现在它在文字层下面，
         * 点在文字框上由那一层先接，点在空白处才轮到它。
         */
        MouseArea {
            id: hand

            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            hoverEnabled: true
            /* 压在图上认出来的字上时给 I 形光标（"这儿能选字"的唯一提示） */
            cursorShape: root.shapeTool ? Qt.CrossCursor
                                        : (root.textHover ? Qt.IBeamCursor
                                                          : Qt.SizeAllCursor)

            onPressed: (mouse) => root.pointerDown(mouse.x, mouse.y,
                                                   mouse.button === Qt.RightButton,
                                                   mouse.modifiers)
            /*
             * 只有**左键按着**才把移动喂进去。
             *
             * 这一层挡的是真事件：hoverEnabled 开着，鼠标"没按任何键、只是从图上
             * 划过"也会发 positionChanged，那串事件不该被当成拖动。
             * pointerMove 里那个 leftDown 是第二层 —— 管的是"按下之后窗口丢了鼠标
             * 抓取、松手那一下没送到"这种残留状态（那时真事件也被这一层挡掉了）。
             *
             * 没按键时只做一件事：看压没压在字上，把光标换成 I 形。
             */
            onPositionChanged: (mouse) => {
                if (mouse.buttons & Qt.LeftButton)
                    root.pointerMove(mouse.x, mouse.y)
                else
                    root.updateTextHover(mouse.x, mouse.y)
            }
            onReleased: (mouse) => root.pointerUp()
        }

        /*
         * 文字层：一条文字标注一个**可编辑**的 TextEdit（用户要的"内容可以选择、
         * 也可以编辑"）。挪动 / 删掉 / 旋转都在这一层。
         */
        Repeater {
            id: textRepeater

            model: root.annotations

            delegate: Item {
                id: entry

                required property int index
                required property var modelData

                readonly property bool isText: entry.modelData.kind === "text"
                readonly property real fsize: entry.modelData.size !== undefined
                                              ? entry.modelData.size : 16
                readonly property real boxW: entry.modelData.w !== undefined
                                             ? entry.modelData.w : 0
                readonly property real boxH: entry.modelData.h !== undefined
                                             ? entry.modelData.h : 0
                readonly property real rot: entry.modelData.rot !== undefined
                                            ? entry.modelData.rot : 0
                /* 定过宽就按那个宽折行；没定过就跟着文字长 */
                readonly property bool fixedWidth: entry.boxW > 0

                visible: entry.isText
                x: root.px(entry.modelData.x)
                y: root.py(entry.modelData.y)
                width: (entry.fixedWidth ? entry.boxW : Math.max(40, editor.implicitWidth))
                       * root.zoom
                height: Math.max(20, Math.max(entry.boxH, editor.implicitHeight)) * root.zoom

                transform: Rotation {
                    origin.x: entry.width / 2
                    origin.y: entry.height / 2
                    angle: entry.rot
                }

                /* 选中 / 正在编辑的那条：一圈蓝框 */
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -2
                    color: "transparent"
                    border.color: root.accent
                    border.width: 1
                    visible: entry.index === root.selected
                }

                /*
                 * 左上角那个小圆点：按住它拖整条文字框。
                 *
                 * 为什么单给一个手柄、不让框身自己拖：框身留给正文了 —— "点一下出
                 * 光标、拖着选字"是用户更要的东西（见 editor 的 activeFocusOnPress）。
                 * 只在"选中"工具下露出来（别的工具下鼠标是划线用的，不该碰到它）。
                 */
                Rectangle {
                    id: moveHandle

                    x: -8 - entry.x
                    y: -8 - entry.y
                    width: 16
                    height: 16
                    radius: 8
                    color: moveHit.containsMouse ? root.accent : "#26282b"
                    border.color: root.accent
                    border.width: 1
                    visible: entry.index === root.selected
                             && (root.tool === "select" || root.tool === "")

                    AppIcon {
                        anchors.centerIn: parent
                        provider: icons
                        kind: "move"
                        size: 10
                        tint: moveHit.containsMouse ? "#ffffff" : root.accent
                    }

                    MouseArea {
                        id: moveHit
                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        cursorShape: Qt.SizeAllCursor
                        onPressed: (mouse) => {
                            const at = mapToItem(pinLayer, mouse.x, mouse.y)
                            root.beginMove(entry.index, at.x, at.y)
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed)
                                return
                            const at = mapToItem(pinLayer, mouse.x, mouse.y)
                            root.pointerMove(at.x, at.y)
                        }
                        onReleased: root.pointerUp()
                    }
                }

                TextEdit {
                    id: editor

                    anchors.fill: parent
                    text: entry.modelData.text !== undefined ? entry.modelData.text : ""
                    color: entry.modelData.color
                    font.pixelSize: Math.max(6, entry.fsize * root.zoom)
                    wrapMode: entry.fixedWidth ? TextEdit.Wrap : TextEdit.NoWrap
                    selectByMouse: true
                    persistentSelection: true
                    /*
                     * 只有"选中"工具下编辑框才吃鼠标：别的工具在图上划重点时，
                     * 鼠标划过文字不该变成选字（选字和划线是两种意图）。
                     */
                    readonly property bool editing: root.tool === "select" || root.tool === ""
                    activeFocusOnPress: editor.editing
                    readOnly: !editor.editing
                    focus: entry.index === root.editing
                    cursorVisible: activeFocus && !readOnly
                    selectionColor: root.accent
                    selectedTextColor: "#ffffff"

                    onTextChanged: {
                        if (editor.text !== entry.modelData.text)
                            root.setTextAt(entry.index, editor.text)
                    }

                    onActiveFocusChanged: {
                        if (activeFocus) {
                            root.selected = entry.index
                            root.editing = entry.index
                        } else if (root.editing === entry.index) {
                            root.editing = -1
                        }
                        /* 快捷键要不要让开（见 root.editorFocused 的说明） */
                        root.editorFocused = activeFocus
                    }

                    /* 有选中的字 -> 工具条上「翻这段」才亮 */
                    onSelectedTextChanged: root.reportSelection(selectedText)
                }
            }
        }
    }

    /*
     * 工具条。**常驻**摆在图片下沿**外面**那条边距里（不叠在图上），水平居中于图片。
     *
     * 单行、不换行（用户要求）：宽度 = 所有键摆成一排的宽度（barFlow.singleRowWidth），
     * Flow 给的宽度比它略大所以永远排成一列不折；条高固定 barH。
     *
     * "不随截图缩小放大"：条的宽高只由**内容**决定，跟 zoom / viewWidth 无关 ——
     * 图放大缩小，条一直是那么大。图片缩到最小的时候宽度就等于这条的宽
     * （量好后回报给 C++：见下面 onWidthChanged → setMinBarWidth）。
     */
    Rectangle {
        id: bar

        width: barFlow.singleRowWidth + 18
        height: root.barH
        radius: 8
        color: "#26282b"
        border.color: root.borderColor
        border.width: 1

        x: Math.max(0, Math.min(root.width - width,
                                root.padSide + (root.viewWidth - width) / 2))
        y: root.padSide + root.viewHeight + root.barGap

        /* 条宽一定下来就回报给 C++ 当缩放下限（图片最小宽 = 这条宽） */
        onWidthChanged: if (root.pinWin && width > 0) root.pinWin.setMinBarWidth(width)
        Component.onCompleted: if (root.pinWin && width > 0) root.pinWin.setMinBarWidth(width)

        /*
         * Flow 的宽度只跟 bar.width 走，且**恒大于** singleRowWidth（+2）——
         * 所以永远排成一行、绝不折行（用户要求）。singleRowWidth 只读每个键自己的
         * 宽度、不回读 Flow 的 width，依赖是单向的（那个环踩过的，见文件别处说明）。
         */

        component BarButton: Rectangle {
            id: btn

            property string label: ""
            property string iconKind: ""
            property bool active: false
            property bool danger: false

            signal clicked()

            implicitWidth: Math.max(30, btnLabel.implicitWidth + 16)
            implicitHeight: 30
            radius: 5
            color: !btn.enabled ? "transparent"
                                : (btn.active ? root.accent
                                              : (btnHit.containsMouse ? "#3a3d41" : "transparent"))

            AppIcon {
                id: btnIcon
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 3
                provider: icons
                kind: btn.iconKind
                size: 13
                tint: !btn.enabled ? "#5c6066"
                                   : (btn.active ? "#ffffff"
                                                 : (btnHit.containsMouse ? "#e8e8e8" : "#b4b8bf"))
            }

            Text {
                id: btnLabel
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 2
                text: btn.label
                font.pixelSize: 9
                color: !btn.enabled ? "#5c6066"
                                    : (btn.active ? "#ffffff"
                                                  : (btn.danger && btnHit.containsMouse ? "#e08a7a"
                                                                                        : "#b4b8bf"))
            }

            MouseArea {
                id: btnHit
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: btn.clicked()
            }
            AppToolTip { hovered: btnHit.containsMouse; text: btn.label }
        }

        component BarGap: Rectangle {
            width: 1
            height: 18
            color: "#4b4d4f"
        }

        /*
         * Flow 而不是 RowLayout：窗口窄的时候（贴图可以很小）自动折行，
         * 不然十几个键摆出来的四百多像素会把"关闭"挤到窗口外面去。
         */
        Flow {
            id: barFlow

            /*
             * **不折行时这一串要占多宽**：把每个键自己的宽度加一遍。
             *
             * 关键点：**不读 Flow 自己的 width / implicitWidth** —— 那样就又和
             * bar.width 成环了（见 bar.width 那段说明）。键的 width 是它们自己的
             * 尺寸（BarButton 由 implicitWidth 定），跟摆在哪、折不折行无关，
             * 所以这个值稳定、两边的依赖是单向的。
             */
            readonly property real singleRowWidth: {
                let total = 0
                const kids = children
                for (let i = 0; i < kids.length; ++i) {
                    if (kids[i].visible === false)
                        continue
                    total += kids[i].width + (total > 0 ? spacing : 0)
                }
                return total
            }

            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: bar.width - 16
            spacing: 2

            /* ---- 用户点名的那五个 ---- */
            BarButton {
                objectName: "pinCopy"
                label: "复制"
                iconKind: "copy"
                onClicked: if (root.pinWin) root.pinWin.copyResult()
            }

            BarGap {}

            BarButton {
                objectName: "pinToolHighlight"
                label: "荧光笔"
                iconKind: "highlight"
                active: root.tool === "highlight"
                onClicked: root.pickTool("highlight")
            }
            BarButton {
                objectName: "pinToolWavy"
                label: "波浪线"
                iconKind: "wavy"
                active: root.tool === "wavy"
                onClicked: root.pickTool("wavy")
            }
            BarButton {
                objectName: "pinToolLine"
                label: "直线"
                iconKind: "line"
                active: root.tool === "line"
                onClicked: root.pickTool("line")
            }
            BarButton {
                objectName: "pinToolStrike"
                label: "删除线"
                iconKind: "strike"
                active: root.tool === "strike"
                onClicked: root.pickTool("strike")
            }

            BarGap {}

            /*
             * ---- 其余几样：选中（可编辑）/ 翻选中的那段 ----
             *
             * 原来这排第一个是「文字」（在图上落一条文字标注），**撤了**（用户
             * 说不要这个功能）。文字标注本身还认：识别卡片上的「加到图上」落下来
             * 的就是一条，接着照样能改字号 / 拖动 / 编辑。
             */
            BarButton {
                objectName: "pinToolSelect"
                label: "选中"
                iconKind: "select-all"
                active: root.tool === "select" || root.tool === ""
                onClicked: root.pickTool("select")
            }
            /*
             * 认字：选"图上选字"用哪个引擎（Windows 自带 / PP-OCRv6），点了就立刻
             * 用那个引擎认一遍。贴图一贴出来已经自动认过一次（见 Component.onCompleted），
             * 这个键是"换引擎 / 再认一遍"。
             */
            BarButton {
                objectName: "pinOcr"
                label: "认字"
                iconKind: "ocr"
                active: root.ocrRunning
                onClicked: root.ocrMenuOpen = !root.ocrMenuOpen
            }
            BarButton {
                objectName: "pinTranslateSelection"
                label: "翻这段"
                iconKind: "translate"
                enabled: root.hasSelection && !root.ocrBusy
                onClicked: {
                    root.lastWasSelection = true
                    root.translateSelection()
                }
            }

            BarGap {}

            BarButton {
                objectName: "pinUndo"
                label: "撤销"
                iconKind: "undo"
                enabled: root.annotations.length > 0
                onClicked: root.undo()
            }
            BarButton {
                objectName: "pinTranslate"
                label: "翻译"
                iconKind: "scan"
                active: root.ocrBusy
                enabled: !root.ocrBusy
                onClicked: {
                    root.lastWasSelection = false
                    root.translateAll()
                }
            }

            BarGap {}

            BarButton {
                objectName: "pinSave"
                label: "保存"
                iconKind: "save-as"
                onClicked: if (root.pinWin) root.pinWin.saveImageAs()
            }
            BarButton {
                objectName: "pinZoomOut"
                label: "缩小"
                iconKind: "zoom-out"
                onClicked: if (root.pinWin) root.pinWin.zoomBy(1 / 1.1)
            }
            BarButton {
                objectName: "pinZoomIn"
                label: "放大"
                iconKind: "zoom-in"
                onClicked: if (root.pinWin) root.pinWin.zoomBy(1.1)
            }
            BarButton {
                objectName: "pinClose"
                label: "关闭"
                iconKind: "close"
                danger: true
                onClicked: if (root.pinWin) root.pinWin.closePin()
            }
        }
    }

    /*
     * 「认字」菜单：两个引擎挑一个（Windows 自带 / PP-OCRv6）。
     *
     * 点一行 = **换成它**（记进 Llm.pinOcrEngine，勾跟着走）再拿它认一遍；
     * 命令跑不通的那一行是灰的，点了不动，底下用红字写着为什么。
     *
     * 不用 DropdownMenu（它是独立原生弹窗，在置顶无边框窗口里点几次就弹不出来，
     * 见 RecognitionCard.qml 文件头那段），就地画一块 —— 位置和提示条一样，
     * 摆在工具条上面。点别处 / Esc 收起（见 pointerDown 和 Esc 快捷键）。
     */
    Rectangle {
        id: ocrMenu

        width: Math.min(320, Math.max(200, root.width - 16))
        height: ocrMenuColumn.implicitHeight + 12
        radius: 8
        color: "#26282b"
        border.color: root.borderColor
        border.width: 1
        x: Math.max(8, (root.width - width) / 2)
        y: Math.max(8, bar.y - height - 8)
        visible: root.ocrMenuOpen
        z: 6

        Column {
            id: ocrMenuColumn

            anchors.fill: parent
            anchors.margins: 6
            spacing: 2

            Text {
                width: parent.width
                text: "图上选字用哪个引擎"
                color: "#8a9098"
                font.pixelSize: 10
            }

            Repeater {
                id: ocrEngineRepeater

                model: root.ocrEngineList

                delegate: Rectangle {
                    id: engineRow

                    required property var modelData

                    readonly property string problem: root.ocrEngineProblem(engineRow.modelData.key)
                    readonly property bool usable: engineRow.problem === ""
                    readonly property bool current: Llm.pinOcrEngine === engineRow.modelData.key

                    objectName: "pinOcrEngine_" + engineRow.modelData.key
                    width: parent.width
                    height: 38
                    radius: 5
                    color: (rowHit.containsMouse && engineRow.usable) ? "#3a3d41" : "transparent"

                    Text {
                        id: engineMark

                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        anchors.top: parent.top
                        anchors.topMargin: 3
                        width: 12
                        text: engineRow.current ? "✓" : ""
                        color: root.accent
                        font.pixelSize: 12
                    }
                    Text {
                        anchors.left: engineMark.right
                        anchors.leftMargin: 6
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.top: parent.top
                        anchors.topMargin: 3
                        text: engineRow.modelData.label
                        color: engineRow.usable ? "#e8e8e8" : "#8a9098"
                        font.pixelSize: 12
                    }
                    Text {
                        anchors.left: engineMark.right
                        anchors.leftMargin: 6
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.top: parent.top
                        anchors.topMargin: 20
                        elide: Text.ElideRight
                        /* 不能用就把"为什么"顶上来（"PATH 里找不到 python" 这类） */
                        text: engineRow.usable ? engineRow.modelData.tip : engineRow.problem
                        color: engineRow.usable ? "#8a9098" : "#c8503c"
                        font.pixelSize: 10
                    }
                    MouseArea {
                        id: rowHit

                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: engineRow.usable ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: if (engineRow.usable) root.runOcr(engineRow.modelData.key)
                    }
                }
            }
        }
    }

    /*
     * 认字那条路有话要说时的提示条（"图上没认出字" / "PATH 里找不到 python" 这类）。
     *
     * 原来这块还兼着"一进来那句用法说明"（图上拖 = 选字 / Ctrl+拖 = 挪贴图 …），
     * **那句撤了**（用户要求）—— 现在只在认字那条路有话时露一下，平时一直是空的。
     */
    Rectangle {
        id: hint

        width: hintText.implicitWidth + 20
        height: 24
        radius: 6
        color: "#26282b"
        border.color: root.borderColor
        border.width: 1
        opacity: root.ocrStatus !== "" ? 0.95 : 0
        visible: opacity > 0
        x: Math.max(8, (root.width - width) / 2)
        y: Math.max(8, bar.y - height - 8)

        Behavior on opacity { NumberAnimation { duration: 220 } }

        Text {
            id: hintText
            anchors.centerIn: parent
            text: root.ocrStatus
            font.pixelSize: 11
            color: "#e08a7a"
        }
    }

    /* 识别 / 翻译的结果卡片（和截图那边共用同一个组件） */
    RecognitionCard {
        id: recognitionCard

        targetLanguages: root.ocrTargets
        sourceLang: "自动检测"
        targetLang: root.ocrTarget

        /*
         * 贴图窗口可能比卡片窄（卡片自己写死 460）：宽了就被窗口边界切掉，
         * 底下那排「复制 / 翻译」按钮正好在被切的那头。所以宽度跟着窗口收。
         */
        width: Math.min(460, Math.max(260, root.width - 16))
        x: Math.max(8, Math.min(root.width - width - 8, (root.width - width) / 2))
        y: Math.max(8, bar.y - height - 10)
        visible: hasResult

        onCopyRequested: root.copyRecognition()
        onAnnotateRequested: root.annotateRecognition()
        onTargetPicked: (lang) => root.retranslate(lang)
        /*
         * ✕：清 hasResult（**不要**直接写 visible）—— visible 是绑在 hasResult 上的，
         * 直接写 visible 会把那条绑定打断，之后「翻译」再点也摆不出来了。
         * 见 RecognitionCard 里 closeRequested 的说明。
         */
        onCloseRequested: recognitionCard.hasResult = false
    }

    IconProvider { id: icons }

    /* ================= 快捷键 ================= */
    /*
     * Esc 关掉这张贴图（原来那个静态贴图窗口就是这个手感）。
     *
     * 打字的时候让开：文字框里 Esc 归编辑自己（TextEdit 收键盘）。
     * 通道走 pinWin.closePin()（和工具条上那个"关闭"是同一个），不是 QML 自己
     * 把窗口藏了 —— 窗口的生死归 C++ 管（关掉就删，见 PinWindow::closePin）。
     */
    Shortcut {
        sequence: "Escape"
        enabled: !editorFocused
        onActivated: {
            /* 认字菜单开着的话，Esc 先收菜单（别一下把贴图关了） */
            if (root.ocrMenuOpen) {
                root.ocrMenuOpen = false
                return
            }
            if (root.pinWin)
                root.pinWin.closePin()
        }
    }

    /* Ctrl+Z 撤销（和工具条上那个"撤销"同一个函数）；打字时让给编辑框自己的撤销 */
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: !editorFocused
        onActivated: root.undo()
    }

    /*
     * Ctrl+C：**选了字就复制那段字**，没选就复制整张贴图（成品图）。
     * 文字框里选中字时让开，那是编辑框自己的"复制选中的字"。
     */
    Shortcut {
        sequence: "Ctrl+C"
        enabled: !editorFocused
        onActivated: root.copyPicked()
    }

    /* Ctrl+S：存盘 */
    Shortcut {
        sequence: "Ctrl+S"
        onActivated: if (root.pinWin) root.pinWin.saveImageAs()
    }

    /*
     * 有文字框正在打字吗（快捷键要让开）。
     * 由文字层里的 TextEdit 上报（见下面 editor 的 onActiveFocusChanged）——
     * 这里不直接读 editing，因为"点一下就选中"和"光标真的在框里"是两回事。
     */
    property bool editorFocused: false

    /* 滚轮缩放：窗口跟着图一起变（见 PinWindow::wheelEvent） */
    WheelHandler {
        onWheel: (event) => {
            if (!root.pinWin)
                return
            root.pinWin.zoomBy(event.angleDelta.y > 0 ? 1.1 : 1 / 1.1)
            event.accepted = true
        }
    }
}
