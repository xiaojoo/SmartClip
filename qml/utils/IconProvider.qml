pragma ComponentBehavior: Bound

import QtQuick

// Central icon generator: returns a tinted inline-SVG data URL for a glyph.
QtObject {
    id: root

    function svg(kind, color) {
        var c = color || "#9aa0a8"
        function p(d) { return '<path d="' + d + '" fill="none" stroke="' + c + '" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>' }
        function f(d) { return '<path d="' + d + '" fill="' + c + '"/>' }
        function ci(x, y, r) { return '<circle cx="' + x + '" cy="' + y + '" r="' + r + '" fill="' + c + '"/>' }
        function co(x, y, r) { return '<circle cx="' + x + '" cy="' + y + '" r="' + r + '" fill="none" stroke="' + c + '" stroke-width="1.7"/>' }
        /*
         * "全部折叠 / 全部展开"共用的四个角（一对向内收的括号）。
         *
         * 形状和 browser-bridge 网页左栏那两个按钮是同一套，
         * 只是把 24 的 viewBox 按 2/3 缩到这里的 16：
         * 四个角 + 中间那条横线（折叠）/ 加号（展开）。
         */
        function treeCorners() {
            return p('M6 2 H3.33 A1.33 1.33 0 0 0 2 3.33 V6')
                 + p('M10 2 H12.67 A1.33 1.33 0 0 1 14 3.33 V6')
                 + p('M6 14 H3.33 A1.33 1.33 0 0 1 2 12.67 V10')
                 + p('M10 14 H12.67 A1.33 1.33 0 0 0 14 12.67 V10')
        }
        /*
         * 喇叭（朗读那几个图标共用）：左边一个小方箱 + 一个朝右张开的号角，
         * 右边按 waves 画几道声波（1 = 一道，2 / 3 = 两道 / 三道）。
         *
         * 和 pin / translate 那些一样是 inline SVG：16 的 viewBox 里画，
         * AppIcon 那个 Image 再按 size 缩放。
         */
        function speakGlyph(waves, color) {
            var g = p('M3.2 6.2 H5.8 L9.6 3.2 V12.8 L5.8 9.8 H3.2 Z')
            if (waves >= 2)
                g += p('M11.4 6 L12.7 8 L11.4 10')
            if (waves >= 3)
                g += p('M13.1 4.4 L14.6 8 L13.1 11.6')
            return g
        }
        var s = ""
        if (kind === "chevron-down")   s = p('M4.4 6.4 L8 10 L11.6 6.4')
        else if (kind === "chevron-right") s = p('M6.4 4.4 L10 8 L6.4 11.6')
        else if (kind === "chevron-left")  s = p('M9.6 4.4 L6 8 L9.6 11.6')
        else if (kind === "chevron-up")    s = p('M4.4 9.6 L8 6 L11.6 9.6')
        else if (kind === "play")          s = f('M5.8 3.4 L13.4 8 L5.8 12.6 Z')
        else if (kind === "close")         s = p('M4.6 4.6 L11.4 11.4 M11.4 4.6 L4.6 11.4')
        // 窗口按钮：缩小 / 放大 / 还原（Fluent 风格的 10px 方框）
        else if (kind === "win-min")       s = p('M3.6 8 L12.4 8')
        else if (kind === "win-max")       s = p('M4.2 4.2 H11.8 V11.8 H4.2 Z')
        else if (kind === "win-restore")   s = p('M6.2 4.2 H11.8 V9.8') + p('M4.2 6.2 H9.8 V11.8 H4.2 Z')
        else if (kind === "plus")          s = p('M8 3.6 L8 12.4 M3.6 8 L12.4 8')
        /*
         * 终端：一个 > 提示符加一条下划线（各家终端都是这个记号）。
         *
         * 两笔都压在 1.7 的线上，16 的格子里 <1.5:1 的细线等于没有，
         * 所以下划线的 y 和 > 的下端点对齐在同一行（10.6），
         * 中间留 1.6 的空 —— 再近就要并成一条了。
         */
        else if (kind === "terminal")      s = p('M3.4 4.6 L6.2 7.6 L3.4 10.6') + p('M7.8 10.6 L12.6 10.6')
        else if (kind === "refresh")       s = p('M14.4 9.4 A6.4 6.4 0 1 1 12.9 4.6') + p('M15.2 3.4 L15.2 6.8 L11.7 6.3')
        else if (kind === "search")        s = co(6.2, 6.2, 4.1) + p('M9.4 9.4 L13.8 13.8')
        else if (kind === "gear")          s = p('M8 4.9 A3.1 3.1 0 1 0 8 11.1 A3.1 3.1 0 1 0 8 4.9') + (function(){ var t = ""; for (var a = 0; a < 360; a += 45) t += '<line x1="13.2" y1="8" x2="15.4" y2="8" stroke="' + c + '" stroke-width="1.6" stroke-linecap="round" transform="rotate(' + a + ' 8 8)"/>'; return t })()
        else if (kind === "more")          s = ci(8, 3.2, 1.5) + ci(8, 8, 1.5) + ci(8, 12.8, 1.5)
        /*
         * 主题那一格用的两枚：
         *   "gear" 那枚画出来本来就是"圆心 + 八道射线"，深色档下它就是"太阳"
         *          （点一下 → 换白色），所以不再另画一枚 sun；
         *   "moon" 两段圆弧拼的月牙，浅色档下表示"点一下 → 换回深色"。
         * 射线那圈半径 13.2~15.4 已经顶到 16 的边上，月牙的外弧取 6.2 半径
         * 是同一套占格 —— 两枚摆在一起大小看着是一对，不会一个胖一个瘦。
         */
        else if (kind === "moon")          s = p('M9.8 2.4 A6.2 6.2 0 1 0 13.2 12.6 A4.9 4.9 0 1 1 9.8 2.4')
        /*
         * "palette" = 设置里「配色方案」那一栏的图标：一个圆圈 + 四颗色点。
         * 点用实心、半径 1.1，四颗按 2x2 摆在圆心四周（不摆正中间，中间要留给
         * 圆心的空白，不然远看就是一坨）。
         */
        else if (kind === "palette")       s = co(8, 8, 5.6) + ci(6.1, 5.6, 1.1) + ci(9.9, 5.6, 1.1)
                                             + ci(5.6, 9.4, 1.1) + ci(10.4, 9.4, 1.1)
        /*
         * 左侧项目树标题栏那排按钮（和 PyCharm 项目面板同款）：
         *   全部折叠 = 四个角的括号 + 中间一条横线
         *   全部展开 = 同一对括号 + 中间一个加号
         *   定位     = 准星（Select Opened File）
         *   minus    = 收起面板那条横线（window 的 win-min 是同一个形状，
         *              这里单独给一个名字，免得看着像窗口按钮）
         */
        else if (kind === "collapse-all")  s = treeCorners() + p('M2.67 8 H13.33')
        else if (kind === "expand-all")    s = treeCorners() + p('M8 5.33 V10.67 M5.33 8 H10.67')
        else if (kind === "minus")         s = p('M3.6 8 L12.4 8')
        /* 定位当前标签：准星（圆圈 + 四条短线），和 PyCharm 的 Select Opened File 一个意思 */
        else if (kind === "locate")        s = co(8, 8, 4.5) + p('M8 1.6 V3.6 M8 12.4 V14.4 M1.6 8 H3.6 M12.4 8 H14.4') + ci(8, 8, 1.2)
        /* 截图：四个角括号 = 框选（左侧图标条上那一格用它） */
        else if (kind === "screenshot")    s = p('M2.4 5.6 V4 A1.6 1.6 0 0 1 4 2.4 H5.6') + p('M10.4 2.4 H12 A1.6 1.6 0 0 1 13.6 4 V5.6') + p('M13.6 10.4 V12 A1.6 1.6 0 0 1 12 13.6 H10.4') + p('M5.6 13.6 H4 A1.6 1.6 0 0 1 2.4 12 V10.4')
        /*
         * 便签：一张卷角的小纸（左侧图标条那一格、菜单里"新建便签"）。
         *
         * 和随包的 resources/icons/note.svg 同一个形状，只是按 16 的 viewBox
         * 缩了一版 —— 图标条那一格用的是这里的 inline SVG（见 AppIcon），
         * 托盘 / 菜单那种地方才走资源文件。
         */
        else if (kind === "note")          s = p('M3.4 2.8 H12.6 A1.1 1.1 0 0 1 13.7 3.9 V9.6 L10 13.2 H3.4 A1.1 1.1 0 0 1 2.3 12.1 V3.9 A1.1 1.1 0 0 1 3.4 2.8 Z') + p('M13.7 9.6 H10.8 A1.1 1.1 0 0 0 9.7 10.7 V13.2') + p('M4.6 6 H11.4 M4.6 8.4 H8.6')
        /*
         * 文本框四个角的手柄图标（见 qml/screenshot/CaptureOverlay.qml）：
         *   rotate  旋转（圆弧 + 箭头）
         *   move    移动整框（四向箭头）
         *   arrow   斜箭头（右下角，自由缩放；按住 Shift 是整体放大）
         */
        else if (kind === "rotate")        s = p('M12.8 9.4 A5.4 5.4 0 1 1 12.2 4.6') + p('M9 1.4 L12.4 4.4 L9.2 7.4')
        else if (kind === "arrow")         s = p('M4.4 4.4 L11.6 11.6') + p('M11.6 7.2 V11.6 H7.2')
        /* move 四向箭头：拖动整个文本框 */
        else if (kind === "move")          s = p('M8 2.2 V13.8 M2.2 8 H13.8') + p('M6.2 4 L8 2.2 L9.8 4') + p('M6.2 12 L8 13.8 L9.8 12') + p('M4 6.2 L2.2 8 L4 9.8') + p('M12 6.2 L13.8 8 L12 9.8')
        else if (kind === "grid")          s = p('M3 3 H7 V7 H3 Z M9 3 H13 V7 H9 Z M3 9 H7 V13 H3 Z M9 9 H13 V13 H9 Z')
        /*
         * 识别：一个"取景框 + 一道扫过去的光"（截图识别 / OCR 那个意思）。
         * 四角各一段角括号（和 screenshot 那个图标同一族），中间一条横线，
         * 底下再垫一条短横线当"读到的那行字"。
         */
        else if (kind === "scan")          s = p('M2.4 5.6 V4 A1.6 1.6 0 0 1 4 2.4 H5.6')
                                                 + p('M10.4 2.4 H12 A1.6 1.6 0 0 1 13.6 4 V5.6')
                                                 + p('M13.6 10.4 V12 A1.6 1.6 0 0 1 12 13.6 H10.4')
                                                 + p('M5.6 13.6 H4 A1.6 1.6 0 0 1 2.4 12 V10.4')
                                                 + p('M3.4 8 H12.6') + p('M5.4 10.6 H10.6')
        /*
         * 贴图窗口那排标注工具（见 qml/screenshot/PinOverlay.qml）。
         *
         * 每个都画成"一条线/一支笔压在字上"的样子，用户瞟一眼就知道是干什么的：
         *   highlight 荧光笔 = 一支斜着的水彩笔 + 底下一条粗线
         *   wavy      波浪线 = 一条起伏的线
         *   line      直线   = 一条斜的直线
         *   strike    删除线 = 一条横线压在字母上
         *   text      文字   = 一个 A 字（压一条底线）
         * undo 用编辑区那把"向左的弧线箭头"（和工具栏里的撤销同一个形状）。
         */
        else if (kind === "highlight")     s = p('M4.6 9.4 L9.6 4.4 A1.3 1.3 0 0 1 11.4 6.2 L6.4 11.2')
                                                 + p('M4.6 9.4 L3.2 12.2 L6.4 11.2')
                                                 + '<path d="M2.6 13.6 H8.4" fill="none" stroke="' + c + '" stroke-width="2.4" stroke-linecap="round"/>'
        else if (kind === "wavy")          s = '<path d="M2.2 9.6 Q4.1 5.6 6 9.6 Q7.9 13.6 9.8 9.6 Q11.7 5.6 13.6 9.6" fill="none" stroke="' + c + '" stroke-width="1.7" stroke-linecap="round"/>'
        else if (kind === "line")          s = p('M3 12.6 L13 3.4')
        else if (kind === "strike")        s = p('M4.8 2.6 V5.2 M4.8 5.2 A2 2 0 0 0 4.8 9.2 A2 2 0 0 1 4.8 13 M4.8 13 V10.6')
                                                 + p('M2.2 8 H13.4')
        else if (kind === "text")          s = p('M4 4.4 L8 12.2 L12 4.4')
                                                 + p('M5.4 10 H10.6') + p('M2.4 14 H13.6')
        else if (kind === "undo")          s = p('M6.2 4.4 L3 7.6 L6.2 10.8') + p('M3 7.6 H9.4 A3.2 3.2 0 0 1 9.4 14')
        else if (kind === "branch")        s = ci(5.2, 4.8, 1.7) + ci(5.2, 11.2, 1.7) + ci(11.6, 8, 1.7) + p('M5.2 6.5 L5.2 9.5') + p('M5.2 9.5 C5.2 12 11.6 10.4 11.6 8.4')
        else if (kind === "folder")        s = f('M2.4 4.4 A1.6 1.6 0 0 1 4 2.8 H6.2 L7.6 4.6 H11.8 A1.6 1.6 0 0 1 13.4 6.2 V11 A1.6 1.6 0 0 1 11.8 12.6 H4 A1.6 1.6 0 0 1 2.4 11 Z')
        else if (kind === "file")          s = p('M4 2.6 H8.9 L13.2 6.9 V12.8 A1.2 1.2 0 0 1 12 14 H4 A1.2 1.2 0 0 1 2.8 12.8 V3.8 A1.2 1.2 0 0 1 4 2.6 Z') + p('M8.9 2.6 V6.9 H13.2') + p('M5.8 10.1 H10.2 M5.8 12.1 H10.2')
        /*
         * Markdown 相关（标签栏最右边那个"预览 / 源码"开关的两态）：
         *   markdown  文件外形 + 中间的 M↓（Markdown 的标志）
         *   preview   文件外形 + 一个朝下的箭头（= 现在看的是**渲染结果**）
         */
        else if (kind === "markdown")      s = p('M4 2.6 H8.9 L13.2 6.9 V12.8 A1.2 1.2 0 0 1 12 14 H4 A1.2 1.2 0 0 1 2.8 12.8 V3.8 A1.2 1.2 0 0 1 4 2.6 Z') + p('M8.9 2.6 V6.9 H13.2') + p('M5.1 11.4 V8.6 L6.7 10.4 L8.3 8.6 V11.4')
        else if (kind === "preview")       s = p('M4 2.6 H8.9 L13.2 6.9 V12.8 A1.2 1.2 0 0 1 12 14 H4 A1.2 1.2 0 0 1 2.8 12.8 V3.8 A1.2 1.2 0 0 1 4 2.6 Z') + p('M8.9 2.6 V6.9 H13.2') + p('M6.4 9.2 V12.2 M4.8 10.7 L6.4 12.3 L8 10.7')
        /*
         * 校验（拼写检查那个意思）：一个对勾 + 旁边一个"对/错"的小叉，
         * 也就是"检查过了"这件事最直白的形状。
         */
        else if (kind === "spellcheck")    s = p('M2.6 8.6 L5.4 11.4 L9.4 5.4') + p('M11 4.4 L14.4 7.8 M14.4 4.4 L11 7.8')
        /* 格式化：三行文字 + 左边一个对齐的竖条（= 重排缩进） */
        else if (kind === "format")        s = p('M6.4 4 H14 M6.4 8 H12.6 M6.4 12 H14') + p('M2.6 2.6 V13.4')
        /* 文件对比：左右两块 + 中间一条分隔线（"两份东西比一比"） */
        else if (kind === "diff")          s = p('M2.4 3.4 H6.8 V12.6 H2.4 Z') + p('M9.2 3.4 H13.6 V12.6 H9.2 Z') + p('M7.4 8 H8.6')
        /*
         * 分栏（tab 右键菜单那两条）：
         *   split-right  左边一块宽、右边一块窄（竖着切一刀）
         *   split-down   上边一块矮、下边一块窄（横着切一刀）
         * 切的那一刀在**右边 / 下边**，和它实际分出来的那一栏位置一致。
         */
        else if (kind === "split-right")   s = p('M2.2 3 H9 V13 H2.2 Z') + p('M11 3 H13.8 V13 H11 Z')
        else if (kind === "split-down")    s = p('M2.2 2.6 H13.8 V7 H2.2 Z') + p('M2.2 9 H13.8 V13.4 H2.2 Z')
        /*
         * Markdown 文档（标签栏最右边那个"预览 / 源码"开关的两态）：
         *   markdown  文件外形 + 中间的 M↓（Markdown 的标志）
         *   preview   文件外形 + 一个朝下的箭头（= 现在看的是**渲染结果**）
         */
        else if (kind === "markdown")      s = p('M4 2.6 H8.9 L13.2 6.9 V12.8 A1.2 1.2 0 0 1 12 14 H4 A1.2 1.2 0 0 1 2.8 12.8 V3.8 A1.2 1.2 0 0 1 4 2.6 Z') + p('M8.9 2.6 V6.9 H13.2') + p('M5.1 11.4 V8.6 L6.7 10.4 L8.3 8.6 V11.4')
        else if (kind === "preview")       s = p('M4 2.6 H8.9 L13.2 6.9 V12.8 A1.2 1.2 0 0 1 12 14 H4 A1.2 1.2 0 0 1 2.8 12.8 V3.8 A1.2 1.2 0 0 1 4 2.6 Z') + p('M8.9 2.6 V6.9 H13.2') + p('M6.4 9.2 V12.2 M4.8 10.7 L6.4 12.3 L8 10.7')
        else if (kind === "image")         s = p('M2.6 3.6 H13.4 A1.2 1.2 0 0 1 14.6 4.8 V11.2 A1.2 1.2 0 0 1 13.4 12.4 H2.6 A1.2 1.2 0 0 1 1.4 11.2 V4.8 A1.2 1.2 0 0 1 2.6 3.6 Z') + ci(6, 7.1, 1.2) + p('M2 12.3 L6.3 8 L8.8 10.2 L11 8.4 L14 11.2')
        else if (kind === "trash")         s = p('M2.8 4.6 H13.2') + p('M5.3 4.6 V3.4 A1.2 1.2 0 0 1 6.5 2.2 H9.5 A1.2 1.2 0 0 1 10.7 3.4 V4.6') + p('M4.2 4.6 L4.9 12.9 A1.2 1.2 0 0 0 6.1 14 H9.9 A1.2 1.2 0 0 0 11.1 12.9 L11.8 4.6') + p('M6.6 7.6 V11.4 M9.4 7.6 V11.4')

        // ---- 编辑器工具栏 ----
        else if (kind === "new")           s = p('M4 2.6 H8.9 L13.2 6.9 V12.8 A1.2 1.2 0 0 1 12 14 H4 A1.2 1.2 0 0 1 2.8 12.8 V3.8 A1.2 1.2 0 0 1 4 2.6 Z') + p('M8.9 2.6 V6.9 H13.2') + p('M4.8 10.4 H8.2 M6.5 8.7 V12.1')
        else if (kind === "open")          s = p('M2 5.4 A1.4 1.4 0 0 1 3.4 4 H6.4 L7.8 5.6 H12.4 A1.4 1.4 0 0 1 13.8 7 V8.2') + p('M2.2 7.8 H14.2 L12.9 12.6 A1.4 1.4 0 0 1 11.5 13.6 H4.3 A1.4 1.4 0 0 1 2.9 12.6 Z')
        else if (kind === "save")          s = p('M3.2 2.6 H10.4 L13.4 5.6 V12.8 A1.2 1.2 0 0 1 12.2 14 H3.2 A1.2 1.2 0 0 1 2 12.8 V3.8 A1.2 1.2 0 0 1 3.2 2.6 Z') + p('M5.2 2.6 V6.2 H10 V2.6') + p('M4.4 9.2 H11 V13.8 H4.4 Z')
        else if (kind === "save-as")       s = p('M3.2 2.6 H10.4 L13.4 5.6 V9.4') + p('M3.2 2.6 A1.2 1.2 0 0 0 2 3.8 V12.8 A1.2 1.2 0 0 0 3.2 14 H8.4') + p('M5.2 2.6 V6.2 H10 V2.6') + p('M10.6 12.4 L14 9 M11.6 8.6 H14.4 V11.4')
        else if (kind === "undo")          s = p('M6.2 4.4 L3 7.6 L6.2 10.8') + p('M3 7.6 H9.4 A3.2 3.2 0 0 1 9.4 14')
        else if (kind === "redo")          s = p('M9.8 4.4 L13 7.6 L9.8 10.8') + p('M13 7.6 H6.6 A3.2 3.2 0 0 0 6.6 14')
        else if (kind === "cut")           s = co(4, 11.6, 2.1) + co(12, 11.6, 2.1) + p('M5.4 10.2 L11.2 2.4 M10.6 10.2 L4.8 2.4')
        else if (kind === "copy")          s = p('M5.6 5.4 H12 A1.2 1.2 0 0 1 13.2 6.6 V12.8 A1.2 1.2 0 0 1 12 14 H5.6 A1.2 1.2 0 0 1 4.4 12.8 V6.6 A1.2 1.2 0 0 1 5.6 5.4 Z') + p('M2.8 10.6 V3.2 A1.2 1.2 0 0 1 4 2 H11.2')
        else if (kind === "paste")         s = p('M5.8 3.6 H4.6 A1.2 1.2 0 0 0 3.4 4.8 V12.8 A1.2 1.2 0 0 0 4.6 14 H11.4 A1.2 1.2 0 0 0 12.6 12.8 V4.8 A1.2 1.2 0 0 0 11.4 3.6 H10.2') + p('M5.8 2.4 H10.2 V4.8 H5.8 Z')
        else if (kind === "print")         s = p('M4.6 6.2 V2.6 H11.4 V6.2') + p('M4.6 12.4 H3 A1.2 1.2 0 0 1 1.8 11.2 V7.4 A1.2 1.2 0 0 1 3 6.2 H13 A1.2 1.2 0 0 1 14.2 7.4 V11.2 A1.2 1.2 0 0 1 13 12.4 H11.4') + p('M4.6 9.8 H11.4 V14 H4.6 Z')
        else if (kind === "replace")       s = co(6, 6, 3.9) + p('M8.9 8.9 L11.4 11.4') + p('M7.6 13.2 H14 M12.6 11.6 L14.2 13.2 L12.6 14.8')
        else if (kind === "goto")          s = p('M2.6 3.8 H13.4 M2.6 7 H8 M2.6 10.2 H8') + p('M10.6 8 V13.2 M9 11.6 L10.6 13.2 L12.2 11.6')
        else if (kind === "zoom-in")       s = co(6.8, 6.8, 4.3) + p('M10 10 L14 14') + p('M4.8 6.8 H8.8 M6.8 4.8 V8.8')
        else if (kind === "zoom-out")      s = co(6.8, 6.8, 4.3) + p('M10 10 L14 14') + p('M4.8 6.8 H8.8')
        else if (kind === "zoom-reset")    s = co(6.8, 6.8, 4.3) + p('M10 10 L14 14') + ci(6.8, 6.8, 1.3)
        else if (kind === "wrap")          s = p('M2.4 4.4 H13.6') + p('M2.4 8 H10.6 A2.6 2.6 0 0 1 10.6 13.2 H8.6') + p('M10.2 11.6 L8.2 13.2 L10.2 14.8')
        else if (kind === "numbers")       s = p('M6.6 4.4 H14 M6.6 8 H14 M6.6 11.6 H14') + p('M2.4 3.6 L3.6 2.8 V7.4') + p('M2.4 10.6 H3.8 L2.4 13 H4.2')
        else if (kind === "whitespace")    s = p('M6.4 3.2 V12.8') + p('M8.8 3.2 V12.8') + p('M6.4 3.2 H11.2 A2.6 2.6 0 0 1 11.2 8.4 H6.4')
        else if (kind === "check")         s = p('M3.4 8.4 L6.4 11.4 L12.6 4.6')
        else if (kind === "comment")       s = p('M6.2 3.4 L4.4 12.6 M9.8 3.4 L8 12.6 M3.4 6.6 H12.6 M2.8 9.4 H12')
        else if (kind === "indent")        s = p('M6 3.8 H14 M6 8 H14 M6 12.2 H14') + p('M2.6 2.8 V13.2') + ci(2.6, 8, 0.7)
        // 行高：右边三条正文行，左边一个上下箭头（行距可拉大 / 缩小）
        else if (kind === "line-height")   s = p('M6 3.6 H14 M6 8 H14 M6 12.4 H14') + p('M2.6 4 V12') + p('M1.5 5.1 L2.6 4 L3.7 5.1') + p('M1.5 10.9 L2.6 12 L3.7 10.9')
        else if (kind === "select-all")    s = p('M3 4.6 H11 M3 8 H11 M3 11.4 H7.6') + p('M9.2 11.2 L10.8 12.8 L13.8 9.2')
        // 认字（图上选字）：一个四角取景框 + 里面两行字
        else if (kind === "ocr")           s = p('M2.4 4.8 V3.6 A1.2 1.2 0 0 1 3.6 2.4 H4.8') + p('M11.2 2.4 H12.4 A1.2 1.2 0 0 1 13.6 3.6 V4.8') + p('M13.6 11.2 V12.4 A1.2 1.2 0 0 1 12.4 13.6 H11.2') + p('M4.8 13.6 H3.6 A1.2 1.2 0 0 1 2.4 12.4 V11.2') + p('M5.4 6.5 H10.6 M5.4 9.5 H8.8')
        else if (kind === "lock")          s = p('M4.6 7.4 H11.4 V13.4 H4.6 Z') + p('M6.4 7.4 V5.6 A1.6 1.6 0 0 1 9.6 5.6 V7.4') + ci(8, 10.4, 1)
        /* 归档：一只带盖的箱子（盖子 + 箱体 + 中间那道拉手） */
        else if (kind === "archive")       s = p('M1.8 2.8 H14.2 V5.4 H1.8 Z') + p('M2.8 5.4 H13.2 V13.2 H2.8 Z') + p('M6.4 8.6 H9.6')
        /*
         * 翻译：一个地球（语言那个意思）。左侧图标条那一格、翻译卡片标题栏上
         * 那个置顶按钮旁边都用它/它那一族。
         */
        else if (kind === "translate")     s = co(8, 8, 6) + p('M2 8 H14')
                                                 + '<ellipse cx="8" cy="8" rx="3.2" ry="6" fill="none" stroke="' + c + '" stroke-width="1.7"/>'
        /* 置顶（固定在桌面上）：一枚图钉 —— 帽 + 针 */
        else if (kind === "pin")           s = p('M6 2.6 H10') + p('M6.8 2.6 V6.4 L5 9.4 H11 L9.2 6.4 V2.6') + p('M8 9.4 V13.4')
        /*
         * 朗读（翻译卡片标题栏那个喇叭）：
         *   speak      喇叭 + 两道声波 = 点一下把这段念出来
         *   speaking   同一个喇叭 + 三道声波 = 正在念（再点一下是停）
         *   speak-off  喇叭 + 一道斜杠 = 这件东西这会儿念不了（没语音包 / 没内容）
         *   stop       方块 = 停止（念的时候那个按钮的另一态）
         *   pause      两根竖条 = 暂停
         */
        else if (kind === "speak")         s = speakGlyph(2, c)
        else if (kind === "speaking")      s = speakGlyph(3, c)
        else if (kind === "speak-off")     s = speakGlyph(1, c) + p('M3 3 L13 13')
        else if (kind === "stop")          s = f('M4.6 4.6 H11.4 V11.4 H4.6 Z')
        else if (kind === "pause")         s = p('M6.4 3.6 V12.4') + p('M9.6 3.6 V12.4')
        else if (kind === "info")          s = co(8, 8, 6) + p('M8 7.4 V11.4') + ci(8, 4.9, 0.95)
        return "data:image/svg+xml;charset=utf-8," + encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">' + s + '</svg>')
    }
}
