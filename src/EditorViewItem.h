#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QPair>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <memory>

/*
 * QsciDocument 必须是**完整类型**（不能只前置声明）：Doc / DocRef 里各存了
 * 一份**值**（它自己就是引用计数的壳子，见 .cpp 里那段说明）。
 */
#include <Qsci/qscidocument.h>

class QWidget;
class QsciScintilla;
class QsciLexer;

class ClipboardStore;

/*
 * 用原生 QScintilla 当作编辑区（现在是**真正的编辑器**，不只是只读查看器）。
 *
 * 为什么不在 QML 里画正文（这是整个项目最花时间的一课，实测数据都在这里）：
 *
 *   同一份 668,373 字符 / 440 行的真实剪贴板内容，4K 窗口：
 *     QML ListView + 每行 Text        换条目第一帧 2538 ms   滚动 17ms/帧
 *     自绘 LineView(updatePaintNode)  换条目第一帧 2500 ms   滚动  7ms/帧
 *     QML TextArea + 换文档布局        换条目第一帧 4285 ms
 *     QScintilla（原生 widget）        换条目第一帧   21 ms
 *
 *   差别不在"算法聪不聪明"：纯 C++ 里换 QPlainTextDocumentLayout 也能到 4ms
 *   （默认精确布局 765ms），但一放进 QML 的 TextArea 就失效——QQuickTextEdit
 *   自己还要在大字符串上重算 text/length/selectedText 这些属性。
 *   只要正文以任何形式进入 QML 的文字体系，就有那 1~3 秒的同步代价。
 *   QScintilla 自带存储与视口，完全不走 QML 的文字体系，所以它没有这一段。
 *
 * 集成方式：QScintilla 是 QAbstractScrollArea（widget），QML 里放不下，
 * 所以用 QWidget::createWindowContainer() 把它包成一个原生子窗口，
 * 再作为 QQuickItem 使用。相比 QQuickWidget，这条路在"无边框 + 透明"窗口下更稳。
 *
 * 多文档：正文与状态放在 QsciDocument 里（Scintilla 自己支持一个视图挂多份
 * 文档），切换标签调 setDocument() 即可。每条文档自己记着文件路径、语言、
 * 编码、光标位置，视图级的设置（自动换行 / 行号 / 缩放）是全局的。
 *
 * 授权提醒：QScintilla 本体是 GPLv3 或 Riverbank 商业授权（见 third/qscintilla/LICENSE）。
 * 本项目按 GPLv3 使用。
 */
class EditorViewItem : public QQuickItem {
    Q_OBJECT

    /*
     * 正文左边缘距离（和外壳留白对齐）。
     *
     * 别把它当成"行号栏外面的留白"：Scintilla 画边距是从编辑器左边缘起算的
     * （Editor::PaintMargin：rcMargin.left = 0），这个值实际落在**折叠栏和
     * 正文之间**。行号离编辑器左边缘多远只看 applyMargins() 里算的边距宽。
     */
    Q_PROPERTY(int paddingLeft READ paddingLeft WRITE setPaddingLeft NOTIFY paddingChanged)
    Q_PROPERTY(int paddingTop READ paddingTop WRITE setPaddingTop NOTIFY paddingChanged)
    Q_PROPERTY(int paddingRight READ paddingRight WRITE setPaddingRight NOTIFY paddingChanged)
    Q_PROPERTY(int paddingBottom READ paddingBottom WRITE setPaddingBottom NOTIFY paddingChanged)

    Q_PROPERTY(int fontPixelSize READ fontPixelSize WRITE setFontPixelSize NOTIFY fontChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontChanged)
    /*
     * 注释字号（像素）。0 = 跟随正文字号。
     *
     * 注释的样式号是 lexer 单独给的（description 里带 "comment"），所以能给它们
     * 单独一套字号/字体 —— 这正是"注释之内的可以分开设置"那件事。
     */
    Q_PROPERTY(int commentFontPixelSize READ commentFontPixelSize WRITE setCommentFontPixelSize NOTIFY fontChanged)

