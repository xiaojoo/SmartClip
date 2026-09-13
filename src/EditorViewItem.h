#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QVariantList>
#include <QVector>

class QWidget;
class QsciScintilla;
class QsciLexer;
class QsciDocument;

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
    Q_PROPERTY(int zoomPercent READ zoomPercent NOTIFY zoomChanged)

    /* ---- 文档 / 标签 ---- */
    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY documentsChanged)
    Q_PROPERTY(QVariantList documents READ documents NOTIFY documentsChanged)
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

    bool hasDocument() const { return m_current >= 0 && m_current < m_docs.size(); }
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
     * 自检用：正文区（不含行号栏/折叠栏）里"纯白"像素的个数。
     *
     * 专门查"底色被刷成白色"这类问题：正文色是 #d6d7da、底色是 #1e1f22，两者
     * 混出来的像素都在 250 以下，所以只要出现接近 #ffffff 的像素，就说明有一块
     * 白底（用户报的"切到纯文本，字体全带上白色背景"就是这么发现的）。
     */
    Q_INVOKABLE int whiteBackgroundPixels() const;

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

signals:
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
    /* 一条打开的文档（正文在 Scintilla 那边，这里放元信息 + 视图位置） */
    struct Doc {
        QsciDocument *document = nullptr;  // 见 .cpp 里为什么用指针
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
        long cursorPos = 0;                // 光标绝对位置（切标签时恢复）
        int firstVisibleLine = 0;           // 首行（保持滚动位置）
        int xOffset = 0;                    // 横向滚动偏移
    };

    static QString languageForPath(const QString &path);

    void ensureWrapped();
    void applyGeometry();

    /*
     * 把两条竖线补到编辑控件最底边（横向滚动条那一条，正文区画不到那里）。
     * 见 .cpp 里的 BottomLines。
     */
    void updateBottomLines();

    /* 标签标题（文件名 / 剪贴板标题 / 未命名 N） */
    QString titleOf(const Doc &d) const;

    /* 把正文灌进当前文档（不动文档元信息） */
    void setContentCurrent(const QString &text);

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
    void applyPadding();
    /*
     * 按 lineHeightFactor 把"额外上下空白"写进 Scintilla（行高就是靠它实现的）。
     * 换字号 / 换字体 / 装 lexer 之后自然行高会变，所以 applyStyle() 末尾要重算一次。
     */
    void applyLineSpacing();
    void applyViewOptions();
    void applyLanguageLexer();
    void applyMargins();

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

    QVector<Doc> m_docs;
    int m_current = -1;
    int m_untitledCounter = 0;
    /* 没有打开任何文档时视图挂着的空文档（见 closeDocument 里的说明） */
    QsciDocument *m_scratch = nullptr;

    QHash<QString, QsciLexer *> m_lexers;
    QString m_appliedLanguage;

    int m_paddingLeft = 0;
    int m_paddingTop = 0;
    int m_paddingRight = 0;
    int m_paddingBottom = 0;

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
    /* 样式是否已在正确几何下应用过（见 applyGeometry 里的说明） */
    bool m_chromeApplied = false;
    /* 正在切文档 / 灌正文：这期间的 modified 通知不算"用户改动" */
    bool m_bulkLoading = false;

    static ClipboardStore *s_store;
    static QWidget *s_hostWidget;
    static EditorViewItem *s_instance;
};