    /*
     * 行高倍数（设置菜单 / 设置面板里的"行高"）。
     *
     * 1.0 = 跟随字体：就是 Scintilla 按当前字体自己算出来的行高，不额外加空。
     * 更大的值把行按比例拉开。基准取**自然行高**而不是字号 —— 字体家族换掉时
     * 字形的 ascent+descent 是不一样的（Consolas 12px 是 15px，别的字体可能
     * 是 17px），按自然行高算才不会算出比字形还矮的行。
     *
     * 实现上 Scintilla 没有"直接设行高"的口子，只有给每一行加额外上下空白
     * （SCI_SETEXTRAASCENT / SCI_SETEXTRADESCENT），差额平均加到上下两侧 ——
     * 见 applyLineSpacing()。
     */
    Q_PROPERTY(qreal lineHeightFactor READ lineHeightFactor WRITE setLineHeightFactor NOTIFY fontChanged)
    /* 现在实际的行高（像素）；设置面板显示用 */
    Q_PROPERTY(int lineHeight READ lineHeight NOTIFY fontChanged)
    /* 自然行高（像素，= 1.0 倍时的高度）；菜单里"行高 1.4 倍（21 px）"那个数 */
    Q_PROPERTY(int naturalLineHeight READ naturalLineHeight NOTIFY fontChanged)

    Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor paperColor READ paperColor WRITE setPaperColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor gutterColor READ gutterColor WRITE setGutterColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor lineNumberColor READ lineNumberColor WRITE setLineNumberColor NOTIFY colorsChanged)

    /* ---- 视图开关（全局，不随标签走） ---- */
    Q_PROPERTY(bool wrapEnabled READ wrapEnabled WRITE setWrapEnabled NOTIFY wrapChanged)
    Q_PROPERTY(bool lineNumbersVisible READ lineNumbersVisible WRITE setLineNumbersVisible NOTIFY lineNumbersChanged)
    Q_PROPERTY(bool whitespaceVisible READ whitespaceVisible WRITE setWhitespaceVisible NOTIFY whitespaceChanged)
    Q_PROPERTY(bool indentGuidesVisible READ indentGuidesVisible WRITE setIndentGuidesVisible NOTIFY indentGuidesChanged)

    /*
     * 紧贴正文左边的那条分隔竖线（"序号右边加一条竖线"）。
     *
     * 实现：把最后一条边距（行号 = 第 0 列，折叠 = 第 1 列，它 = 第 2 列）留
     * 1 像素宽、背景刷成分隔色 —— 边距背景是整列一次填满的，所以它是一条从顶
     * 到底的竖线，滚动 / 折叠 / 换语言都不影响（见 .cpp 的 applyMargins 与
     * applyMarginTheme）。
     *
     * 摆在第 2 列（折叠栏**右边**、正文紧左边）：折叠栏是 14px 宽的一列，夹在
     * 分隔线和正文之间的话，线和正文之间就空出那 14px。顺序因此是
     *     行号 | 折叠箭头 | 分隔线 | 正文
     */
    Q_PROPERTY(bool gutterLineVisible READ gutterLineVisible WRITE setGutterLineVisible NOTIFY gutterLineChanged)

    /*
     * 字数参考线（"一行 N 字"那条竖线）与它的列号。
     *
     * 走 Scintilla 的 edge（EDGE_LINE）：线画在**正文区**第 rulerColumn 列，
     * 位置按当前字体的空格宽算，跟行号栏多宽没关系；正文下方的空白区也一起
     * 画（EditView.cpp 的 rcBeyondEOF 那一支），所以它同样通到底。
     * 出厂值 120（原来 80），设置菜单里可以改（见 setRulerColumn 的夹取范围）。
     */
    Q_PROPERTY(bool rulerVisible READ rulerVisible WRITE setRulerVisible NOTIFY rulerChanged)
    Q_PROPERTY(int rulerColumn READ rulerColumn WRITE setRulerColumn NOTIFY rulerChanged)

    Q_PROPERTY(bool foldingEnabled READ foldingEnabled WRITE setFoldingEnabled NOTIFY foldingChanged)
    Q_PROPERTY(bool readOnly READ readOnly WRITE setReadOnly NOTIFY readOnlyChanged)
    /*
     * 当前缩放百分比。
     *
     * 本来只读（缩放是命令式的：zoomIn / zoomOut 直接改 Scintilla 的 zoom），
     * 现在**可写** —— 分栏之后第二栏要跟着主栏走，而"跟"只能靠 QML 绑定，
     * 绑定就得有个可写的口子（见 EditorArea.qml 里 mirrorPane 的 zoomPercent）。
     */
    Q_PROPERTY(int zoomPercent READ zoomPercent WRITE setZoomPercent NOTIFY zoomChanged)

    /* ---- 文档 / 标签 ---- */
    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY documentsChanged)    Q_PROPERTY(QVariantList documents READ documents NOTIFY documentsChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY currentChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY currentChanged)
    Q_PROPERTY(bool modified READ modified WRITE setModified NOTIFY modifiedChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(QString encoding READ encoding WRITE setEncoding NOTIFY encodingChanged)
    Q_PROPERTY(QString eolMode READ eolMode WRITE setEolMode NOTIFY eolChanged)

    /* ---- 光标与统计 ---- */
    Q_PROPERTY(int cursorLine READ cursorLine NOTIFY cursorChanged)
    Q_PROPERTY(int cursorColumn READ cursorColumn NOTIFY cursorChanged)
    Q_PROPERTY(int selectionLength READ selectionLength NOTIFY cursorChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY cursorChanged)
    Q_PROPERTY(int lineCount READ lineCount NOTIFY statsChanged)
    Q_PROPERTY(int charCount READ charCount NOTIFY statsChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStateChanged)

    /* 诊断：上次灌入正文的耗时（毫秒） */
    Q_PROPERTY(int lastLoadMs READ lastLoadMs NOTIFY statsChanged)
    Q_PROPERTY(int lastLoadChars READ lastLoadChars NOTIFY statsChanged)

    /* 当前是否已有内容，外壳据此决定要不要显示占位 */
    Q_PROPERTY(bool hasContent READ hasContent NOTIFY statsChanged)

public:
    explicit EditorViewItem(QQuickItem *parent = nullptr);
    ~EditorViewItem() override;

    void setStore(ClipboardStore *store) { m_store = store; }

    /*
     * 全局数据源。
     *
     * QML 里的 EditorView 是 QML 自己构造的实例，main.cpp 拿不到它，
     * 所以由 main 在加载 QML 之前设一次，实例构造时自动接上。
     */
    static void setGlobalStore(ClipboardStore *store) { s_store = store; }

    /*
     * 全局宿主 QWidget。
     *
     * QScintilla 必须挂在主窗口的 QWidget 层级里，才能跟着窗口走、
     * 并被 QML 的圆角裁剪。主窗口由 main.cpp 创建，这里静态传进来，
     * 实例构造时自动接上。
     */
    static void setGlobalHostWidget(QWidget *host) { s_hostWidget = host; }

    /* 当前活着的编辑器实例（快捷键需要知道命令发给谁） */
    static EditorViewItem *instance() { return s_instance; }

    /*
     * 让**所有**编辑器实例按当前 QML 布局重新摆一次原生控件（主编栏 + 分栏镜像）。
     *
     * 为什么需要它（用户报的"最大化时编辑区还挂在旧坐标上飘着"）：
     * 编辑区那个原生 QScintilla 子窗的位置本来是 applyGeometry() 里按 QML 布局算的，
     * 而它挂在 QML 的 geometryChange 上 —— 主窗口换尺寸那一下，QML 布局和窗口几何
     * 谁先谁后不保证；窗口先变大的话，编辑器就在旧坐标上露一帧（屏幕上就是"一块
     * 带着旧内容的窗口飘在中间"）。WindowHelper 在换几何**之前**调一次这个，
     * 让它先按新布局摆到目标坐标上。
     */
    static void syncAllGeometry();

    /*
     * 把"按 QML 布局摆原生控件"这一步**冻住**（on=false 才放行）。
     *
     * 现在只有一个调用方：WindowHelper 预热最大化的那几十到几百毫秒。那边趁窗口还是
     * 卡片大小时就把内容控件摆到了 4K，QML 一 resize，编辑区这条 geometryChange 就会
     * 把原生 QScintilla 子窗一起撑到 4K —— 而那是块独立合成表面，宿主贴的"预热前那一
     * 张"盖不住它，开了换行的文档会当场重排，看着就是"正文动了、别的全冻住"。
     *
     * 冻住的是**摆坐标**这一步，不是显示状态（显示状态归 setAllNativeVisible 管）。
     */
    static void setGeometryFrozen(bool frozen);

    /*
     * 过渡期把编辑区那块**原生子窗口**摘出屏幕 / 放回去。
     *
     * 实测依据（144fps 逐帧，素材 build\uc2\b84.png）：最大化中间那些帧里，整屏已经
     * 被宿主铺成黑的了，**左上角却还亮着一块旧编辑区**（行号 1..11 + 正文看得清清楚楚）。
     * 因为 QScintilla 是 createWindowContainer() 出来的独立原生子窗，自己一块合成表面，
     * 不参与宿主的 backing store —— 宿主那一层铺什么底色都盖不到它。而它的坐标在换几何
     * 之前就已经按新布局摆好了（超出旧窗口那圈被旧窗口裁掉），窗口一变大就整块露出来。
     * 这就是用户报的"原始窗口先闪到左上角"里，唯一**有内容**的那一块。
     *
     * 传 false 摘出去，传 true 放回来（放回来时按当前 QML 布局重摆一遍，
     * 该不该显示仍由各实例自己的 isVisible() 说了算 —— 预览页里编辑区本来就该是隐藏的）。
     */
    static void setAllNativeVisible(bool on);

    /*
     * 代码折叠（第 1 列那个折叠边距）与当前行行号高亮，都在 applyMargins /
     * applyMarginTheme 里落地，见 .cpp 里的说明。
     */
    bool foldingEnabled() const { return m_folding; }
    void setFoldingEnabled(bool on);
    Q_INVOKABLE void foldAll();
    Q_INVOKABLE void unfoldAll();

    /* 某一行当前是否可见（折叠起来就不可见） */
    Q_INVOKABLE bool lineVisible(int line) const;

    /*
     * 自检用：边距 / 样式的颜色与宽度。
     *
     * 行号栏那条白带就是因为边距背景没跟着主题走（Scintilla 里
     * SC_MARGIN_NUMBER 的背景取自 STYLE_LINENUMBER 的 paper，
     * 而装 lexer 时的 SCI_STYLECLEARALL 会把它刷回默认的白色）。
     */
    Q_INVOKABLE int marginWidth(int margin) const;
    Q_INVOKABLE int marginBack(int margin) const;
    Q_INVOKABLE int styleBack(int style) const;
    Q_INVOKABLE int styleFore(int style) const;

    /*
     * 自检用：边距区域（行号栏 + 折叠栏）的像素统计，返回
     *   [0] 行号栏里的墨点（数字）
     *   [1] 折叠栏里的墨点（折叠标记）
     *   [2] 纯白像素个数（必须为 0 —— 白带就是它）
     *   [3] 行号栏右侧那条分隔竖线的像素（关掉开关就是 0）
     *   [7][8][9] 三条边距的宽度（0 行号 / 1 折叠 / 2 分隔线）；[10] 分隔线起始列
     *   [12] 缩进参考线的像素
     *
     * 抓的是控件自己渲染出来的图，不依赖窗口是不是在前台 ——
     * 在真实窗口上截屏量像素会被别的窗口盖住，实测不可靠。
     */
    Q_INVOKABLE QVariantList marginPixelStats() const;

    /*
     * 折叠尖括号位图的边长（逻辑像素）。
     *
     * 跟着正文字号走（12px 字号 → 10px），夹在 8~16 之间；折叠栏宽度就是
     * 它 + 左右各 5px。自检按同一个函数核宽度，避免两处各写一份公式。
     */
    Q_INVOKABLE int foldIconSize() const;

    /*
     * 自检用：两个折叠尖括号位图里墨迹的包围盒，返回
     *   { 折叠态宽, 折叠态高, 展开态宽, 展开态高 }
     *
     * 钉的是方向：折叠态是向右的 "›"（竖着比横着长），展开态是向下的 "⌄"
     * （横着比竖着长）。位图标记没法从 Scintilla 那边读回形状，所以直接量自己
     * 画出来的那张图。
     */
    Q_INVOKABLE QVariantList foldIconPixelStats() const;

    /*
     * 自检用：参考线在 Scintilla 那边的实际状态（读的是 edge 消息，不是成员变量
     * —— 钉的是"设置真的下发到了内核"）。
     *   rulerEdgeMode   —— 0 = 不画（EDGE_NONE），1 = 竖线（EDGE_LINE）
     *   rulerEdgeColumn —— 线的列号
     *   rulerEdgeColor  —— 线的颜色（按 Scintilla 的 BGR 打包，配合 packed() 比）
     */
    Q_INVOKABLE int rulerEdgeMode() const;
    Q_INVOKABLE int rulerEdgeColumn() const;
    Q_INVOKABLE int rulerEdgeColor() const;

    /*
     * 自检用：参考线在控件上**画出来**的像素位置。
     * 返回 { found, expected, bottomGap }：found = 抓图里扫到的列（-1 = 没扫到），
     * expected = 按"边距 + 左留白 + 列号 × 空格宽"算出来的位置，
     * bottomGap = 这条线最低那点离控件底边还有几像素（0 = 一直补到底）。
     *
     * 光看 edgeColumn 只是"消息发对了"，这条是"线真的落在 120 个字的地方"——
     * 抓的是控件自己渲染的图，不依赖窗口在前台（同 marginPixelStats）。
     */
    Q_INVOKABLE QVariantList rulerPixelStats() const;

    /*
     * 自检用：编辑区底边那块补线控件的状态（见 .cpp 的 BottomLines）。
     *   [0] 可见  [1] 鼠标穿透  [2] 不画背景  [3] 自动填背景  [4] 补了几条线  [5] 高度
     */
    Q_INVOKABLE QVariantList bottomLinesState() const;

    /*
     * 自检用：编辑控件**最底下那一行**（横向滚动条占的那一条）从左到右扫一遍。
     *
     * 钉的是"滑块不许爬到序号栏底下"。量的是抓图里的墨迹，不是属性值：
     * 样式表里 margin-left 写没写、生效没生效，属性上看不出来（Qt 不会把它
     * 回报成几何变化），只有那一行的像素能说话。
     *
     * 返回 { 内缩(实际生效), 序号栏宽(该内缩多少), 行高, 序号栏带子里的墨迹数,
     *       内缩右侧的滑块墨迹数, 抓图缩放×1000, 抓图宽 }。
     *   - 分界线是**现读的边距宽**（gutterRightEdge），不是实际生效的那个内缩：
     *     拿内缩当界，旧行为（内缩 0）扫出来也是干净的，那条绿就是空转；
     *   - 第 4 项：界左边"既不是正文底色、也不是分隔线"的像素，旧行为量到的是
     *     滑块（深灰 #4b4d4f）压在行号栏底下，现在必须是 0；
     *   - 第 5 项：界右边数得到的滑块像素，必须 **> 0** —— 不然上一条的 0
     *     只是"横条根本没画出来"。
     * 横条没出现（行高 0）时前三项给 -1，不能当成 0 混过去。
     */
    Q_INVOKABLE QVariantList horizontalBarRowStats() const;

    /*
     * 自检用：同一件事在**真桌面**上再量一遍。
     *
     * 上面那条抓的是控件自己重画的图；样式表里那个 margin-left 到底把轨道挪开
     * 没有、挪出来的那一条屏幕上是什么，只有合成完的那一帧算数。
     *
     * 返回 { 量到了吗, 内缩, 序号栏宽, 行高, 带子里的墨迹, 右侧滑块墨迹, 屏 DPR×1000 }。
     * "量到了吗" = 那一帧里数得出滑块 —— 窗口被盖住时数不出来，那就是没量到，
     * 既不算绿也不算红。
     */
    Q_INVOKABLE QVariantList horizontalBarRowScreenStats() const;

    int paddingLeft() const { return m_paddingLeft; }
    void setPaddingLeft(int v);
    int paddingTop() const { return m_paddingTop; }
    void setPaddingTop(int v);
    int paddingRight() const { return m_paddingRight; }
    void setPaddingRight(int v);
    void setPaddingBottom(int v);
    int paddingBottom() const { return m_paddingBottom; }

    int fontPixelSize() const { return m_fontPixelSize; }
    void setFontPixelSize(int px);

    /*
     * 正文字体家族（默认 Consolas）。
     *
     * 说明一下"中英文分开设字体"这件事为什么没做成：Scintilla 的样式是按
     * **词法记号**分段（关键字 / 注释 / 字符串…），不是按"这个字是汉字还是
     * 拉丁字母"分段，同一个样式只能有一个字体名。汉字能正常显示，靠的是
     * 系统缺字回退（QFont 在 Consolas 里找不到汉字时自动换成中文字体）——
     * 但回退字体和字号是系统定的，没法单独设。
     * 能做的替代：把正文字体整个换成中英都覆盖的等宽字体（新宋体 / 更纱黑体
     * 之类），中英混排就整齐了 —— 就是下面这个 fontFamily。
     */
    QString fontFamily() const { return m_fontFamily; }
    void setFontFamily(const QString &family);

    int commentFontPixelSize() const { return m_commentFontPixelSize; }
    void setCommentFontPixelSize(int px);

    qreal lineHeightFactor() const { return m_lineHeightFactor; }
    void setLineHeightFactor(qreal factor);

    /*
     * 实际行高 / 自然行高，单位像素。
     *
     * 自然行高 = 现在这一行的高度**减掉**自己加的那份额外上下空白
     * （SCI_GETEXTRAASCENT / DESCENT），也就是"1.0 倍时的高度"。
     * 没开文档时两者都是 0。
     */
    int lineHeight() const;
    int naturalLineHeight() const;

    QColor textColor() const { return m_textColor; }
    void setTextColor(const QColor &c);

    QColor paperColor() const { return m_paperColor; }
    void setPaperColor(const QColor &c);

    QColor gutterColor() const { return m_gutterColor; }
    void setGutterColor(const QColor &c);

    QColor lineNumberColor() const { return m_lineNumberColor; }
    void setLineNumberColor(const QColor &c);

    bool wrapEnabled() const { return m_wrap; }
    void setWrapEnabled(bool on);

    bool lineNumbersVisible() const { return m_lineNumbers; }
    void setLineNumbersVisible(bool on);

    bool whitespaceVisible() const { return m_whitespace; }
    void setWhitespaceVisible(bool on);

    /*
     * 缩进参考线（代码前面那些竖直的竖线）。
     *
     * 独立开关、独立颜色：以前它是挂在"显示空白字符"上的副产物，一开空白
     * 就顺带出来，而且颜色跟着 STYLE_DEFAULT 走（装 lexer 后 STYLECLEARALL
     * 会把它刷成正文色，亮得刺眼）。
     */
    bool indentGuidesVisible() const { return m_indentGuides; }
    void setIndentGuidesVisible(bool on);

    /* 行号栏右侧的分隔竖线（见 Q_PROPERTY 里的说明） */
    bool gutterLineVisible() const { return m_gutterLine; }
    void setGutterLineVisible(bool on);

    /* 字数参考线（列号以**字符**为单位，1 = 第 1 个字后面） */
    bool rulerVisible() const { return m_rulerVisible; }
    void setRulerVisible(bool on);
    int rulerColumn() const { return m_rulerColumn; }
    void setRulerColumn(int column);

    bool readOnly() const { return m_readOnly; }
    void setReadOnly(bool on);

    int zoomPercent() const { return m_zoomPercent; }
    /* 直接定一个缩放百分比（夹在 -10 ~ 20，和 zoomIn / zoomOut 一个范围） */
    void setZoomPercent(int percent);

    /*
     * 这一栏当前有没有打开着文档。
     *
     * 注意判的是**这一栏的标签列表**（m_open），不是池子 —— 分栏之后池子里
     * 有东西不代表这一栏开着什么。
     */
    bool hasDocument() const { return m_current >= 0 && m_current < m_open.size(); }
    QVariantList documents() const;
    int currentIndex() const { return m_current; }
    QString filePath() const;
    QString displayName() const;
    bool modified() const;
    void setModified(bool m);
    QString language() const;
    void setLanguage(const QString &id);
    QString encoding() const;
    void setEncoding(const QString &name);
    QString eolMode() const;
    void setEolMode(const QString &name);

    int cursorLine() const;
    int cursorColumn() const;
    int selectionLength() const;
    bool hasSelection() const;
    int lineCount() const;
    int charCount() const;
    bool canUndo() const;
    bool canRedo() const;

    int lastLoadMs() const { return m_lastLoadMs; }
    int lastLoadChars() const { return m_lastLoadChars; }
    bool hasContent() const { return m_hasContent; }

    /* 支持的语言列表 [{ id, label }]，给"语言"下拉用 */
    Q_INVOKABLE QVariantList languages() const;

    /* 语言 id -> 显示名（状态栏、工具栏下拉按钮用） */
    Q_INVOKABLE QString languageLabel(const QString &id) const;

    /* ---- 文档生命周期 ---- */
    Q_INVOKABLE int newDocument();
    Q_INVOKABLE int openFile(const QString &path);
    Q_INVOKABLE bool saveCurrent();
    Q_INVOKABLE bool saveCurrentAs(const QString &path);
    Q_INVOKABLE bool saveDocument(int index, const QString &path);
    Q_INVOKABLE void closeDocument(int index);
    Q_INVOKABLE void closeCurrent();
    Q_INVOKABLE void closeAll();
    Q_INVOKABLE void activateDocument(int index);
    Q_INVOKABLE int activateNextDocument();
    Q_INVOKABLE int activatePreviousDocument();
    Q_INVOKABLE int indexOfPath(const QString &path) const;
    Q_INVOKABLE QString lastError() const { return m_lastError; }

    /*
     * 文件在磁盘上被改名之后，把打开着的那个标签也跟着改过来。
     *
     * 左树右键"重命名"改的是真实文件；标签里缓存着旧路径，不跟着换的话
     * 再按 Ctrl+S 就会往一个已经不存在的老名字上写。
     * 返回有没有找到那个标签。
     */
    Q_INVOKABLE bool updateDocumentPath(const QString &oldPath, const QString &newPath);

    /* 当前文档的正文（诊断 / 自检用；正常编辑不经过 QML 属性） */
    Q_INVOKABLE QString currentText() const;

    /* 释放原生子窗口的资源（QML 侧在组件销毁时调用） */
    Q_INVOKABLE void detach();

    /* ---- 编辑命令 ---- */
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void cut();
    Q_INVOKABLE void copy();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void duplicateLine();
    Q_INVOKABLE void deleteLine();
    Q_INVOKABLE void toggleComment(const QString &prefix);

    /* 复制当前行 / 复制全文到系统剪贴板 */
    Q_INVOKABLE void copyCurrentLine();
    Q_INVOKABLE void copyAll();

    /* ---- 查找 / 替换 ---- */
    Q_INVOKABLE int find(const QString &text, bool caseSensitive, bool wholeWord,
                         bool regex, bool forward);
    Q_INVOKABLE int highlightMatches(const QString &text, bool caseSensitive,
                                     bool wholeWord, bool regex);
    Q_INVOKABLE void clearHighlights();
    Q_INVOKABLE bool replaceCurrent(const QString &text, const QString &replacement,
                                    bool caseSensitive, bool wholeWord, bool regex);
    Q_INVOKABLE int replaceAll(const QString &text, const QString &replacement,
                               bool caseSensitive, bool wholeWord, bool regex);
    Q_INVOKABLE void gotoLine(int line);

    /*
     * 校验结果（中文用词 / 代码语法，见 src/Checker.h）。
     *
     * Main.qml 在 Check.issuesChanged 里把 Check.issues 整份推过来：每一处在
     * 正文里画一条波浪线，鼠标停上去弹一个说明框（见 .cpp 里 setCheckIssues /
     * eventFilter 的 ToolTip 分支）。每项的口径和 Checker 那边一致：
     *   { row, col, endRow, endCol, severity, message, snippet, source, suggestion }
     *
     * 正文一改、或者换了标签，这些波浪线就作废了（位置不再对得上），
     * 所以在 textChanged / 切文档 / 关标签那几处统一清掉。
     */
    Q_INVOKABLE void setCheckIssues(const QVariantList &issues);
    Q_INVOKABLE void clearCheckIssues();

    /*
     * 自检用：现在画着波浪线的那几段在文档里的**字节**位置
     * （[起0, 止0, 起1, 止1, …]，空表 = 一条都没有）。
     *
     * 校验给的是"第几行、这一行第几个字"，Scintilla 要的是字节位置，中文一个字
     * 三字节 —— 这块换算错了波浪线就画在旁边的字上，所以留个口子让自检钉住。
     */
    Q_INVOKABLE QVariantList checkIssueRanges() const;

    /*
     * 自检用：正文区里**波浪线**的像素统计 { 错误色像素数, 警告色像素数, 第一个
     * 命中像素的 x, y }（没有就是 0,0）。查的是"那条线真的画出来了"——
     * 光有区间还说明不了它被渲染出来。
     */
    Q_INVOKABLE QVariantList checkWavePixelStats() const;

    /*
     * 自检用：鼠标停在控件坐标 (x, y) 上会弹出来的那段说明（空串 = 那儿没有波浪线）。
     * 悬浮那条路走的就是它，所以自检跟真鼠标看到的是同一份文字。
     */
    Q_INVOKABLE QString checkTipAtPoint(int x, int y) const;

    /* ------------------------------------------------------------------
     * 文件对比的绘制层（见 src/Diff.h + qml/components/DiffPane.qml）
     *
     * BC 那种"两栏一行对一行"要的是**空白带**，而往正文里插真空行会把缓冲区、
     * 撤销栈、行号、保存点全弄脏（用户就是要在对比页里直接改字）。所以走
     * Scintilla 的行注释（annotation）：它只给那一行**加显示高度**，
     * Document::AnnotationSetText 既不写撤销栈也不动保存点，正文一个字没变。
     *
     * 整行底色走 INDIC_FULLBOX 指示器：它压在字底下，语法高亮那套颜色一个不丢。
     * （SC_MARK_BACKGROUND 那个"理论上铺满整行"的走法试过、量不出来，
     *   理由记在 .cpp 的 beginDiff 里。）
     *
     * 这些**都是画在文档上的**（指示器、行注释都存在 QsciDocument 里，
     * 不是存在视图里）：所以对比页退出时必须 endDiff() 清干净，不然同一份文档
     * 在普通标签页里会带着这些色块。
     * ---------------------------------------------------------------- */

    /* 开始一轮对比：配好用的样式 / 指示器，并把上一轮的痕迹清掉 */
    Q_INVOKABLE void beginDiff();
    /* 退出对比：清空行底色、字级高亮、空白带 */
    Q_INVOKABLE void endDiff();

    /* 在第 line 行**下面**撑出 lines 行高的空白带（0 = 撤掉） */
    Q_INVOKABLE void setDiffGap(int line, int lines);
    /* 第 line 行下面现在撑着几行空白（自检量对齐用） */
    Q_INVOKABLE int diffGapAt(int line) const;

    /* 整行涂一种差异底色：kind = "del" / "add" / "mod" / "same" */
    Q_INVOKABLE void setDiffLineKind(int line, const QString &kind);
    /*
     * 当前选中的那一处差异：把这一行涂成**更亮一档**的同族色。
     * kind 传 "same" 或空串 = 把这一行的"当前"标记撤掉。
     */
    Q_INVOKABLE void setDiffCurrent(int line, const QString &kind);

    /*
     * 一行里的**字级**差异区间：cols 是 [起列, 长度, 起列, 长度, …]。
     * 列按**字符**数（不是字节 —— 中文一个字三个字节，换算只在这里做）。
     */
    Q_INVOKABLE void setDiffWordMarks(int line, const QVariantList &cols);

    /* 第 line 行在正文坐标里的上沿 y（自检量"两栏对没对上"量的就是它） */
    Q_INVOKABLE int lineTopY(int line) const;
    /* 控件坐标 (x, y) 落在文档的第几行（点右侧导航条 / 拖动定位用；-1 = 不在正文里） */
    Q_INVOKABLE int lineAtPoint(int x, int y) const;

    /* 自检用：第 line 行、x 那一处的颜色（ARGB 整数；x < 0 = 从右边数过去） */
    Q_INVOKABLE int diffPixelAt(int line, int x) const;

    /*
     * 自检用：把第 line 行**整行扫一遍**，报 { band, paper, glyph } 三个像素数
     * （band = 和底色 bandHex 相同的像素数，glyph = 既不是底色也不是正文底色的）。
     *
     * 为什么不是量一个点：单点会正好撞到字形笔画上，量出来是字的颜色，说明不了
     * 底色在不在；而"底色铺开了"和"字还看得见"这两件事必须**同时**成立
     * （SC_MARK_BACKGROUND 并进边距掩码那一版就是铺开了但把字糊掉了，单点量不出来）。
     */
    Q_INVOKABLE QVariantMap diffRowStats(int line, const QString &bandHex) const;

    /*
     * 两栏滚动同步用的四个口子。
     *
     * 口径是**显示行**（SCI_GETFIRSTVISIBLELINE）不是文档行：差异挂成行注释之后，
     * 两栏的第 k 个显示行就是同一逻辑行，所以"同步行号"本身就是对齐，
     * 不需要再查一遍映射表。
     */
    Q_INVOKABLE int firstVisibleLine() const;
    Q_INVOKABLE void setFirstVisibleLine(int line);
    Q_INVOKABLE int viewXOffset() const;
    Q_INVOKABLE void setViewXOffset(int x);

    /*
     * 用给定内容换掉第 startLine 行起的 count 行（count = 0 就是插进去）。
     * 对比页上"左 → 右 / ← 右"那两个合并键走它。
     *
     * 整段走 BEGINUNDOACTION / ENDUNDOACTION 包成**一步**撤销：用户按一次
     * Ctrl+Z 就该把这一处合并整个收回，而不是收到半截。
     */
    Q_INVOKABLE void replaceLines(int startLine, int count, const QVariantList &lines);

    /*
     * 整份替换（格式化 / 批量改写用），**可撤销的一步**（见 .cpp 里的说明）。
     * 失败时调用方不该调它 —— 传进来的必须是"确定要写回去的正文"。
     */
    Q_INVOKABLE void setText(const QString &text);

    /* ---- 视图 ---- */
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void zoomReset();
    Q_INVOKABLE void printDocument();

    /*
     * 焦点在 QML 与原生编辑控件之间来回。
     *
     * 查找栏 / 弹窗要能用键盘输入，就必须把原生 QScintilla 的焦点交还给
     * QQuickWidget，否则按键全被原生控件吃掉（它是独立的原生子窗口）。
     */
    Q_INVOKABLE void requestEditorFocus();
    Q_INVOKABLE void releaseEditorFocus();

    /* 键盘焦点是不是在原生编辑控件上（工具栏按钮点完要能直接打字） */
    Q_INVOKABLE bool hasEditorFocus() const;

    /*
     * 自检用：当前行底色的 alpha。
     *
     * 必须是 256（SC_ALPHA_NOALPHA）。给 255 会让 Scintilla 把这层底色叠在
     * 文字**上面**，整行字就看不见了 —— 这个值光看代码很像"不透明"，容易写错。
     *
     * 顺带记一笔：这个坑在进程内是量不到像素的（控件 grab() 出来的图里
     * 根本没有当前行那层底色，把 alwaysShowCaretLineBackground 打开也一样），
     * 只能靠这个值本身来卡，或者真在窗口上截屏去数。
     */
    Q_INVOKABLE int caretLineAlpha() const;

    /*
     * 自检用：某个样式号的字号（返回 Scintilla 的"点 × 100"）。
     *
     * 装 lexer 之后样式字号必须还是 uiFont() 的点数（正数），不能变成负值 ——
     * 见 uiFont() 的注释。要和 stylePointSize() 相等。
     */
    Q_INVOKABLE int styleSize(int style) const;

    /* 自检用：当前字号换算成 Scintilla 的"点 × 100"（uiFont() 的同一份换算） */
    Q_INVOKABLE int stylePointSize() const;

    /* 自检用：注释样式该用的字号（"点 × 100"）；没单独设就跟着正文 */
    Q_INVOKABLE int styleCommentPointSize() const;

    /* 自检用：某个样式号用的字体名（SCI_STYLEGETFONT） */
    Q_INVOKABLE QString styleFontName(int style) const;

    /*
     * 自检用：把字号换算回**像素**（设置里写的单位）。
     *
     * 卡"设置写 12、渲染出来就是 12 像素"——曾经这里把 12 当成"点"直接给 QFont，
     * 96 DPI 下画出来是 16 像素，比界面大一圈。
     */
    Q_INVOKABLE double fontPixelSizeEffective() const;

    /*
     * 自检用：正文一行的实际像素高度（Scintilla 按当前字体算出来的）。
     *
     * 之前这里数是"抓图里有墨的扫描行数"，但抓图受滚动条、可见区高度影响，
     * 同一个字号在不同文档状态下能差出一倍，卡不准。行高是 Scintilla 自己
     * 算的，装不装 lexer 必须一样 —— 字号被带坏成负值时它会直接塌掉。
     */
    Q_INVOKABLE int textLineHeight() const;

    /*
     * 自检用：这个文档里最高的一行比最矮的一行高多少像素（0 = 每行一样高）。
     *
     * 卡的是"同一份文档内部"行高不齐（某一行用了一个偏大的样式）。文档**之间**
     * 不齐是另一个根，见下面 offFamilyStyleSlots()。
     */
    Q_INVOKABLE int lineHeightSpread() const;

    /*
     * 自检用：样式表里有多少格的字体族**不是**正文字体。
     *
     * 必须恒为 0。不为 0 的那些格会按**应用字体**量高度（本机 YaHei UI 9pt = 16px，
     * Consolas 12px = 15px），而 Scintilla 的行高取整张表里最大的 ascent+descent ——
     * 一格用不到的空样式就能把全篇每行撑高 1px，行号栏和正文跟着错开。
     * 见 EditorViewItem.cpp 的 unifyStyleFonts()。
     */
    Q_INVOKABLE int offFamilyStyleSlots() const;

    /*
     * 自检用：正文区（不含行号栏/折叠栏）里"纯白"像素的个数。
     *
     * 专门查"底色被刷成白色"这类问题：正文色是 #d6d7da、底色是 #1e1f22，两者
     * 混出来的像素都在 250 以下，所以只要出现接近 #ffffff 的像素，就说明有一块
     * 白底（用户报的"切到纯文本，字体全带上白色背景"就是这么发现的）。
     */
    Q_INVOKABLE int whiteBackgroundPixels() const;

    /*
     * 自检用：两条滚动条自己那块画面的像素统计。
     *
     * 查的是"轨道没人画、露出底下一层"这类问题：滑块的深灰来自样式表，一眼
     * 就能对上；轨道早先写的是 background: transparent（= 这一层不画），露出
     * 来的就是底下那层 —— 实测有机器上是一条 12px 的 #f2f2f2 白带（用户报的
     * "滚动条白色背景"）。现在轨道色写死成正文底色，这里直接抓滚动条控件自己
     * 渲染出来的图数一遍，钉住"一个近白像素都不许有"。
     *
     * 返回 [竖条近白像素, 横条近白像素, 竖条可见, 横条可见,
     *       竖条轨道色, 横条轨道色]（颜色是 0xRRGGBB，跟 "#" 后面那串一致）。
     * 滚动条不可见时对应的近白像素给 -1（"没得量"，不能当成 0 混过去）。
     */
    Q_INVOKABLE QVariantList scrollBarPixelStats() const;

    /*
     * 自检用：横向滚动条的状态。
     *
     * 钉的是"内容没撑满就不该有横条"这条（见 .cpp 里 updateHorizontalScroll
     * 那段说明）：Scintilla 判显隐用的是**一页文本宽** GetTextRectangle().Width()
     * —— viewport 扣掉行号/折叠那几条边距和左右留白之后的宽度，不是 viewport 宽。
     * 拿后者当界，短内容也会一直挂着一条横条、还能向右滚几十像素。
     *
     * 返回 visible / maximum / value / pageStep（Scintilla 的一页宽）/ pageWidthComputed
     * （自己按公式算的一页宽，用来核口径）/ viewportWidth / scrollWidth /
     * contentWidth（最近一次量到的最长行宽）/ wrap。
     */
    Q_INVOKABLE QVariantMap horizontalScrollState() const;

    /*
     * 自检用：把编辑控件拉窄 deltaWidth，**当场**读一次横条状态，再把宽度还原。
     *
     * 拖分栏分隔线 / 拉窗口走的就是这一个效果：编辑控件宽度变了、一页文本宽
     * （hNewPage）跟着变小，而我们这边重算横向范围是排到下一轮的
     * （applyGeometry 里那个 singleShot）。返回的就是"宽度已经变了、重算还没轮到"
     * 的那一拍 —— 用户报的"拖分隔线时底下横条一闪一闪"必须在那一拍里量，
     * 转一次事件循环就看不到了。
     */
    Q_INVOKABLE QVariantMap horizontalScrollAfterNarrowForTest(int deltaWidth);

    /*
     * 自检用：这一栏的 geometry —— QML item（场景坐标）和它那块**原生控件**
     * 各摆在哪、多大。
     *
     * 两边必须一样大：原生控件是子窗口，尺寸是 applyGeometry() 从 item 几何
     * 算出来的。只对上 item、对不上控件时，屏幕上就是"该分的地方没分开"
     * —— 用户报的"上下分栏没有展开"就是这种（分栏时 item 高度变了，
     * 但那块原生窗口还停在旧高度上，把分隔线整个盖住）。
     */
    Q_INVOKABLE QVariantMap paneGeometryForTest() const;
    /* 自检用：正文区里和 hex 差得<=12 的像素有多少个 / 取消选择 */
    Q_INVOKABLE int dbgCountNearForTest(const QString &hex) const;
    Q_INVOKABLE void dbgClearSelectionForTest();
    /* 光标位置存/取：上面那个"取消选择"会把光标留在选区末尾，用例之间得靠它还原 */
    Q_INVOKABLE QPoint dbgCaretForTest() const;
    Q_INVOKABLE void dbgSetCaretForTest(int line, int col);

    /*
     * 滚动条的右键动作（"滚动到这里 / 左边缘 / 翻页 / 滚一行"那七条）。
     *
     * axis 传 "h" / "v"，what 传下面这几个之一：
     *   here        滚到刚才右键点住的那个位置
     *   edgeStart   顶边 / 左边缘
     *   edgeEnd     底边 / 右边缘
     *   pageBack    向上一页 / 向左一页
     *   pageForward 向下一页 / 向右一页
     *   lineBack    向上滚一行 / 向左滚一格
     *   lineForward 向下滚一行 / 向右滚一格
     *
     * 落地就是 QScrollBar::triggerAction()，和 Qt 自带那个菜单内部走的是同一套
     * （见 .cpp 里的说明：为什么要自己弹菜单）。
     */
    Q_INVOKABLE void scrollBarAction(const QString &axis, const QString &what);

    /*
     * 自检用：给滚动条发一个右键事件。
     *
     * 返回"这个事件是不是被我们吃掉了"—— 吃掉才意味着 Qt 那个浅色英文的
     * 原生菜单不会弹出来（界面上换成 QML 那套深色菜单，见 Main.qml）。
     */
    Q_INVOKABLE bool triggerScrollBarContextMenu(bool horizontal, int pos = -1);

    /* ------------------------------------------------------------------
     * 分栏（两个**独立**编辑组，同一份文档池 —— 和 VS Code 一样）
     *
     * 这是第二版做法。第一版是"镜像"：右栏由左栏把正文推过来、只读，
     * 两栏永远显示同一份文档（见 git 历史）。用户要的是 VS Code 那种：
     * 两栏各有自己的标签栏，可以**各自**看不同的文件；看同一份文件时，
     * 在任意一栏改，另一栏立刻就变。
     *
     * 做法：文档池共享，视图状态各自一份。
     *
     *   Doc（这个类私有的那个结构）现在用 shared_ptr 持有，两个栏拿到的是
     *   **同一个** Doc —— 正文（QsciDocument）、路径、语言、修改标记只有一份，
     *   所以"在左边打字右边立刻变"是天然的（它们本来就是同一份文档），
     *   不需要任何同步代码。
     *
     *   每个栏另有自己的一份 DocRef：光标 / 滚动位置是**视图状态**，两栏
     *   各看各的位置才对（用户就是想让它们对着文件的不同地方）。
     *
     *   QsciDocument 自己就是引用计数的壳子（见 third/qscintilla 的
     *   qscidocument.cpp）：每个栏把自己那份 DocRef 的 m_doc 指向同一个底层
     *   文档时，引用计数会自己涨；谁最后放手谁负责归还。所以这里**不需要**
     *   自己写一套引用计数。
     *
     * 池子里有哪些文档、什么时候加一个 / 删一个，由静态的 s_pool 广播给
     * 所有活着的实例（见 .cpp 的 broadcastPool）：任何一栏打开 / 关闭文件，
     * 别的栏的池子跟着加 / 减，只是**不跟着切标签**（切标签是每个栏自己的事）。
     * ------------------------------------------------------------------ */

    Q_PROPERTY(int docId READ docId WRITE setDocId NOTIFY boundChanged)
    Q_PROPERTY(bool mirror READ mirror WRITE setMirror NOTIFY boundChanged)
    Q_PROPERTY(bool bound READ bound NOTIFY boundChanged)

    /*
     * "这一份是主编辑器"（由 QML 显式指定，见 .cpp 构造函数里那段说明）。
     *
     * 分栏之后工程里有两个 EditorViewItem，而 instance()（"当前编辑器是谁"）
     * 只能有一个答案 —— 靠"最后构造"来定是不可靠的（实测 QML 的构造顺序
     * 和声明顺序不一致）。写 QML 的人说哪一份是主栏，就是哪一份。
     * 之后"用户点了哪一栏"会让它跟着走（见 setPaneFocus）。
     */
    Q_PROPERTY(bool mainEditor READ mainEditor WRITE setMainEditor NOTIFY boundChanged)

    /*
     * 让这一栏**取消分栏**：把它自己打开的那些标签放回池子、关掉自己。
     *
     * 文档本身不删（池子里可能还有别的栏在看），它只是不再显示它们 ——
     * 所以取消分栏之后主栏那些标签一个都不少。
     */
    Q_INVOKABLE void unbind();

    /*
     * 分栏刚开、池子里已经有一份文档时，把它拉进这一栏并显示出来。
     *
     * 这一栏在构造时是空的（池子里的东西不进新栏的标签栏），所以"启动时按
     * 上次的分栏模式开局"和"点右键菜单分栏"都要显式叫一下 —— 否则分出来的
     * 那一栏是个空壳，看着像分栏没生效。
     *
     * 传的是**文档号**（currentDocId），不是池子里的位置：池子里会有人
     * 过期（关掉的文档留着空位），位置会变，文档号不会。
     */
    Q_INVOKABLE void openPoolDocument(int docId);

    /* 池子里现在有几份文档（自检 / 诊断用） */
    Q_INVOKABLE int poolCount() const;
    /* 这一栏里有没有开着这个文档号的标签（自检用） */
    Q_INVOKABLE bool hasPoolDocument(int docId) const;
    /* 当前这一份的文档号（-1 = 没打开任何文档） */
    Q_INVOKABLE int currentDocId() const;
    /* 这份文档在第几个标签上（自检用；找不到返回 -1） */
    Q_INVOKABLE int tabIndexOfDocId(int docId) const;

    /*
     * 另一栏改了正文，把这一栏的画面重画一遍。
     *
     * 两栏看同一份文档时，底层是**同一个** Scintilla 文档，但两个视图各有
     * 自己的缓存，Scintilla 不会替另一个视图重画 —— 不叫这一下就是"在左边
     * 打字，右边那半屏还是旧的，得滚一下才刷新"。
     *
     * 只在"两边看的是同一份文档"时才有意义；不同文档时它只是白重画一次，
     * 无害（所以调用方不必先判断）。
     */
    Q_INVOKABLE void refreshSharedDocument();

    /*
     * 用户最后**在哪个栏里点了 / 打了字**（Main.qml 用它决定命令发给谁）。
     *
     * 这是个**可写属性**：QML 那边（notePaneFocus）一次只把一栏标上 true、
     * 别的栏清成 false，所以它得能被赋值。C++ 侧 setPaneFocus() 顺手把
     * "当前编辑器"（s_instance）也指过来。
     */
    Q_PROPERTY(bool paneFocus READ hasPaneFocus WRITE setPaneFocus NOTIFY paneFocusChanged)
    /*
     * "用户点的是这一栏"的**只读**版本，给 QML 的绑定用（哪个属性变化都要有
     * NOTIFY，绑定才会重算 —— Main.qml 的 activeView 就挂在这上面）。
     */
    Q_PROPERTY(bool activePane READ hasPaneFocus NOTIFY paneFocusChanged)
    /* 这个栏是不是最后被操作的那个（Main.qml 拼"当前编辑器"用） */
    bool hasPaneFocus() const { return m_paneFocus; }
    void setPaneFocus(bool on);

    /* 分栏那几个属性的读写（说明见上面那组 Q_PROPERTY） */
    int docId() const { return m_docId; }
    void setDocId(int id);
    bool mirror() const { return m_mirror; }
    void setMirror(bool on);
    bool bound() const { return m_mirror; }
    bool mainEditor() const { return m_mainEditor; }
    void setMainEditor(bool on);

signals:
    /* 分栏绑定 / 解除（界面据此决定右栏显不显示） */
    void boundChanged();
    /* 这个栏被点了（Main.qml 接住：把"当前编辑器"切到它） */
    void paneFocused();
    /*
     * 这一栏的滚动位置变了（滚轮 / 键盘 / 拖滚动条 / 程序设行号都算，见 .cpp）。
     * 文件对比页拿它做两栏同步。
     */
    void viewScrolled();
    /* 这一栏的"最后被点的是我"标记变了 */
    void paneFocusChanged();
    /*
     * 这一栏的标签状态（标题 / 修改标记 / 池子里多了一份）变了，
     * 别的栏收到就把自己的标签栏重算一遍。
     *
     * 正文不走这条 —— 两栏看的是同一份文档，改哪边都是改同一个东西；
     * 这里走的是"界面上的账目"，所以用队列连接、允许晚一拍。
     */
    void tabsChanged();

    /*
     * 正文被**用户**改了（灌正文进去的那一段不算，见 .cpp 里 m_bulkLoading 那道闸）。
     *
     * 原来外面只能听 statsChanged，那个连光标移动都发；对比页要"改一个字就
     * 重算一次差异"，得有一条只认真改动的。
     */
    void textChanged();

    void paddingChanged();
    void fontChanged();
    void colorsChanged();
    void statsChanged();
    void wrapChanged();
    void lineNumbersChanged();
    void whitespaceChanged();
    void indentGuidesChanged();
    void gutterLineChanged();
    void rulerChanged();
    void foldingChanged();
    void readOnlyChanged();
    void zoomChanged();
    void documentsChanged();
    void currentChanged();
    void modifiedChanged();
    void languageChanged();
    void encodingChanged();
    void eolChanged();
    void cursorChanged();
    void undoStateChanged();
    void errorOccurred(const QString &message);
    void saved(const QString &path);
    void fileDropped(const QString &path);

    /*
     * 编辑区里按下了右键。
     *
     * 坐标是**场景坐标**（等于 QML 窗口内容区坐标，和 DropdownMenu 的 x/y 同一套），
     * 由 Main.qml 拿它去弹 QML 那套下拉菜单（见 .cpp 的 eventFilter 说明）。
     */
    void contextMenuRequested(qreal x, qreal y);

    /*
     * 滚动条上按下了右键。
     *
     * horizontal = 哪一条（横向 / 纵向）。坐标同样是场景坐标，Main.qml 拿它
     * 弹 QML 那套下拉菜单 —— 原来这里是 Qt 自带的菜单，浅色底、英文条目
     * （"Scroll here / Left edge / …"），和界面里其它菜单完全不是一个样子。
     */
    void scrollBarContextMenuRequested(bool horizontal, qreal x, qreal y);

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

    /*
     * 拦住编辑区的右键事件，换成 QML 的下拉菜单 —— 不让 Scintilla 弹它自己
     * 那个 QtWidgets 菜单（英文、和界面风格不搭，用户报了）。见 .cpp 的说明。
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /*
     * 一条打开的文档。
     *
     * 这是**文档池里的那一份**，两个栏看到的是同一个（shared_ptr）：
     * 正文、路径、语言、编码、修改标记只有一份，"在左边改、右边立刻变"
     * 就是天然的。光标 / 滚动位置不在这里 —— 那是视图状态，每个栏各一份
     * （见下面的 DocRef）。
     */
    struct Doc {
        /*
         * 底层文档句柄。
         *
         * 用**值**而不是指针：QsciDocument 自己就是引用计数的壳子
         * （第三个 qscintilla 的 qscidocument.cpp 里 nr_attaches / nr_displays），
         * 每个栏把自己那份 DocRef.m_doc 拷一份过去，引用计数自己就涨；
         * 谁最后放手谁负责把底层文档归还给 Scintilla 的池子。
         * 不需要自己写引用计数。
         */
        QsciDocument m_doc;
        QString filePath;
        QString language = QStringLiteral("plain");
        QString encoding = QStringLiteral("UTF-8");
        /*
         * 修改标记。
         *
         * 不能直接问 QsciDocument::isModified()（那是私有的），所以自己记一份：
         * 当前文档的改动由 modificationChanged 信号同步进来，切走前留在
         * 这里，切回来时接着用。Scintilla 自己的保存点随文档走，两边一致。
         */
        bool modified = false;
        int untitledNo = 0;                // 未命名标签的序号
        /*
         * 文档号：一份文档一个，**永不变**（s_nextDocId 单调递增）。
         *
         * 两个栏靠它对"说的是哪一份"：池子里的**位置**会因为别人关掉文档
         * 而变（池子留着过期的空位），文档号不会。分栏开局、自检断言都用它。
         */
        int id = 0;
    };

    /*
     * 某个栏里的一个标签：指向池子里那份文档，外加**这个栏自己的**视图状态。
     *
     * 同一份文档在两个栏里各有一条 DocRef —— 正文是共享的，光标和滚动
     * 位置不是（两栏各看各的位置）。
     */
    struct DocRef {
        std::shared_ptr<Doc> doc;
        /*
         * 本栏显示这一份用的那层壳（QsciDocument 的引用计数靠它涨，见
         * Doc::m_doc）。shown 是"这一栏有没有挂上它"—— QsciDocument 没有
         * 公开的判空接口，所以自己记一个。
         */
        QsciDocument m_doc;
        bool shown = false;
        long cursorPos = 0;                // 光标绝对位置（切标签时恢复）
        int firstVisibleLine = 0;          // 首行（保持滚动位置）
        int xOffset = 0;                   // 横向滚动偏移
    };
    static QString languageForPath(const QString &path);

    void ensureWrapped();
    void applyGeometry();
    /*
     * 盯住所有祖先的位置 / 尺寸变化（见 .cpp 里的说明）：原生子窗口按场景坐标
     * 摆，而 item 自己的 geometryChange 感知不到"父壳整体挪位置、尺寸不变"。
     */
    void watchAncestorGeometry();

    /*
     * 把两条竖线补到编辑控件最底边（横向滚动条那一条，正文区画不到那里）。
     * 见 .cpp 里的 BottomLines。
     */
    void updateBottomLines();

    /* 标签标题（文件名 / 剪贴板标题 / 未命名 N） */
    QString titleOf(const Doc &d) const;

    /* 把正文灌进当前文档（不动文档元信息） */
    void setContentCurrent(const QString &text);

    /* ---- 文档池（两个栏共用，见上面那段说明） ---- */

    /*
     * 往池子里放一份新文档（newDocument / openFile 走它），并广播给所有栏。
     * 返回它在**这一栏**标签列表里的下标（-1 = 失败）。
     *
     * 打开文件时别的栏要跟着切过去（用户点了左树上一份文件，两栏都该显示它）；
     * 新建空白文档同理 —— 都是一条明确的"打开一个东西"的动作。
     */
    int appendPoolDocument(const std::shared_ptr<Doc> &doc);
    /* 把这一栏里下标 index 那个标签放回池子（不删文档本身） */
    void releaseDocument(int index);
    /*
     * 把池子（s_pool）对齐到这一栏的文档表上。
     *
     * 池子是"整个编辑器里开着哪些文档"的唯一真相；每个栏都留一份**同样顺序**
     * 的表，所以"这一栏开着哪几份"（m_open）和池子下标在两边是一致的。
     * 别的栏开 / 关文档时都要走一遍它。
     */
    void syncPoolFromRegistry();

    /* 把这一栏自己的标签状态（标题 / 修改标记）重发一遍 */
    void refreshTabs();

    /*
     * 让这一栏离开当前文档（视图切回 scratch），并把视图状态存下来。
     * 关标签 / 换标签 / 取消分栏都从这里过 —— 顺序错了就是踩已释放的
     * Scintilla 文档（见 releaseDocument 里那段说明）。
     */
    void detachFromCurrentDocument();

    /*
     * 视图状态（光标 / 首行 / 横向偏移）的存与取，只动这一栏自己的账 */
    void saveCurrentViewState();
    void applyStoredViewState();

    /*
     * 这一栏的标签列表（m_open）与池子下标之间的小工具。
     *
     * 对外（QML）一律用"标签下标"（0 = 最左边那条标签），跟用户看到的一致；
     * 池子下标只在内部用来对齐两边共用的那份账。
     */
    /* 某个标签对应 m_docs 里第几份；没打开返回 -1 */
    int slotOfTab(int tabIndex) const;
    /* m_docs 第 slot 份在这一栏的标签栏里是第几条；没打开返回 -1 */
    int tabOfSlot(int slot) const;
    /* 把 docId 那一份加进这一栏的标签栏并切过去；返回标签下标或 -1 */
    int openDocumentById(int docId, bool activate);
    /* 这一栏当前那个标签对应池子里的下标（-1 = 没打开任何文档） */
    int currentDocSlot() const;

    /* 取消分栏：把自己打开的标签全放回池子 */
    void releaseAllDocuments();

    /* 去掉边框、深色滚动条 */
    void styleChrome();

    /*
     * 按内容宽度决定要不要显示横向滚动条。
     *
     * Scintilla 的 scrollWidth 默认 2000，横向范围 = scrollWidth - **一页文本宽**
     * （GetTextRectangle().Width()，见 horizontalPageWidth），只要一页比 2000 窄
     * 就恒 > 0 —— 短内容也会一直挂着横条。
     * 这里自己算内容宽度：放得下就把 scrollWidth 设成一页文本宽（hMax = 0），
     * 真的超宽才设成内容宽度。
     */
    void updateHorizontalScroll();

    /*
     * Scintilla 判横条显隐用的"一页文本宽"（就是 QsciScintillaQt::ModifyScrollBars
     * 里的 hNewPage）。注意它**比 viewport 窄**：差的正是行号/折叠那几条边距
     * 和左右留白 —— 拿 viewport 宽当界，短内容也会挂横条。
     */
    long horizontalPageWidth() const;

    /* 和 QML contentArea 的 radius: 10 对齐 */
    static constexpr int kCardRadius = 10;

    void applyStyle();
    /* 字体/前景/底色写进 STYLE_DEFAULT 并刷满样式表（见 .cpp 里的说明） */
    void applyDefaultStyle();
    /*
     * 把整张样式表（0..255）的字体统一成正文字体（见 .cpp 里的说明：没设过字体的格会
     * 被按应用字体量高度，而 Scintilla 的行高取整张表最大的 ascent+descent，一格偏高
     * 就全篇每行被撑高 1px、序号和正文错开）。
     */
    void unifyStyleFonts();
    /* 文档位置 pos 落在第几条校验问题里（-1 = 没有；同一行上有问题也认） */
    int checkIssueAt(long pos) const;
    /* 一条校验问题的悬浮说明（HTML，QToolTip 用，见 .cpp） */
    QString checkIssueHtml(int index) const;
    void applyPadding();
    /*
     * 按 lineHeightFactor 把"额外上下空白"写进 Scintilla（行高就是靠它实现的）。
     * 换字号 / 换字体 / 装 lexer 之后自然行高会变，所以 applyStyle() 末尾要重算一次。
     */
    void applyLineSpacing();
    void applyViewOptions();
    /* 切主题：lexers 按语言缓存着，色板要逐个重铺（见 AppTheme::lightChanged） */
    void restyleForTheme();
    void applyLanguageLexer();
    void applyMargins();

    /*
     * 横向滚动条的左内缩（见 .cpp 的 scrollBarStyleSheet）。
     *
     * 横条原本是整条铺满编辑控件宽度的 —— 也就是从行号栏底下就开始，
     * 长行一滚滑块就爬到序号栏下面。这里把轨道按序号栏宽度右移。
     */
    QString scrollBarStyleSheet() const;
    void applyScrollBarInset();
    /* 序号栏那一条带的右边缘 = 三条边距宽之和（不含正文侧的左留白） */
    int gutterRightEdge() const;
    /* 横条占的那一行有多高（viewport 底边到控件底边）；-1 = 这一行不存在 */
    int barRowHeight() const;

    /*
     * 字数参考线（Scintilla 的 edge）。
     *
     * 独立于边距：边距管"行号右边那条分隔线"，edge 管"第 N 个字那条线"。
     * 三发消息分别是列号 / 颜色 / 模式，设置或开关一变就重发一次。
     */
    void applyRuler();

    /*
     * 边距（行号栏 / 折叠栏）的主题化。
     *
     * 必须在 applyLanguageLexer() **之后**调用：装 lexer 时 QScintilla 会先
     * SCI_STYLECLEARALL，把 STYLE_LINENUMBER 一起刷成默认样式（白底黑字），
     * 而 SC_MARGIN_NUMBER 的背景正是取 STYLE_LINENUMBER 的 paper ——
     * 这就是"行号栏变成一条白带"的根因。
     */
    void applyMarginTheme();

    /*
     * 折叠标记的形状 / 颜色 / 折叠栏宽度（细线尖括号 + 左右各 5px）。
     *
     * 必须在 setFolding() **之后**调用：QScintilla 在那里把 7 个折叠标记号配成
     * 它那套方框（BOXPLUS / VLINE…），这里换成自己画的 "›" / "⌄" 位图。
     */
    void applyFoldMarkers();
    QsciLexer *lexerFor(const QString &id);
    void themeLexer(QsciLexer *lexer);

    /* 视图级状态存文档里 / 从文档里恢复 */
    void storeViewState();
    void restoreViewState();
    void emitDocumentsState();
    void updateZoomPercent();

    /*
     * 编辑器正文用的字体（Consolas + 当前字号）。
     *
     * 字号是**像素**（设置里写 12 就是 12 像素，跟 QML 的 font.pixelSize 一致），
     * 但给 QFont 时必须落成 pointSizeF()：QScintilla 把字体交给 Scintilla 用的是
     * SCI_STYLESETSIZEFRACTIONAL(f.pointSizeF() * 100)（见 qsciscintilla.cpp 的
     * setStylesFont）。用 QFont::setPixelSize() 造字体的话 pointSizeF() 是 -1，
     * 样式字号就变成 -100 —— 一装语法高亮 lexer，整篇文字就小到看不见。
     * 所以这里做"像素 → 点"的换算（见 .cpp），两个坑都绕开。
     */
    QFont uiFont() const;

    /* 注释用的字体：正文字体 + 可选的独立字号 + 一直斜体（见 commentFontPixelSize） */
    QFont commentFont() const;

    /* 字号换算用的逻辑 DPI（真正渲染编辑区的那块屏幕） */
    qreal logicalDpiY() const;

    /* 正文编解码 */
    QString decodeBytes(const QByteArray &bytes, QString *encodingOut) const;
    QByteArray encodeText(const QString &text, const QString &encoding) const;

    /* 查找的内部实现（前向 / 后向 + 环绕） */
    long searchFrom(long from, long to, const QString &text, bool caseSensitive,
                    bool wholeWord, bool regex) const;
    long lastMatchBefore(long limit, const QString &text, bool caseSensitive,
                         bool wholeWord, bool regex) const;

    Doc *currentDoc();

    ClipboardStore *m_store = nullptr;

    /*
     * 原生控件指针必须是 QPointer。
     *
     * 编辑器控件挂在宿主 QWidget 下，宿主析构时 Qt 会先把子控件删掉，
     * 这时 ~EditorViewItem 才跑（QML 场景随 QQuickWidget 一起销毁）。
     * 裸指针在这种情况下是悬空的，后面任何一次访问都是崩溃。
     */
    QPointer<QsciScintilla> m_sci;
    /* 宿主 QWidget 与它内部的 QScintilla 子控件 */
    QPointer<QWidget> m_hostWidget;
    QPointer<QWidget> m_sciWidget;
    /* 横向滚动条那一条上的补线控件（见 .cpp 里的 BottomLines） */
    QPointer<QWidget> m_bottomLines;

    /*
     * 池子里有哪几份文档（全局的，顺序 = 打开顺序）。
     *
     * 这个表是**给"哪一份是哪一份"用的**（切标签 / 关标签 / 自检）；界面上
     * 那个标签栏用的是 m_open（这一栏开着哪几份）—— 两栏是独立的标签，
     * 池子里的一共有哪些跟"这一栏显示什么"是两件事。
     */
    QVector<DocRef> m_docs;
    /*
     * 这一栏**打开着**的标签：值是 m_docs 的下标，顺序就是标签栏从左到右。
     *
     * 分栏之后两栏各有一份 —— "像 VS Code 那样，两栏是独立的 tab"就是它。
     */
    QVector<int> m_open;
    /* m_current 是 m_open 里的位置（-1 = 这一栏没有打开任何文档） */
    int m_current = -1;
    /*
     * 未命名文档的编号计数。
     *
     * 放在**池子**这一层编号（见 appendPoolDocument）：两个栏都能开新文件，
     * 各编各的话会出现两个"未命名 1"。静态的 s_untitledCounter 就是它。
     */
    /* 没有打开任何文档时视图挂着的空文档（见 closeDocument 里的说明） */
    QsciDocument *m_scratch = nullptr;

    /* ---- 分栏（见上面那组 Q_PROPERTY 的说明） ---- */
    /* 这一栏是不是"第二栏"（只影响界面上的语气和自检怎么认，行为上两栏对等） */
    bool m_mirror = false;
    /* 这个栏是不是最后被操作的那个（Main.qml 据此挑"当前编辑器"） */
    bool m_paneFocus = false;
    /* 这一份是不是主编辑器（QML 指定；见 mainEditor 那个 Q_PROPERTY） */
    bool m_mainEditor = false;
    /*
     * 正在按池子广播调整自己的标签（加 / 删）。
     *
     * 调整过程中会连着发 documentsChanged / textChanged 一类信号，那些信号
     * 会反过来再去动池子 —— 这一圈要挡住（和以前 m_syncing 同一个作用）。
     */
    bool m_syncing = false;
    /* QML 给的文档身份号（自检和界面用它认"这是哪一栏"） */
    int m_docId = 0;

    QHash<QString, QsciLexer *> m_lexers;
    QString m_appliedLanguage;

    int m_paddingLeft = 0;
    int m_paddingTop = 0;
    int m_paddingRight = 0;
    int m_paddingBottom = 0;

    /*
     * 横条样式表里那个 margin-left（逻辑像素）= 上一次同步时的序号栏宽。
     * applyMargins 每次击键都会走，靠这个值判"有没有真的变"，不变就不重设样式表。
     */
    int m_hBarInset = 0;

    /* 最近一次量到的最长行宽（像素），自检诊断用，见 horizontalScrollState() */
    long m_lastContentWidth = 0;

    int m_fontPixelSize = 12;
    /* 正文字体家族；注释字号 0 = 跟随正文 */
    QString m_fontFamily = QStringLiteral("Consolas");
    int m_commentFontPixelSize = 0;
    /* 行高倍数：1.0 = 跟随字体（见 Q_PROPERTY 里的说明） */
    qreal m_lineHeightFactor = 1.0;
    int m_zoomPercent = 100;
    QColor m_textColor{0xd6, 0xd7, 0xda};
    QColor m_paperColor{0x1e, 0x1f, 0x22};
    QColor m_gutterColor{0x1e, 0x1f, 0x22};
    QColor m_lineNumberColor{0x60, 0x63, 0x66};

    bool m_wrap = false;
    bool m_lineNumbers = true;
    bool m_whitespace = false;
    bool m_indentGuides = true;
    /* 行号栏右侧的分隔竖线（默认开） */
    bool m_gutterLine = true;
    /* 字数参考线：默认开、"一行 120 字" */
    bool m_rulerVisible = true;
    int m_rulerColumn = 120;
    bool m_folding = true;
    bool m_readOnly = false;

    int m_lastLoadMs = -1;
    int m_lastLoadChars = 0;

    /*
     * 滚动条右键菜单的两个小状态（见 .cpp 的 scrollBarAction）。
     *
     * m_scrollMenuValue 是"右键点住的那个位置"换算出来的滚动值 —— 菜单里
     * "滚动到这里"就是它（和 Qt 自带菜单一个口径：按点击位置在滚动条上的比例）。
     * m_scrollMenuHandled 给自检看"这个右键事件有没有被我们吃掉"。
     */
    int m_scrollMenuValue = 0;
    bool m_scrollMenuHandled = false;
    bool m_hasContent = false;

    QString m_lastError;

    /* 查找结果高亮用的指示器编号（INDIC_CONTAINER 起，避开 lexer 自己的） */
    static constexpr int kFindIndicator = 8;
    /* 校验结果的波浪线：9 = 错误、10 = 警告（理由同上，8 已经被查找占了） */
    static constexpr int kCheckErrorIndicator = 9;
    static constexpr int kCheckWarnIndicator = 10;
    /*
     * 文件对比那八条指示器：11 / 12 / 13 = 删 / 增 / 改 的整行底色，
     * 14 = 行内字级差异，15 / 16 / 17 = 当前那一处的删 / 增 / 改（更亮一档）。
     *
     * 三种底色要占三个编号是因为 Scintilla 的颜色挂在**指示器编号**上
     * （SCI_INDICSETFORE 按编号设，不按区间），一档一个才分得开。
     * 编号从 11 起：8 被查找占了、9 / 10 被校验波浪线占了。
     * Scintilla 按编号从小到大画，所以"当前"压在整行底色上、字级压在两者之间。
     *
     * 为什么不是 SC_MARK_BACKGROUND 标记（那个理论上能铺满整行）：
     * 见 .cpp 里 beginDiff 那段实测记录。
     *
     * "当前这一处"为什么是"更亮一档的底色"而不是一个描边框：
     * 试过 INDIC_STRAIGHTBOX 只描边（fillAlpha=0 / outlineAlpha=255），
     * 实测**下边框看不见** —— 那个矩形的底边正好落在行界上
     * （Indicator.cpp 里 rcBox.bottom = rcLine.bottom，只有 top 往里收一格），
     * 而指示器画在文字之前，下一行铺自己那行背景时正好把那条线糊掉。
     */
    static constexpr int kDiffDelIndicator = 11;
    static constexpr int kDiffAddIndicator = 12;
    static constexpr int kDiffModIndicator = 13;
    static constexpr int kDiffWordIndicator = 14;
    static constexpr int kDiffCurDelIndicator = 15;
    static constexpr int kDiffCurAddIndicator = 16;
    static constexpr int kDiffCurModIndicator = 17;
    /*
     * 空白带（行注释）用的那个扩展样式号：向 Scintilla 要一个 256 起的
     * 扩展样式，不撞 lexer 的样式表。-1 = 还没要过（beginDiff 里要）。
     */
    int m_diffGapStyle = -1;

    /* 本行那一段的字节区间 [行首, 下一行行首)：含换行符，字级框才画得满 */
    QPair<long, long> diffLineRange(int line) const;
    /* 抹掉画在文档上的那一层（标记 / 指示器 / 行注释），不动视图设置 */
    void clearDiffPaint();
    /*
     * 上一次校验推过来的问题（悬浮时按它拼说明框），以及每一处在文档里的
     * **字节**区间（悬浮定位用）—— 两个表一一对应，clearCheckIssues 一起清。
     */
    QVariantList m_checkIssues;
    QList<QPair<long, long>> m_checkRanges;
    /* 样式是否已在正确几何下应用过（见 applyGeometry 里的说明） */
    bool m_chromeApplied = false;
    /* 正在切文档 / 灌正文：这期间的 modified 通知不算"用户改动" */
    bool m_bulkLoading = false;

    /* 已经接上位置/尺寸信号的祖先（见 watchAncestorGeometry） */
    QVector<QPointer<QQuickItem>> m_watchedAncestors;

    static ClipboardStore *s_store;
    static QWidget *s_hostWidget;
    static EditorViewItem *s_instance;
    /* 所有活着的实例（syncAllGeometry 要挨个摆，见那边的说明） */
    static QVector<QPointer<EditorViewItem>> s_all;
    /*
     * 文档池：所有栏打开着的文档，按打开顺序排。
     *
     * weak_ptr 是故意的：文档什么时候删由 shared_ptr 的引用计数说了算，
     * 这里只是"有哪几份、什么顺序"的账本（新开一栏时按它对一遍池子）。
     * 过期的那几条（没人持有了）在 poolCount 里顺手清掉。
     */
    static QVector<std::weak_ptr<Doc>> s_pool;
    /* 未命名文档的编号（池子级的，见 m_docs 上面那段） */
    static int s_untitledCounter;
    /* 文档号发号器（一份文档一个，永不变 —— 见 Doc::id） */
    static int s_nextDocId;
};
