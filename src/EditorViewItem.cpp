#include "EditorViewItem.h"

#include "ClipboardStore.h"

#include <QAction>
#include <QClipboard>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMenu>
#include <QPair>
#include <QHash>
#include <QPalette>
#include <QPrintDialog>
#include <QPrinter>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QScrollBar>
#include <QStringConverter>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <Qsci/qscilexer.h>
#include <Qsci/qscilexerbash.h>
#include <Qsci/qscilexerbatch.h>
#include <Qsci/qscilexercmake.h>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexercsharp.h>
#include <Qsci/qscilexercss.h>
#include <Qsci/qscilexerdiff.h>
#include <Qsci/qscilexerfortran.h>
#include <Qsci/qscilexerhtml.h>
#include <Qsci/qscilexerjava.h>
#include <Qsci/qscilexerjavascript.h>
#include <Qsci/qscilexerjson.h>
#include <Qsci/qscilexerlua.h>
#include <Qsci/qscilexermakefile.h>
#include <Qsci/qscilexermarkdown.h>
#include <Qsci/qscilexernasm.h>
#include <Qsci/qscilexerpascal.h>
#include <Qsci/qscilexerperl.h>
#include <Qsci/qscilexerproperties.h>
#include <Qsci/qscilexerpython.h>
#include <Qsci/qscilexerruby.h>
#include <Qsci/qscilexersql.h>
#include <Qsci/qscilexertex.h>
#include <Qsci/qscilexerverilog.h>
#include <Qsci/qscilexervhdl.h>
#include <Qsci/qscilexeryaml.h>
#include <Qsci/qscidocument.h>
#include <Qsci/qsciprinter.h>
#include <Qsci/qsciscintilla.h>

#include <algorithm>

/*
 * 把 QScintilla 包成 QML 可用的 Item。
 *
 * QScintilla 是 widget，QML 场景图里放不下，所以用
 * QWidget::createWindowContainer() 得到一个独立的原生子窗口，
 * 再把它的 contentItem 挂到主 QQuickWindow 上。
 *
 * 为什么不用 QQuickWidget：
 *   它会开自己的 RHI 渲染上下文并与主场景图嵌套；我们的主窗口是
 *   "无边框 + 透明 + 圆角遮罩"，这种情况下 QQuickWidget 容易出白底或圆角丢失。
 *   原生子窗口只参与合成、不进场景图，行为更可预期。
 *   代价：它盖在 QML 内容之上，不受 QML 的变换/裁剪影响。
 *
 * 这条"原生子窗口永远盖在 QML 之上"的性质决定了界面结构：
 * 查找栏不能浮在编辑区上面，只能占一条独立的布局条（见 FindBar.qml）。
 */
ClipboardStore *EditorViewItem::s_store = nullptr;
QWidget *EditorViewItem::s_hostWidget = nullptr;
EditorViewItem *EditorViewItem::s_instance = nullptr;

namespace {

constexpr long kScEolCrLf = 0;
constexpr long kScEolCr = 1;
constexpr long kScEolLf = 2;

constexpr long kScWsVisibleAlways = 1;
constexpr long kScIvLookBoth = 3;

/*
 * Scintilla 的搜索标志和指示器常量。
 *
 * Qsci 的头文件只转出 SCI_* 的枚举，SCFIND_* / INDIC_* 这几个宏它不带出来；
 * 而直接 #include <Scintilla.h> 会把这些宏定义成同名标识符，
 * 之后代码里所有 QsciScintillaBase::SCI_XXX 都会被宏替换成
 * QsciScintillaBase::2240 这种样子，整个文件编译不过。
 * 所以这里按 Scintilla.h 的值自己写一份（值取自 Scintilla 5.x）。
 */
constexpr long kScFindWholeWord = 0x2;
constexpr long kScFindMatchCase = 0x4;
constexpr long kScFindRegexp = 0x00200000;
constexpr long kIndicRoundBox = 7;

constexpr int kMaxFileBytes = 64 * 1024 * 1024;

/*
 * Scintilla 的颜色是 **0x00BBGGRR（BGR 打包）**，而 Qt 的 QColor::rgb() 是
 * 0xRRGGBB。把 rgb() 原样交给 SCI_* 消息会红蓝对调 —— 实测把强调蓝 #4c96d8
 * 传给边距标记，画出来是橙色 #d8964c。凡是要传裸整数的消息都过一遍这里。
 *
 * （QScintilla 自己的 QColor 重载会正确打包，所以凡是它能收 QColor 的地方
 *   尽量走 QColor 重载；这里是给只能传整数的消息用的。）
 */
inline long scColor(const QColor &c) {
    return long((c.blue() << 16) | (c.green() << 8) | c.red());
}

/* 主题色（用的时候过 scColor 打包） */
const QColor kAccent(0x4c, 0x96, 0xd8);         // 强调蓝
const QColor kCaretLineBack(0x26, 0x28, 0x2b);  // 当前行底色
const QColor kSelectionBack(0x2f, 0x65, 0x9c);  // 选中底色
/* 深色主题的语法配色（JetBrains 暗色系） */
QColor paletteDefault() { return QColor(0xd6, 0xd7, 0xda); }
QColor paletteKeyword() { return QColor(0xcf, 0x8e, 0x6d); }
QColor paletteString() { return QColor(0x6a, 0xab, 0x73); }
QColor paletteNumber() { return QColor(0x2a, 0xac, 0xb8); }
QColor paletteComment() { return QColor(0x7a, 0x7e, 0x85); }
QColor paletteFunction() { return QColor(0x56, 0xa8, 0xf5); }
QColor paletteType() { return QColor(0xc7, 0x7d, 0xbb); }
QColor paletteOperator() { return QColor(0xbc, 0xbe, 0xc4); }
QColor palettePreproc() { return QColor(0xb3, 0xae, 0x60); }
QColor paletteTag() { return QColor(0xe8, 0xbf, 0x6a); }
QColor paletteError() { return QColor(0xff, 0x6b, 0x68); }

/*
 * 把 lexer 的样式描述映射到深色配色。
 *
 * 为什么按"描述"而不是按样式号：每个 lexer 的样式号完全不一样
 * （C++ 的注释是 1，Python 的是 1/2，Markdown 的 3/4 又是别的），
 * 但 QsciLexer::description() 给的文本是统一的（"Comment"、"Keyword"、
 * "Double-quoted string"…），照它分类一套色板就能覆盖全部语言。
 *
 * 匹配顺序有讲究：先判 Comment，否则 "Comment keyword" 会被当成关键字。
 */
QColor themeColorFor(const QString &description) {
    const QString d = description.toLower();

    if (d.contains(QStringLiteral("comment")))
        return paletteComment();
    if (d.contains(QStringLiteral("keyword")))
        return paletteKeyword();
    if (d.contains(QStringLiteral("number")) || d.contains(QStringLiteral("numeric")))
        return paletteNumber();
    if (d.contains(QStringLiteral("string")) || d.contains(QStringLiteral("char"))
        || d.contains(QStringLiteral("literal")))
        return paletteString();
    if (d.contains(QStringLiteral("preprocessor")) || d.contains(QStringLiteral("directive")))
        return palettePreproc();
    if (d.contains(QStringLiteral("operator")))
        return paletteOperator();
    if (d.contains(QStringLiteral("function")) || d.contains(QStringLiteral("method")))
        return paletteFunction();
    if (d.contains(QStringLiteral("class")) || d.contains(QStringLiteral("type"))
        || d.contains(QStringLiteral("struct")) || d.contains(QStringLiteral("namespace"))
        || d.contains(QStringLiteral("union")) || d.contains(QStringLiteral("enum")))
        return paletteType();
    if (d.contains(QStringLiteral("tag")) || d.contains(QStringLiteral("element")))
        return paletteTag();
    if (d.contains(QStringLiteral("attribute")) || d.contains(QStringLiteral("property")))
        return QColor(0xba, 0xba, 0xba);
    if (d.contains(QStringLiteral("error")))
        return paletteError();

    /* 认不出来的样式一律给正文色，绝不放任 lexer 用自己那套浅色主题 */
    return paletteDefault();
}

bool isCommentDescription(const QString &description) {
    return description.toLower().contains(QStringLiteral("comment"));
}

bool isKeywordDescription(const QString &description) {
    return description.toLower().contains(QStringLiteral("keyword"));
}

/* 语言表：id 与显示名。id 同时是 lexer 的键。 */
struct LanguageInfo {
    const char *id;
    const char *label;
};

const LanguageInfo kLanguages[] = {
    {"plain", "纯文本"},
    {"cpp", "C / C++"},
    {"csharp", "C#"},
    {"java", "Java"},
    {"python", "Python"},
    {"javascript", "JavaScript"},
    {"json", "JSON"},
    {"html", "HTML / XML"},
    {"css", "CSS"},
    {"markdown", "Markdown"},
    {"sql", "SQL"},
    {"bash", "Shell 脚本"},
    {"batch", "Windows 批处理"},
    {"yaml", "YAML"},
    {"cmake", "CMake"},
    {"makefile", "Makefile"},
    {"properties", "INI / Properties"},
    {"diff", "Diff / Patch"},
    {"tex", "LaTeX"},
    {"perl", "Perl"},
    {"ruby", "Ruby"},
    {"lua", "Lua"},
    {"verilog", "Verilog"},
    {"vhdl", "VHDL"},
    {"asm", "汇编"},
    {"fortran", "Fortran"},
    {"pascal", "Pascal"},
};

}  // namespace

EditorViewItem::EditorViewItem(QQuickItem *parent) : QQuickItem(parent) {
    /* 本 Item 自己什么都不画，内容是原生子窗口；但需要有尺寸所以铺满父级 */
    setFlag(ItemHasContents, false);

    m_store = s_store;
    s_instance = this;
}

EditorViewItem::~EditorViewItem() {
    if (s_instance == this)
        s_instance = nullptr;
    detach();
}

/* ------------------------------------------------------------------ */
/* 原生控件生命周期                                                    */
/* ------------------------------------------------------------------ */

void EditorViewItem::ensureWrapped() {
    if (m_sci)
        return;

    /*
     * 宿主必须是 QWidget —— 否则 QScintilla 只能当独立顶层窗口。
     * 这正是 main.cpp 把主窗口从 QQuickWindow 换成 QWidget 的原因。
     */
    m_hostWidget = s_hostWidget;
    if (!m_hostWidget) {
        qWarning("EditorViewItem: 宿主 QWidget 未设置（见 setGlobalHostWidget）");
        return;
    }

    m_sciWidget = new QWidget(m_hostWidget);
    m_sciWidget->setAutoFillBackground(false);

    m_sci = new QsciScintilla(m_sciWidget);

    /* 可编辑 + 不换行 + UTF-8 */
    m_sci->setReadOnly(false);
    m_sci->setWrapMode(QsciScintilla::WrapNone);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETCODEPAGE, 65001);

    /* 行号栏：边距 0，宽度按位数留（applyMargins 里算） */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINTYPEN, 0,
                         QsciScintillaBase::SC_MARGIN_NUMBER);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINWIDTHN, 0, 56);

    /*
     * 空文档。
     *
     * Scintilla 里视图必须挂着一份文档，而"一个标签都没开"是有意义的状态
     * （显示欢迎页）。所以留一份 scratch 文档当占位；关掉最后一个标签时
     * 视图切回它，被关掉的那份文档才能真正释放（见 closeDocument）。
     *
     * 注意要在 applyStyle() 之前挂上：Scintilla 的样式表是按文档存的，
     * 后挂的话样式全落在前一份文档上，scratch 还是默认的白底黑字。
     */
    m_scratch = new QsciDocument();
    m_sci->setDocument(*m_scratch);

    applyStyle();
    applyViewOptions();
    applyPadding();

    m_sci->SendScintilla(QsciScintillaBase::SCI_SETREADONLY, 1L);

    /*
     * 状态同步。
     *
     * 改动标记只认"用户改动"：灌正文（setText）期间会连着发好几次
     * modificationChanged，用 m_bulkLoading 挡掉，否则刚打开的文件
     * 一进来就是"已修改"。
     */
    connect(m_sci, &QsciScintilla::modificationChanged, this, [this](bool m) {
        if (m_bulkLoading)
            return;
        if (Doc *d = currentDoc()) {
            if (d->modified == m)
                return;
            d->modified = m;
        }
        emit modifiedChanged();
        emit documentsChanged();
        emit undoStateChanged();
    });

    connect(m_sci, &QsciScintilla::textChanged, this, [this]() {
        if (m_bulkLoading)
            return;
        applyMargins();
        emit statsChanged();
    });

    connect(m_sci, &QsciScintilla::linesChanged, this, [this]() {
        applyMargins();
        emit statsChanged();
    });

    connect(m_sci, &QsciScintilla::cursorPositionChanged, this, [this](int, int) {
        if (Doc *d = currentDoc())
            d->cursorPos = long(m_sci->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS));
        emit cursorChanged();
    });

    connect(m_sci, &QsciScintilla::selectionChanged, this, [this]() {
        emit cursorChanged();
        emit undoStateChanged();
    });

    /*
     * 先保持隐藏。
     *
     * m_sciWidget 是宿主 QWidget 的原生子控件，show() 之后不受 QML 侧
     * visible 约束（那个只管 QQuickItem）。所以启动、空白页、选中图片等
     * 情况下它会一直显示成一块默认 640x480 的深色方块，并把窗口左上角
     * 的圆角盖成直角。显示与否统一交给 applyGeometry() 按 isVisible() 判断。
     */
    m_sciWidget->hide();
}

void EditorViewItem::detach() {
    /*
     * 顺序很重要，这里踩过两个坑，改之前先看：
     *
     * 坑一：QsciDocument 是引用计数的壳子，视图那边也挂着一份。它的析构在
     *   "自己这份是最后一个引用、且视图也不再显示它" 时会去找
     *   QsciScintillaBase::pool() 归还文档 —— 而 pool() 的实现是
     *   `return poolList.first();`（没判空，虽然调用方写了 if (qsb)）。
     *   视图已经删掉之后再析构文档句柄，poolList 正好是空的，
     *   Debug 构建直接在 qlist.h 里断言崩掉
     *   （实测 ASSERT: "!isEmpty()" in qlist.h line 715）。
     *   所以趁 QScintilla 还活着先把文档句柄删干净：
     *   先 setDocument 到一份临时文档，让我们持有的文档全部脱离视图。
     *
     * 坑二：反过来的情形也存在 —— 宿主 QWidget 析构时 Qt 先删子控件，
     *   那时脚本才轮到 ~EditorViewItem。这时 m_sci 已经没了（QPointer 为
     *   空），文档句柄的析构同样会走进空的 pool()。既然底层文档已经随
     *   控件销毁，这些壳子只能直接丢掉（进程退出前的一次性泄漏，
     *   换的是不崩）。
     *
     * lexer 的父对象就是 m_sci，随它一起销毁，所以这里只清表、不删对象。
     */
    if (m_sci) {
        m_sci->setDocument(QsciDocument());

        for (Doc &d : m_docs)
            delete d.document;

        delete m_scratch;
    }

    m_docs.clear();
    m_current = -1;
    m_scratch = nullptr;

    m_lexers.clear();
    m_appliedLanguage.clear();

    if (m_sciWidget) {
        m_sciWidget->hide();
        delete m_sciWidget;
        m_sciWidget = nullptr;
    }
    m_sci = nullptr;
}

/* ------------------------------------------------------------------ */
/* 样式                                                                */
/* ------------------------------------------------------------------ */

qreal EditorViewItem::logicalDpiY() const {
    /*
     * 换算要用**真正渲染这个编辑区的屏幕**的 DPI。
     *
     * 优先问原生控件（它最清楚自己在哪块屏上），拿不到再退回宿主、主屏。
     * 100% 缩放的 Windows 上是 96。
     */
    if (m_sciWidget)
        return m_sciWidget->logicalDpiY();
    if (s_hostWidget)
        return s_hostWidget->logicalDpiY();
    if (const QScreen *screen = QGuiApplication::primaryScreen())
        return screen->logicalDotsPerInch();
    return 96.0;
}

QFont EditorViewItem::uiFont() const {
    /*
     * 字号是**像素**（设置菜单里写的 12 就是 12 像素，和 QML 那侧 font.pixelSize
     * 一个单位），但落到 QFont 上必须用 pointSizeF() 给：
     *
     * QScintilla 把字体交给 Scintilla 时走的是 SCI_STYLESETSIZEFRACTIONAL(
     * f.pointSizeF() * 100)。用 setPixelSize() 造的字体 pointSizeF() == -1，
     * 装 lexer 时样式字号会被设成 -100 —— 表现就是"一切换语言，正文小到看不见"。
     * 所以这里自己做 像素→点 的换算，两个目的都达到：
     *   * pointSizeF() 是正常值（12px 在 96 DPI 下是 9.0），不会踩上面那个坑；
     *   * 渲染出来正好是设置的像素数（12px），跟界面上别处的 12 一般大。
     * 之前直接 setPointSize(12) 是把 12 当成"点"，96 DPI 下等于 16 像素 ——
     * "设置写着 12、画出来比界面大一圈"就是这么来的。
     */
    QFont font(m_fontFamily);
    font.setPointSizeF(double(m_fontPixelSize) * 72.0 / logicalDpiY());
    return font;
}

QFont EditorViewItem::commentFont() const {
    /*
     * 注释样式单独一套字体：家族跟正文一样，字号可以单独设（0 = 跟随正文），
     * 并且一直斜体 —— 主流编辑器都是这么区分注释的。
     */
    QFont font = uiFont();
    if (m_commentFontPixelSize > 0)
        font.setPointSizeF(double(m_commentFontPixelSize) * 72.0 / logicalDpiY());
    font.setItalic(true);
    return font;
}

/* Scintilla 样式字号用的"点 × 100"（SCI_STYLESET/GETSIZEFRACTIONAL 的单位） */
int EditorViewItem::stylePointSize() const {
    return qRound(uiFont().pointSizeF() * 100.0);
}

/*
 * 把字体 / 前景 / 底色写进 STYLE_DEFAULT，再 SCI_STYLECLEARALL 刷到所有样式号。
 *
 * 只调 setFont()/setColor() 不够 —— 实测那样会留下一堆未设置的样式号，正文用
 * 极细的默认字体画出来，看起来就是一团细线。
 */
void EditorViewItem::applyDefaultStyle() {
    if (!m_sci)
        return;

    QFont mono = uiFont();
    m_sci->setFont(mono);

    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETFONT,
                         QsciScintillaBase::STYLE_DEFAULT,
                         mono.family().toUtf8().constData());
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETSIZEFRACTIONAL,
                         QsciScintillaBase::STYLE_DEFAULT,
                         static_cast<long>(stylePointSize()));
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETFORE,
                         QsciScintillaBase::STYLE_DEFAULT,
                         scColor(m_textColor));
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETBACK,
                         QsciScintillaBase::STYLE_DEFAULT,
                         scColor(m_paperColor));
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLECLEARALL);

    m_sci->setPaper(m_paperColor);
    m_sci->setColor(m_textColor);
}

void EditorViewItem::applyStyle() {
    if (!m_sci)
        return;

    applyDefaultStyle();

    /*
     * 语法高亮必须在 STYLECLEARALL 之后重新装一次。
     *
     * 样式表是**按文档**存的，STYLECLEARALL 会把 lexer 刷进去的颜色一起抹掉；
     * 换文档时也一样（新文档没有样式）。所以这里无条件重装，别做"已经装过"
     * 的短路。
     *
     * 注意顺序：它内部的 detachLexer()/setLexer() 还会再来一次 STYLECLEARALL，
     * 所以行号栏 / 折叠栏的颜色必须放在它**后面**重刷（见 applyMarginTheme）。
     */
    applyLanguageLexer();

    /*
     * 纯文本（没装 lexer）要再压一次底色，不然会整篇白底。
     *
     * 根因：切换语言走的是 QsciScintilla::setLexer(nullptr) → detachLexer()，
     * 里面先 SCI_STYLERESETDEFAULT（把 STYLE_DEFAULT 打回 Scintilla 内置的
     * 白底黑字），紧接着 SCI_STYLECLEARALL 把这套刷满所有样式号。而正文每个
     * 字符身上还留着上一个 lexer 打下的样式号（5、17 这种），没有 lexer 就
     * 没人重新着色 —— 于是那些字符就拿白底样式画出来了（用户报的"一选 txt
     * 字体全带上白色背景"）。实测：切到纯文本后 styleBack(5)=#ffffff。
     *
     * 两条一起做：CLEARDOCUMENTSTYLE 把字符上的样式号清回 0，再把默认样式
     * （深底）刷满样式表。
     *
     * 有 lexer 时绝不能做：STYLECLEARALL 会把 lexer 刚刷好的颜色一起抹掉。
     */
    if (!m_sci->lexer()) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_CLEARDOCUMENTSTYLE);
        applyDefaultStyle();
    }

    applyMargins();

    styleChrome();
}

void EditorViewItem::applyMarginTheme() {
    if (!m_sci)
        return;

    const long paper = scColor(m_paperColor);
    const long numberFore = scColor(m_lineNumberColor);

    /*
     * 行号栏那条白带的根因就在这里。
     *
     * SC_MARGIN_NUMBER 的背景不是 SCI_SETMARGINBACKN，而是直接取
     * STYLE_LINENUMBER 的 paper（见 Scintilla 的 MarginView.cpp）；
     * 而装 lexer 时 QScintilla 的 detachLexer() 会 SCI_STYLECLEARALL，
     * 把 STYLE_LINENUMBER 一起刷成"默认样式"（白底黑字）。
     * 所以每次装完 lexer 都要把行号样式重刷一遍。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETBACK,
                         QsciScintillaBase::STYLE_LINENUMBER, paper);
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETFORE,
                         QsciScintillaBase::STYLE_LINENUMBER, numberFore);
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETFONT,
                         QsciScintillaBase::STYLE_LINENUMBER,
                         uiFont().family().toUtf8().constData());
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETSIZEFRACTIONAL,
                         QsciScintillaBase::STYLE_LINENUMBER,
                         static_cast<long>(stylePointSize()));

    /*
     * 每个边距自己的背景色也一起压成底色。
     *
     * Scintilla 里边距背景的默认值是**白色**（0xffffff），不设的话
     * 符号 / 折叠边距就是一条白带。
     */
    for (long margin = 0; margin <= 2; ++margin)
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINBACKN, margin, paper);

    /* 折叠边距自己有颜色设置（0 = 跟随默认），改成跟底色一致 */
    m_sci->setFoldMarginColors(m_paperColor, m_paperColor);

    /*
     * 缩进参考线的颜色。
     *
     * 它走的是 STYLE_INDENTGUIDE(37) 的前景色，而 STYLECLEARALL 会把它刷成
     * 正文色（#d6d7da）—— 那样竖线亮得跟正文一样抢眼。这里压成一个比底色亮
     * 一点点的灰，正好是"看得出层级、又不打扰读代码"的强度。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETFORE,
                         QsciScintillaBase::STYLE_INDENTGUIDE,
                         scColor(QColor(0x3e, 0x42, 0x47)));
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETBACK,
                         QsciScintillaBase::STYLE_INDENTGUIDE, paper);

    /*
     * 这里原来还配了"当前行行号高亮"的样式号和 marginStyleOffset。去掉的原因
     * 见 applyMargins()：数字边距根本不看逐行样式；换成文本边距那条路也试过，
     * 会让 QML 侧认不到编辑区对象，已一并回退。
     */
}

void EditorViewItem::themeFoldMarkers() {
    if (!m_sci)
        return;

    const long paper = scColor(m_paperColor);
    const long line = scColor(QColor(0x5c, 0x60, 0x66));   // 树线（竖线 / 拐角）
    const long box = scColor(QColor(0x9a, 0xa0, 0xa8));    // 折叠方块与 +/- 号

    /* QScintilla 默认把标记画成白底黑框，深色主题下很刺眼，这里按主题重设 */
    const int treeMarks[] = {QsciScintillaBase::SC_MARKNUM_FOLDERSUB,
                             QsciScintillaBase::SC_MARKNUM_FOLDERTAIL,
                             QsciScintillaBase::SC_MARKNUM_FOLDERMIDTAIL};
    for (int m : treeMarks) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_MARKERSETFORE, long(m), line);
        m_sci->SendScintilla(QsciScintillaBase::SCI_MARKERSETBACK, long(m), paper);
    }

    const int boxMarks[] = {QsciScintillaBase::SC_MARKNUM_FOLDER,
                            QsciScintillaBase::SC_MARKNUM_FOLDEROPEN,
                            QsciScintillaBase::SC_MARKNUM_FOLDEREND,
                            QsciScintillaBase::SC_MARKNUM_FOLDEROPENMID};
    for (int m : boxMarks) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_MARKERSETBACK, long(m), box);
        m_sci->SendScintilla(QsciScintillaBase::SCI_MARKERSETFORE, long(m), m_paperColor);
        /* 悬停 / 按下时用强调色，鼠标扫过时能看出可以点 */
        m_sci->SendScintilla(QsciScintillaBase::SCI_MARKERSETBACKSELECTED, long(m),
                             0x4c96d8);
    }
}

/*
 * 说明：这里原来有个"当前行行号高亮 / 左边蓝竖条"的实现，已整体去掉。
 * 查证与实测结果都记在 applyMargins() 里，简单说：
 *   * 数字边距的号码写死用 vs.styles[STYLE_LINENUMBER] 画（MarginView.cpp），
 *     边距背景整列填一次、不逐行 —— 在这个边距里做不出"当前行行号变色"；
 *   * 换成文本边距（SC_MARGIN_RTEXT + SCI_MARGINSETTEXT/SETSTYLE）做到了，
 *     但一写这套逐行边距数据，QML 侧就认不到编辑区对象了，只能回退。
 * 当前行的提示现在只剩正文那层底色（#26282b）+ 光标本身，和主流编辑器一致。
 */

void EditorViewItem::setFoldingEnabled(bool on) {
    if (m_folding == on)
        return;
    m_folding = on;
    applyMargins();
    emit foldingChanged();
}

void EditorViewItem::foldAll() {
    if (!m_sci || !hasDocument())
        return;
    /*
     * 先把整篇重新着色一遍：折叠层级是 lexer 在着色时算出来的，
     * 只加载不滚动的话后半篇可能还没算，折叠会漏掉。
     *
     * 折叠动作直接用 Scintilla 的 SCI_FOLDALL：QScintilla 2.14 只有 foldAll()
     * 没有 unfoldAll()，用同一个消息给 CONTRACT / EXPAND 两种动作最省事。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_COLOURISE, 0L, -1L);
    m_sci->SendScintilla(QsciScintillaBase::SCI_FOLDALL,
                         long(QsciScintillaBase::SC_FOLDACTION_CONTRACT));
}

void EditorViewItem::unfoldAll() {
    if (!m_sci || !hasDocument())
        return;
    m_sci->SendScintilla(QsciScintillaBase::SCI_COLOURISE, 0L, -1L);
    m_sci->SendScintilla(QsciScintillaBase::SCI_FOLDALL,
                         long(QsciScintillaBase::SC_FOLDACTION_EXPAND));
}

bool EditorViewItem::lineVisible(int line) const {
    if (!m_sci || line < 1)
        return false;
    return m_sci->SendScintilla(QsciScintillaBase::SCI_GETLINEVISIBLE,
                                long(line) - 1) != 0;
}

int EditorViewItem::marginWidth(int margin) const {
    if (!m_sci)
        return -1;
    return int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINWIDTHN,
                                    long(margin)));
}

int EditorViewItem::marginBack(int margin) const {
    if (!m_sci)
        return -1;
    return int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINBACKN,
                                    long(margin)));
}



int EditorViewItem::styleBack(int style) const {
    if (!m_sci)
        return -1;
    return int(m_sci->SendScintilla(QsciScintillaBase::SCI_STYLEGETBACK, long(style)));
}

int EditorViewItem::styleFore(int style) const {
    if (!m_sci)
        return -1;
    return int(m_sci->SendScintilla(QsciScintillaBase::SCI_STYLEGETFORE, long(style)));
}

QVariantList EditorViewItem::marginPixelStats() const {
    QVariantList out{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (!m_sci || !m_sciWidget || !hasDocument())
        return out;

    /* 诊断：当前行的标记位、第 1 列的掩码（查"竖条画不出来"用） */
    const long cur = m_sci->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION,
                                          long(m_sci->SendScintilla(
                                              QsciScintillaBase::SCI_GETCURRENTPOS)));
    out[4] = int(m_sci->SendScintilla(QsciScintillaBase::SCI_MARKERGET, cur));
    out[5] = int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINMASKN, 1L));

    const QImage img = m_sciWidget->grab().toImage();
    if (img.isNull())
        return out;

    const qreal scale =
        m_sciWidget->width() > 0 ? qreal(img.width()) / qreal(m_sciWidget->width()) : 1.0;

    const int mw0 = int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINWIDTHN, 0L) * scale);
    const int mw1 = int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINWIDTHN, 1L) * scale);
    const int mw2 = int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINWIDTHN, 2L) * scale);
    const int numbersEnd = qMin(img.width(), mw0);
    const int foldStart = qMin(img.width(), numbersEnd + mw1);
    const int foldEnd = qMin(img.width(), foldStart + mw2);

    const QColor paper = m_paperColor;

    int numberInk = 0, foldInk = 0, white = 0, guideInk = 0;
    const QColor guide(0x3e, 0x42, 0x47);   // 缩进参考线的颜色
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < numbersEnd; ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c == QColor(Qt::white))
                ++white;
            else if (qAbs(c.red() - paper.red()) + qAbs(c.green() - paper.green())
                         + qAbs(c.blue() - paper.blue()) > 90)
                ++numberInk;
        }
        for (int x = foldEnd; x < img.width(); ++x) {
            if (img.pixelColor(x, y) == guide)
                ++guideInk;
        }
        for (int x = foldStart; x < foldEnd; ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c == QColor(Qt::white))
                ++white;
            else if (qAbs(c.red() - paper.red()) + qAbs(c.green() - paper.green())
                         + qAbs(c.blue() - paper.blue()) > 90)
                ++foldInk;
        }
    }

    out[0] = numberInk;
    out[1] = foldInk;
    out[2] = white;
    out[7] = mw0;
    out[8] = mw1;
    out[9] = mw2;
    out[12] = guideInk;
    return out;
}

void EditorViewItem::styleChrome() {
    if (!m_sci)
        return;

    /* ---- 1) 去掉边框 ---- */
    m_sci->setFrameShape(QFrame::NoFrame);
    m_sci->setFrameShadow(QFrame::Plain);
    m_sci->setLineWidth(0);
    m_sci->setMidLineWidth(0);

    /* ---- 2) 滚动条：深色、细、带圆角，轨道不见白 ---- */
    /*
     * 规则之间必须有换行（写成 \n 转义）。
     *
     * 最早把所有规则拼成一整行、选择器之间只隔一个空格，Qt 的 CSS 解析器
     * 会整段判为无效 —— 滚动条保持系统默认样式（实测右边一条 12px 宽的
     * #f3f3f3 浅色竖带）。加了换行才会生效。
     */
    const QString bar = QStringLiteral(
        "QScrollBar:vertical {\n"
        "    background: transparent;\n"
        "    width: 12px;\n"
        "    margin: 0px;\n"
        "    border: none;\n"
        "}\n"
        "QScrollBar::handle:vertical {\n"
        "    background: %1;\n"
        "    min-height: 28px;\n"
        /*
         * 半径要按"可绘制宽度"给：轨道 12px 减去左右各 3px margin，
         * 实际只有 6px 宽，radius 给 3px 才是真正的胶囊形。
         */
        "    border-radius: 3px;\n"
        "    margin: 3px;\n"
        "}\n"
        "QScrollBar::handle:vertical:hover {\n"
        "    background: %2;\n"
        "}\n"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {\n"
        "    height: 0px;\n"
        "    background: none;\n"
        "    border: none;\n"
        "}\n"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {\n"
        "    background: transparent;\n"
        "}\n"
        "QScrollBar:horizontal {\n"
        "    background: transparent;\n"
        "    height: 12px;\n"
        "    margin: 0px;\n"
        "    border: none;\n"
        "}\n"
        "QScrollBar::handle:horizontal {\n"
        "    background: %1;\n"
        "    min-width: 28px;\n"
        "    border-radius: 3px;\n"
        "    margin: 3px;\n"
        "}\n"
        "QScrollBar::handle:horizontal:hover {\n"
        "    background: %2;\n"
        "}\n"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {\n"
        "    width: 0px;\n"
        "    background: none;\n"
        "    border: none;\n"
        "}\n"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {\n"
        "    background: transparent;\n"
        "}\n")
        .arg(QStringLiteral("#4b4d4f"),      // 滑块
             QStringLiteral("#5f6266"));     // 悬停

    /*
     * 整块控件的 Window 色也要设成底色。
     *
     * 滚动条预留区（以及横竖交汇的角落）用的是 QPalette::Window，
     * 不设的话在深色主题下就是一条浅灰边（实测右侧 12px 竖带 #f3f3f3）。
     * 注意这里用调色板而**不是** stylesheet —— 给 QsciScintilla 设全局
     * stylesheet 会把整个视口背景覆盖、正文完全看不见。
     */
    QPalette panelPal = m_sci->palette();
    panelPal.setColor(QPalette::Window, m_paperColor);
    panelPal.setColor(QPalette::Base, m_paperColor);
    panelPal.setColor(QPalette::Button, m_paperColor);
    panelPal.setColor(QPalette::Highlight, QColor(0x2f, 0x65, 0x9c));
    panelPal.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    m_sci->setPalette(panelPal);
    if (m_sci->viewport()) {
        m_sci->viewport()->setPalette(panelPal);
        m_sci->viewport()->setAutoFillBackground(true);
    }

    /* 滚动条的调色板：轨道用底色、滑块用深灰 */
    QPalette barPal = m_sci->palette();
    barPal.setColor(QPalette::Window, m_paperColor);
    barPal.setColor(QPalette::Base, m_paperColor);
    barPal.setColor(QPalette::Button, QColor(0x4b, 0x4d, 0x4f));
    barPal.setColor(QPalette::Light, m_paperColor);
    barPal.setColor(QPalette::Midlight, m_paperColor);
    barPal.setColor(QPalette::Dark, m_paperColor);
    barPal.setColor(QPalette::Mid, m_paperColor);
    barPal.setColor(QPalette::Shadow, m_paperColor);

    /*
     * 用 QScintilla 自己的滚动条，只给它设样式 —— **不要**替换成自己的控件。
     *
     * 实测替换（replaceVerticalScrollBar / replaceHorizontalScrollBar）之后
     * QScintilla 的"按需显示"逻辑会失效：
     *   1) 短内容时横向滚动条也常显；
     *   2) pageStep 变成 4 这种明显不对的值。
     * 因为 Scintilla 内部是按它自己创建的滚动条对象管理的，换掉就断了联动。
     */
    if (auto *vb = m_sci->verticalScrollBar()) {
        vb->setPalette(barPal);
        vb->setStyleSheet(bar);
    }
    if (auto *hb = m_sci->horizontalScrollBar()) {
        hb->setPalette(barPal);
        hb->setStyleSheet(bar);
    }

    /*
     * 横竖滚动条交汇处的角落控件默认按系统调色板画 —— 深色主题下就是
     * 右下角那块白色正方形。换成一个跟底色一致的控件把它盖掉。
     */
    if (auto *corner = m_sci->cornerWidget()) {
        corner->setStyleSheet(QStringLiteral("background: %1;").arg(m_paperColor.name()));
        corner->setAutoFillBackground(true);
    } else {
        auto *blank = new QWidget(m_sci);
        blank->setStyleSheet(QStringLiteral("background: %1;").arg(m_paperColor.name()));
        blank->setAutoFillBackground(true);
        blank->setFixedSize(12, 12);
        m_sci->setCornerWidget(blank);
    }

    /*
     * 查找结果的"全部高亮"指示器。
     *
     * 用容器指示器 8（0~7 留给 lexer，Scintilla 里 8 起叫 INDIC_CONTAINER，
     * 必须配 SCI_INDICSETUNDER 才允许设置样式）。半透明圆角框是主流编辑器
     * 那种"浅色描边 + 淡底"的效果。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE, long(kFindIndicator),
                         long(kIndicRoundBox));
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETUNDER, long(kFindIndicator), 1L);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE, long(kFindIndicator),
                         scColor(kAccent));
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA, long(kFindIndicator), 48L);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETOUTLINEALPHA, long(kFindIndicator), 150L);

    /*
     * 注意：不要给 QsciScintilla 自己设全局 stylesheet。
     *
     * 实测写成 "QsciScintilla { background: ... }" 会让整个视口被背景色
     * 覆盖、正文完全看不见（文字像素数 0）。底色交给 setPaper() 处理即可。
     */
}

void EditorViewItem::applyViewOptions() {
    if (!m_sci)
        return;

    m_sci->setWrapMode(m_wrap ? QsciScintilla::WrapWord : QsciScintilla::WrapNone);

    /* 编辑行为：主流编辑器的默认手感 */
    m_sci->setIndentationsUseTabs(false);
    m_sci->setTabWidth(4);
    m_sci->setAutoIndent(true);
    m_sci->setBackspaceUnindents(true);
    m_sci->setBraceMatching(QsciScintilla::SloppyBraceMatch);
    m_sci->setAutoCompletionSource(QsciScintilla::AcsNone);

    /* 当前行高亮 + 光标 */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETCARETLINEVISIBLE, 1L);
    /*
     * 编辑区失去焦点时也保留当前行高亮（IDE 的习惯），顺带让"当前行文字
     * 有没有被底色盖住"这件事在任何时候都能被自检的像素检查看到。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETCARETLINEVISIBLEALWAYS, 1L);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETCARETLINEBACK,
                         scColor(kCaretLineBack));
    /*
     * 当前行底色的 alpha 必须给 256（SC_ALPHA_NOALPHA），不能给 255。
     *
     * Scintilla 只在 alpha == SC_ALPHA_NOALPHA(256) 时把当前行底色当**背景**
     * 画在文字下面（见 EditView.cpp 里 DrawFrame 那一支）；其余值是
     * "画完文字之后再叠一层半透明色"（DrawTranslucentLineState →
     * SimpleAlphaRectangle）。给 255 等于用近乎不透明的底色把整行文字糊掉 ——
     * 表现就是"光标移到哪一行，那一行的字就看不见了"（实测：光标行文字像素
     * 0 个，相邻行 230~350 个）。
     *
     * 注意 SC_ALPHA_OPAQUE 也是 255，名字很像但语义相反：这里要的是
     * "不要 alpha 通道"，不是"不透明"。想调成半透明效果（比如 128）也可以，
     * 但文字会被那层色罩住一点，属正常表现。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETCARETLINEBACKALPHA, 256L);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETCARETFORE,
                         scColor(QColor(0xd6, 0xd7, 0xda)));

    /*
     * 代码折叠：第 2 列做成折叠边距（具体设置在 applyMargins 里）。
     * 这里只在切换开关时重排一次边距。
     */

    /* 选中色：和 QML 外壳的强调蓝一套 */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSELBACK, 1L,
                         scColor(kSelectionBack));
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSELFORE, 1L,
                         static_cast<long>(0xffffff));

    /* 空白字符 */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETVIEWWS,
                         m_whitespace ? kScWsVisibleAlways : 0L);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETVIEWEOL, m_whitespace ? 1L : 0L);

    /* 缩进参考线：独立开关，和"显示空白字符"不再绑在一起 */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDENTATIONGUIDES,
                         m_indentGuides ? kScIvLookBoth : 0L);

    m_sci->SendScintilla(QsciScintillaBase::SCI_SETREADONLY, m_readOnly ? 1L : 0L);

    /*
     * 折叠属性。
     *
     * 折叠层级是 lexer 在着色时按这些 property 算出来的：
     *   fold          —— 总开关（QScintilla 的 setLexer 也会设）
     *   fold.comment  —— 注释块也可以折
     *   fold.compact  —— 关掉"空行也算一层"的紧凑模式，折出来的层级更符合直觉
     *
     * 自动折叠只开 SHOW | CHANGE：让折叠标记一出现就画出来、改文本时跟着更新。
     * **不要**开 CLICK —— 点折叠边距这件事 QScintilla 自己在
     * handleMarginClick() 里做了（SCI_TOGGLEFOLD），两边都开等于点一次折叠两次，
     * 看起来就是"点了没反应"。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETPROPERTY, "fold", "1");
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETPROPERTY, "fold.comment", "1");
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETPROPERTY, "fold.compact", "0");
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETAUTOMATICFOLD,
                         long(QsciScintillaBase::SC_AUTOMATICFOLD_SHOW
                              | QsciScintillaBase::SC_AUTOMATICFOLD_CHANGE));

    applyMargins();
}

void EditorViewItem::applyMargins() {
    if (!m_sci)
        return;

    const int lines = qMax(1, lineCount());
    const int digits = QString::number(lines).size() + 1;
    const QFontMetrics fm(uiFont());
    const int width = 10 + digits * fm.horizontalAdvance(QLatin1Char('9'));

    m_sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINWIDTHN, 0,
                         m_lineNumbers ? long(width) : 0L);

    /*
     * 代码折叠：第 2 列做成折叠边距。
     *
     * 用 QScintilla 现成的 BoxedTreeFoldStyle —— 它把 margin 2 设成
     * SC_MARGIN_SYMBOL + SC_MASK_FOLDERS + 敏感（点击即可折叠），并配好
     * "方框 + 竖线"那套树形标记（和主流编辑器观感一致）。
     * 折叠层级是 lexer 着色时算出来的：QScintilla 的 setLexer() 会给文档设置
     * fold=1 属性，所以换了语言（装了 lexer）就有折叠点。
     */
    if (m_folding)
        m_sci->setFolding(QsciScintilla::BoxedTreeFoldStyle, 2);
    else
        m_sci->setFolding(QsciScintilla::NoFoldStyle, 2);

    /*
     * 第 1 列不用，宽度清零。
     *
     * 这里原来放过"当前行蓝色竖条"（3px 符号边距 + SC_MARK_FULLRECT 标记），
     * 已按使用意见去掉 —— 当前行有正文那层底色加光标就够醒目了。
     *
     * 顺带记一笔查证结果（两条路都走过了，别再绕）：
     *
     * 1) "让光标那一行的**行号**高亮"在**数字边距**里做不到：号码写死用
     *    vs.styles[STYLE_LINENUMBER] 一种颜色画、整列底色也只填一次
     *    （Scintilla 的 MarginView.cpp:370 一带），逐行样式对它无效。
     * 2) 换成**文本边距**（SC_MARGIN_RTEXT + SCI_MARGINSETTEXT/SETSTYLE，
     *    MarginView.cpp:409 那一支确实按行样式填格子）**验证过、已回退**：
     *    号码能填、当前行也能单独上色，但一旦在编辑区里写这套逐行边距数据，
     *    QML 侧就再也认不到这个 EditorView 对象了（editor.view 变 null，
     *    随后 fontSize / zoomIn / newDocument 这些命令全部打空）。现象是
     *    "改一次字号，编辑器就跟界面失联"，所以这条实现不能留。
     *    真要做，得另起一个原生 QWidget 自画行号栏，不动 Scintilla 的边距。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINWIDTHN, 1L, 0L);

    /* 颜色最后压：装 lexer 时那次 STYLECLEARALL 会把行号样式刷回白底 */
    themeFoldMarkers();
    applyMarginTheme();
}

void EditorViewItem::applyLanguageLexer() {
    if (!m_sci)
        return;

    const QString lang = hasDocument() ? m_docs[m_current].language
                                       : QStringLiteral("plain");

    if (lang == QStringLiteral("plain")) {
        m_sci->setLexer(nullptr);
    } else {
        m_sci->setLexer(lexerFor(lang));
    }

    m_appliedLanguage = lang;
}

QsciLexer *EditorViewItem::lexerFor(const QString &id) {
    if (m_lexers.contains(id))
        return m_lexers.value(id);
    if (!m_sci)
        return nullptr;

    QsciLexer *lexer = nullptr;
    if (id == QLatin1String("cpp"))            lexer = new QsciLexerCPP(m_sci);
    else if (id == QLatin1String("csharp"))    lexer = new QsciLexerCSharp(m_sci);
    else if (id == QLatin1String("java"))      lexer = new QsciLexerJava(m_sci);
    else if (id == QLatin1String("python"))    lexer = new QsciLexerPython(m_sci);
    else if (id == QLatin1String("javascript"))lexer = new QsciLexerJavaScript(m_sci);
    else if (id == QLatin1String("json"))      lexer = new QsciLexerJSON(m_sci);
    else if (id == QLatin1String("html"))      lexer = new QsciLexerHTML(m_sci);
    else if (id == QLatin1String("css"))       lexer = new QsciLexerCSS(m_sci);
    else if (id == QLatin1String("markdown"))  lexer = new QsciLexerMarkdown(m_sci);
    else if (id == QLatin1String("sql"))       lexer = new QsciLexerSQL(m_sci);
    else if (id == QLatin1String("bash"))      lexer = new QsciLexerBash(m_sci);
    else if (id == QLatin1String("batch"))     lexer = new QsciLexerBatch(m_sci);
    else if (id == QLatin1String("yaml"))      lexer = new QsciLexerYAML(m_sci);
    else if (id == QLatin1String("cmake"))     lexer = new QsciLexerCMake(m_sci);
    else if (id == QLatin1String("makefile"))  lexer = new QsciLexerMakefile(m_sci);
    else if (id == QLatin1String("properties"))lexer = new QsciLexerProperties(m_sci);
    else if (id == QLatin1String("diff"))      lexer = new QsciLexerDiff(m_sci);
    else if (id == QLatin1String("tex"))       lexer = new QsciLexerTeX(m_sci);
    else if (id == QLatin1String("perl"))      lexer = new QsciLexerPerl(m_sci);
    else if (id == QLatin1String("ruby"))      lexer = new QsciLexerRuby(m_sci);
    else if (id == QLatin1String("lua"))       lexer = new QsciLexerLua(m_sci);
    else if (id == QLatin1String("verilog"))   lexer = new QsciLexerVerilog(m_sci);
    else if (id == QLatin1String("vhdl"))      lexer = new QsciLexerVHDL(m_sci);
    /*
     * 汇编：QsciLexerAsm 本身是抽象基类（语言名由子类给），
     * 能实例化的是 NASM / MASM 两个子类，这里用 NASM（Intel 语法最常见）。
     */
    else if (id == QLatin1String("asm"))       lexer = new QsciLexerNASM(m_sci);
    else if (id == QLatin1String("fortran"))   lexer = new QsciLexerFortran(m_sci);
    else if (id == QLatin1String("pascal"))    lexer = new QsciLexerPascal(m_sci);

    if (!lexer)
        return nullptr;

    themeLexer(lexer);
    m_lexers.insert(id, lexer);
    return lexer;
}

void EditorViewItem::themeLexer(QsciLexer *lexer) {
    if (!lexer)
        return;

    /*
     * 字号必须走 uiFont()：QScintilla 交给 Scintilla 的是 pointSizeF()，
     * 用 setPixelSize() 造的字体 pointSizeF() == -1，一装 lexer 字号就变负值。
     * 注释样式走 commentFont()（家族同正文、字号可单独设、一直斜体）。
     */
    QFont mono = uiFont();
    QFont italic = commentFont();
    QFont bold = mono;
    bold.setBold(true);
    bold.setWeight(QFont::DemiBold);

    lexer->setDefaultColor(m_textColor);
    lexer->setDefaultPaper(m_paperColor);

    for (int style = 0; style <= 127; ++style) {
        const QString description = lexer->description(style);
        if (description.isEmpty())
            continue;

        lexer->setColor(themeColorFor(description), style);
        lexer->setPaper(m_paperColor, style);

        if (isCommentDescription(description))
            lexer->setFont(italic, style);
        else if (isKeywordDescription(description))
            lexer->setFont(bold, style);
        else
            lexer->setFont(mono, style);
    }
}

void EditorViewItem::applyPadding() {
    if (!m_sci)
        return;

    /*
     * 这是 Scintilla 的"页边距"（整块文本区相对窗口的内缩），
     * 不是行号栏的边距 —— 用它给正文留出左右留白。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINLEFT, 0L,
                         static_cast<long>(m_paddingLeft));
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINRIGHT, 0L,
                         static_cast<long>(m_paddingRight));
}

void EditorViewItem::updateHorizontalScroll() {
    if (!m_sci)
        return;

    /*
     * 只有内容真的比视口宽时才显示横向滚动条。
     *
     * 为什么不能偷懒：
     *   * Scintilla 的 scrollWidth 默认是 2000 像素（Editor.cpp:155），
     *     横向范围 = scrollWidth - 视口宽度，只要视口比 2000 窄就恒 > 0，
     *     于是短内容也一直挂着横条；
     *   * SCI_SETSCROLLWIDTH 要求 wParam > 0（Editor.cpp:6659），
     *     传 0 无效；
     *   * SCI_SETHSCROLLBAR 0 是永久关闭，长内容也滚不了。
     *
     * 做法：**实际量**最长行的像素宽度。
     *   1) 先按字符数找最长行（只比长度，很便宜）；
     *   2) 只对那一行量一次实际像素宽度；
     *   3) 放得下 -> scrollWidth 设成视口宽（范围 0，横条隐藏）；
     *      超了   -> 设成内容宽度（横条出现）。
     *
     * 注意不能用"字符数 × 字符宽 × 系数"估算：那个系数会多算一截，
     * 结果就是横条出现、还能向右滚正好多算的那些像素（实测过）。
     */
    const long viewWidth = m_sci->viewport() ? m_sci->viewport()->width() : 0;
    if (viewWidth <= 0)
        return;

    /* 自动换行时内容永远不超过视口宽度，横条恒隐藏 */
    if (m_wrap) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTH,
                             (unsigned long)qMax(1L, viewWidth));
        return;
    }

    const long lineCountNow =
        m_sci->SendScintilla(QsciScintillaBase::SCI_GETLINECOUNT);
    if (lineCountNow <= 0)
        return;

    /* 1) 找字符数最多的那一行 */
    long longestLine = 0;
    long maxChars = 0;
    for (long i = 0; i < lineCountNow; ++i) {
        const long start =
            m_sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, i);
        const long end =
            m_sci->SendScintilla(QsciScintillaBase::SCI_GETLINEENDPOSITION, i);
        const long n = end - start;
        if (n > maxChars) {
            maxChars = n;
            longestLine = i;
        }
    }

    /*
     * 2) 量这一行的实际像素宽度。
     *
     * 用 QFontMetrics 而不是 Scintilla 的 SCI_TEXTWIDTH：
     * 后者的参数是字符串指针（uintptr_t），64 位下类型写错就会截断指针、
     * 让 Scintilla 去读坏地址（实测直接 0xC0000005 崩溃）。字体是同一个
     * （编辑器就用的它），量出来的宽度一致。
     */
    long contentWidth = 0;
    if (maxChars > 0) {
        const QString line = m_sci->text(int(longestLine));
        if (!line.isEmpty()) {
            const QFontMetrics fm(uiFont());
            contentWidth = (long)fm.horizontalAdvance(line) + 8;
        }
    }

    /*
     * 放得下：把 scrollWidth 设成视口宽度，横向范围为 0 -> 横条自动隐藏。
     * 需要横滚：设成内容宽度。
     * 两个分支都 > 0，满足 Scintilla 的断言要求。
     */
    const long scrollWidth = (contentWidth <= viewWidth) ? viewWidth
                                                         : contentWidth;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTH,
                         (unsigned long)qMax(1L, scrollWidth));
}

/* ------------------------------------------------------------------ */
/* 几何                                                                */
/* ------------------------------------------------------------------ */

void EditorViewItem::applyGeometry() {
    if (!m_sci || !m_sciWidget || !m_hostWidget || !window())
        return;

    /* 尺寸还没定下来就先不动 */
    if (width() < 2 || height() < 2)
        return;

    /*
     * 把本 Item 的坐标换算到宿主 QWidget 的控件坐标系。
     *
     * mapToItem(nullptr, ...) 给的是场景坐标（等于窗口内容区坐标），
     * 而 QML 内容在 QQuickWidget 里是有黑边的（QQuickWidget 不支持
     * 真正的透明，会自己留一圈），所以再减去 QQuickWidget 相对宿主的偏移。
     */
    const QPointF scene = mapToItem(nullptr, QPointF(0, 0));

    QPoint origin(0, 0);
    auto *qw = m_hostWidget->findChild<QQuickWidget *>();
    if (qw)
        origin = qw->mapTo(m_hostWidget, QPoint(0, 0));

    const int x = origin.x() + qRound(scene.x());
    const int y = origin.y() + qRound(scene.y());
    const int w = qMax(1, qRound(width()));

    /*
     * 高度就用本 Item 的高度，**不要**超过容器。
     *
     * QML 已经把 EditorView 布局在卡片（contentArea）内部，Item 的底边
     * 就是卡片内容区的底边。
     */
    const int h = qMax(1, qRound(height()));

    /* 尺寸还没定下来就别显示：否则会露出一块 640x480 的默认尺寸 */
    if (!isVisible()) {
        m_sciWidget->hide();
        return;
    }

    m_sciWidget->setGeometry(x, y, w, h);
    m_sci->setGeometry(0, 0, w, h);
    m_sciWidget->raise();

    if (!m_sciWidget->isVisible())
        m_sciWidget->show();

    /*
     * 几何确定之后再应用一次样式。
     *
     * styleChrome() 里设的滚动条策略/宽度是按"当前文本区域宽度"算的
     * （见 ScintillaQt.cpp 的 ModifyScrollBars），而 ensureWrapped() 第一次
     * 调它时控件还是默认尺寸，算出来的横向范围是错的
     * （实测 pageStep=4，导致短内容也一直显示横向滚动条）。
     */
    if (!m_chromeApplied) {
        m_chromeApplied = true;
        applyStyle();
    }

    /*
     * 视口宽度变了要重算横向范围（换窗口大小 / 最大化之后横条可能不对）。
     * 同样排到下一轮：此刻 viewport 的宽度还没落定。
     */
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });

    m_sci->viewport()->update();
    m_sci->update();
}

void EditorViewItem::geometryChange(const QRectF &newGeometry,
                                    const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);

    /*
     * 位置也要跟：原来只比较 size，主窗口移动或布局变化导致的位置变化
     * 会完全被漏掉（编辑区窗口就飘走了）。
     */
    if (newGeometry != oldGeometry)
        applyGeometry();
}

void EditorViewItem::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickItem::itemChange(change, value);

    switch (change) {
    case ItemSceneChange:
        if (value.window) {
            ensureWrapped();
            applyGeometry();
        } else if (m_sciWidget) {
            m_sciWidget->hide();
        }
        break;
    case ItemVisibleHasChanged:
        if (m_sciWidget) {
            if (value.boolValue) {
                applyGeometry();
                m_sciWidget->show();
            } else {
                m_sciWidget->hide();
            }
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 属性                                                                */
/* ------------------------------------------------------------------ */

void EditorViewItem::setPaddingLeft(int v) {
    if (v == m_paddingLeft)
        return;
    m_paddingLeft = v;
    applyPadding();
    emit paddingChanged();
}

void EditorViewItem::setPaddingTop(int v) {
    if (v == m_paddingTop)
        return;
    m_paddingTop = v;
    emit paddingChanged();
}

void EditorViewItem::setPaddingRight(int v) {
    if (v == m_paddingRight)
        return;
    m_paddingRight = v;
    applyPadding();
    emit paddingChanged();
}

void EditorViewItem::setPaddingBottom(int v) {
    if (v == m_paddingBottom)
        return;
    m_paddingBottom = v;
    emit paddingChanged();
}

void EditorViewItem::setFontPixelSize(int px) {
    if (px < 6 || px > 72 || px == m_fontPixelSize)
        return;
    m_fontPixelSize = px;

    /* 换字号要连 lexer 的字体一起换，否则语法高亮的字还是旧的 */
    for (QsciLexer *lexer : std::as_const(m_lexers))
        themeLexer(lexer);

    applyStyle();
    emit fontChanged();
}

void EditorViewItem::setFontFamily(const QString &family) {
    if (family.isEmpty() || family == m_fontFamily)
        return;
    m_fontFamily = family;

    /* 同上：lexer 的字体也得跟着重装，否则只有"没装 lexer"的正文换字体 */
    for (QsciLexer *lexer : std::as_const(m_lexers))
        themeLexer(lexer);

    applyStyle();
    emit fontChanged();
}

void EditorViewItem::setCommentFontPixelSize(int px) {
    /* 0 = 跟随正文；其余按像素给（和正文字号同一个单位） */
    if (px < 0 || px > 72 || px == m_commentFontPixelSize)
        return;
    m_commentFontPixelSize = px;

    for (QsciLexer *lexer : std::as_const(m_lexers))
        themeLexer(lexer);

    applyStyle();
    emit fontChanged();
}

void EditorViewItem::setTextColor(const QColor &c) {
    if (m_textColor == c)
        return;
    m_textColor = c;
    applyStyle();
    emit colorsChanged();
}

void EditorViewItem::setPaperColor(const QColor &c) {
    if (m_paperColor == c)
        return;
    m_paperColor = c;
    applyStyle();
    emit colorsChanged();
}

void EditorViewItem::setGutterColor(const QColor &c) {
    if (m_gutterColor == c)
        return;
    m_gutterColor = c;
    applyStyle();
    emit colorsChanged();
}

void EditorViewItem::setLineNumberColor(const QColor &c) {
    if (m_lineNumberColor == c)
        return;
    m_lineNumberColor = c;
    applyStyle();
    emit colorsChanged();
}

void EditorViewItem::setWrapEnabled(bool on) {
    if (m_wrap == on)
        return;
    m_wrap = on;
    if (m_sci) {
        m_sci->setWrapMode(on ? QsciScintilla::WrapWord : QsciScintilla::WrapNone);
        QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
    }
    emit wrapChanged();
}

void EditorViewItem::setLineNumbersVisible(bool on) {
    if (m_lineNumbers == on)
        return;
    m_lineNumbers = on;
    applyMargins();
    emit lineNumbersChanged();
}

void EditorViewItem::setWhitespaceVisible(bool on) {
    if (m_whitespace == on)
        return;
    m_whitespace = on;
    if (m_sci) {
        /* 只管空白字符；缩进参考线是另一个开关（见 setIndentGuidesVisible） */
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETVIEWWS,
                             on ? kScWsVisibleAlways : 0L);
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETVIEWEOL, on ? 1L : 0L);
        m_sci->viewport()->update();
    }
    emit whitespaceChanged();
}

void EditorViewItem::setIndentGuidesVisible(bool on) {
    if (m_indentGuides == on)
        return;
    m_indentGuides = on;
    if (m_sci) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDENTATIONGUIDES,
                             on ? kScIvLookBoth : 0L);
        m_sci->viewport()->update();
    }
    emit indentGuidesChanged();
}

void EditorViewItem::setReadOnly(bool on) {
    if (m_readOnly == on)
        return;
    m_readOnly = on;
    if (m_sci) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETREADONLY, on ? 1L : 0L);
        m_sci->viewport()->update();
    }
    emit readOnlyChanged();
}

/* ------------------------------------------------------------------ */
/* 文档                                                                */
/* ------------------------------------------------------------------ */

EditorViewItem::Doc *EditorViewItem::currentDoc() {
    if (m_current < 0 || m_current >= m_docs.size())
        return nullptr;
    return &m_docs[m_current];
}

QString EditorViewItem::displayName() const {
    if (!hasDocument())
        return QString();
    const Doc &d = m_docs.at(m_current);
    if (!d.filePath.isEmpty())
        return QFileInfo(d.filePath).fileName();
    if (d.clipboard)
        return d.clipTitle.isEmpty() ? QStringLiteral("剪贴板内容") : d.clipTitle;
    return QStringLiteral("未命名 %1").arg(d.untitledNo);
}

QString EditorViewItem::filePath() const {
    if (!hasDocument())
        return QString();
    return m_docs.at(m_current).filePath;
}

bool EditorViewItem::modified() const {
    if (!hasDocument())
        return false;
    return m_docs.at(m_current).modified;
}

void EditorViewItem::setModified(bool m) {
    /*
     * 只能"清空"修改标记（Scintilla 没有反向的 SCI_SETMODIFY）。
     * m == true 时什么都不做：真正的修改标记由用户编辑产生。
     */
    if (m || !m_sci || !hasDocument())
        return;
    m_bulkLoading = true;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);
    m_bulkLoading = false;
    m_docs[m_current].modified = false;
    emit modifiedChanged();
    emit documentsChanged();
}

QString EditorViewItem::language() const {
    if (!hasDocument())
        return QStringLiteral("plain");
    return m_docs.at(m_current).language;
}

void EditorViewItem::setLanguage(const QString &id) {
    if (!hasDocument() || id.isEmpty())
        return;
    if (m_docs.at(m_current).language == id)
        return;
    m_docs[m_current].language = id;

    /*
     * 必须走 applyStyle()，不能只调 applyLanguageLexer()。
     *
     * 装 / 卸 lexer 时 QScintilla 会 SCI_STYLERESETDEFAULT + SCI_STYLECLEARALL，
     * 把**整张样式表**刷回默认值（白底黑字），只有 lexer 自己覆盖到的那些样式号
     * 会被重新设色。基础样式、行号样式、边距主题都得在这之后再刷一遍 ——
     * 这就是"从菜单里换个语言，行号栏变白 / 正文底色不对"的来源。
     */
    applyStyle();
    emit languageChanged();
    emit documentsChanged();
}

QString EditorViewItem::encoding() const {
    if (!hasDocument())
        return QStringLiteral("UTF-8");
    return m_docs.at(m_current).encoding;
}

void EditorViewItem::setEncoding(const QString &name) {
    if (!hasDocument() || name.isEmpty())
        return;
    if (m_docs.at(m_current).encoding == name)
        return;
    m_docs[m_current].encoding = name;
    emit encodingChanged();
}

QString EditorViewItem::eolMode() const {
    if (!m_sci)
        return QStringLiteral("LF");
    switch (m_sci->SendScintilla(QsciScintillaBase::SCI_GETEOLMODE)) {
    case kScEolCrLf: return QStringLiteral("CRLF");
    case kScEolCr:   return QStringLiteral("CR");
    default:         return QStringLiteral("LF");
    }
}

void EditorViewItem::setEolMode(const QString &name) {
    if (!m_sci || !hasDocument())
        return;
    const long mode = (name == QLatin1String("CRLF")) ? kScEolCrLf
                    : (name == QLatin1String("CR"))     ? kScEolCr
                                                        : kScEolLf;
    if (m_sci->SendScintilla(QsciScintillaBase::SCI_GETEOLMODE) == mode)
        return;
    /*
     * 换行符是**内容的一部分**：转换会把文档标成已修改，
     * 和主流编辑器一致（不顺手改文件，等用户保存）。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETEOLMODE, mode);
    m_sci->SendScintilla(QsciScintillaBase::SCI_CONVERTEOLS, mode);
    emit eolChanged();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
}

QVariantList EditorViewItem::documents() const {
    QVariantList out;
    for (int i = 0; i < m_docs.size(); ++i) {
        const Doc &d = m_docs.at(i);
        QVariantMap m;
        m.insert(QStringLiteral("index"), i);
        m.insert(QStringLiteral("title"), titleOf(d));
        m.insert(QStringLiteral("filePath"), d.filePath);
        m.insert(QStringLiteral("modified"), d.modified);
        m.insert(QStringLiteral("active"), i == m_current);
        m.insert(QStringLiteral("clipboard"), d.clipboard);
        m.insert(QStringLiteral("language"), d.language);
        out.append(m);
    }
    return out;
}

QString EditorViewItem::titleOf(const Doc &d) const {
    if (!d.filePath.isEmpty())
        return QFileInfo(d.filePath).fileName();
    if (d.clipboard)
        return d.clipTitle.isEmpty() ? QStringLiteral("剪贴板内容") : d.clipTitle;
    return QStringLiteral("未命名 %1").arg(d.untitledNo);
}

QVariantList EditorViewItem::languages() const {
    QVariantList out;
    for (const LanguageInfo &info : kLanguages) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), QString::fromLatin1(info.id));
        m.insert(QStringLiteral("label"), QString::fromUtf8(info.label));
        out.append(m);
    }
    return out;
}

QString EditorViewItem::languageForPath(const QString &path) {
    const QFileInfo fi(path);
    const QString name = fi.fileName().toLower();
    const QString suffix = fi.suffix().toLower();

    if (name == QLatin1String("cmakelists.txt") || suffix == QLatin1String("cmake"))
        return QStringLiteral("cmake");
    if (name.startsWith(QLatin1String("makefile")) || suffix == QLatin1String("mk"))
        return QStringLiteral("makefile");

    static const QHash<QString, QString> map = {
        {QStringLiteral("c"), QStringLiteral("cpp")},
        {QStringLiteral("h"), QStringLiteral("cpp")},
        {QStringLiteral("cc"), QStringLiteral("cpp")},
        {QStringLiteral("cpp"), QStringLiteral("cpp")},
        {QStringLiteral("cxx"), QStringLiteral("cpp")},
        {QStringLiteral("hpp"), QStringLiteral("cpp")},
        {QStringLiteral("hh"), QStringLiteral("cpp")},
        {QStringLiteral("hxx"), QStringLiteral("cpp")},
        {QStringLiteral("cs"), QStringLiteral("csharp")},
        {QStringLiteral("java"), QStringLiteral("java")},
        {QStringLiteral("py"), QStringLiteral("python")},
        {QStringLiteral("pyw"), QStringLiteral("python")},
        {QStringLiteral("js"), QStringLiteral("javascript")},
        {QStringLiteral("mjs"), QStringLiteral("javascript")},
        {QStringLiteral("cjs"), QStringLiteral("javascript")},
        {QStringLiteral("ts"), QStringLiteral("javascript")},
        {QStringLiteral("qml"), QStringLiteral("javascript")},
        {QStringLiteral("json"), QStringLiteral("json")},
        {QStringLiteral("html"), QStringLiteral("html")},
        {QStringLiteral("htm"), QStringLiteral("html")},
        {QStringLiteral("xml"), QStringLiteral("html")},
        {QStringLiteral("svg"), QStringLiteral("html")},
        {QStringLiteral("xaml"), QStringLiteral("html")},
        {QStringLiteral("css"), QStringLiteral("css")},
        {QStringLiteral("scss"), QStringLiteral("css")},
        {QStringLiteral("less"), QStringLiteral("css")},
        {QStringLiteral("md"), QStringLiteral("markdown")},
        {QStringLiteral("markdown"), QStringLiteral("markdown")},
        {QStringLiteral("sql"), QStringLiteral("sql")},
        {QStringLiteral("sh"), QStringLiteral("bash")},
        {QStringLiteral("bash"), QStringLiteral("bash")},
        {QStringLiteral("zsh"), QStringLiteral("bash")},
        {QStringLiteral("bat"), QStringLiteral("batch")},
        {QStringLiteral("cmd"), QStringLiteral("batch")},
        {QStringLiteral("yml"), QStringLiteral("yaml")},
        {QStringLiteral("yaml"), QStringLiteral("yaml")},
        {QStringLiteral("ini"), QStringLiteral("properties")},
        {QStringLiteral("conf"), QStringLiteral("properties")},
        {QStringLiteral("properties"), QStringLiteral("properties")},
        {QStringLiteral("diff"), QStringLiteral("diff")},
        {QStringLiteral("patch"), QStringLiteral("diff")},
        {QStringLiteral("tex"), QStringLiteral("tex")},
        {QStringLiteral("pl"), QStringLiteral("perl")},
        {QStringLiteral("pm"), QStringLiteral("perl")},
        {QStringLiteral("rb"), QStringLiteral("ruby")},
        {QStringLiteral("lua"), QStringLiteral("lua")},
        {QStringLiteral("v"), QStringLiteral("verilog")},
        {QStringLiteral("sv"), QStringLiteral("verilog")},
        {QStringLiteral("vhd"), QStringLiteral("vhdl")},
        {QStringLiteral("vhdl"), QStringLiteral("vhdl")},
        {QStringLiteral("asm"), QStringLiteral("asm")},
        {QStringLiteral("s"), QStringLiteral("asm")},
        {QStringLiteral("f90"), QStringLiteral("fortran")},
        {QStringLiteral("for"), QStringLiteral("fortran")},
        {QStringLiteral("pas"), QStringLiteral("pascal")},
        {QStringLiteral("txt"), QStringLiteral("plain")},
        {QStringLiteral("log"), QStringLiteral("plain")},
    };

    return map.value(suffix, QStringLiteral("plain"));
}

QString EditorViewItem::languageLabel(const QString &id) const {
    for (const LanguageInfo &info : kLanguages) {
        if (id == QLatin1String(info.id))
            return QString::fromUtf8(info.label);
    }
    return QStringLiteral("纯文本");
}

int EditorViewItem::indexOfPath(const QString &path) const {
    const QString abs = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_docs.size(); ++i) {
        if (!m_docs.at(i).filePath.isEmpty()
            && QFileInfo(m_docs.at(i).filePath).absoluteFilePath() == abs)
            return i;
    }
    return -1;
}

int EditorViewItem::newDocument() {
    ensureWrapped();
    if (!m_sci)
        return -1;

    /* 先把当前文档的滚动位置和光标记下来，再往列表里加新文档 */
    if (hasDocument())
        storeViewState();

    Doc d;
    d.document = new QsciDocument();
    d.language = QStringLiteral("plain");
    d.untitledNo = ++m_untitledCounter;
    m_docs.append(d);

    const int index = m_docs.size() - 1;
    m_current = index;

    m_sci->setDocument(*m_docs[index].document);
    applyStyle();
    applyViewOptions();

    m_docs[index].modified = false;
    m_docs[index].cursorPos = 0;
    m_docs[index].firstVisibleLine = 0;
    m_docs[index].xOffset = 0;

    emitDocumentsState();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
    return index;
}

int EditorViewItem::openFile(const QString &path) {
    ensureWrapped();
    if (!m_sci)
        return -1;

    if (path.isEmpty()) {
        m_lastError = QStringLiteral("路径为空");
        emit errorOccurred(m_lastError);
        return -1;
    }

    const QFileInfo fi(path);
    const QString abs = fi.absoluteFilePath();

    const int existing = indexOfPath(abs);
    if (existing >= 0) {
        activateDocument(existing);
        return existing;
    }

    if (!fi.exists() || !fi.isFile()) {
        m_lastError = QStringLiteral("文件不存在：%1").arg(abs);
        emit errorOccurred(m_lastError);
        return -1;
    }
    if (fi.size() > kMaxFileBytes) {
        m_lastError = QStringLiteral("文件过大（超过 64 MB），本编辑器不打开");
        emit errorOccurred(m_lastError);
        return -1;
    }

    QFile file(abs);
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = QStringLiteral("无法打开文件：%1").arg(file.errorString());
        emit errorOccurred(m_lastError);
        return -1;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QString detectedEncoding;
    const QString text = decodeBytes(bytes, &detectedEncoding);

    int crlf = bytes.count("\r\n");
    const int lf = bytes.count('\n') - crlf;
    const int cr = bytes.count('\r') - crlf;
    const long eol = (crlf >= lf && crlf >= cr) ? kScEolCrLf
                   : (cr > lf)                 ? kScEolCr
                                               : kScEolLf;

    const int index = newDocument();
    if (index < 0)
        return -1;

    setContentCurrent(text);

    m_docs[m_current].filePath = abs;
    m_docs[m_current].clipboard = false;
    m_docs[m_current].clipId = -1;
    m_docs[m_current].clipTitle.clear();
    m_docs[m_current].encoding = detectedEncoding;
    m_docs[m_current].language = languageForPath(abs);

    m_sci->SendScintilla(QsciScintillaBase::SCI_SETEOLMODE, eol);
    m_sci->SendScintilla(QsciScintillaBase::SCI_CONVERTEOLS, eol);

    applyStyle();
    applyViewOptions();
    m_docs[m_current].modified = false;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);

    emitDocumentsState();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
    return m_current;
}

int EditorViewItem::openClipboardItem(qint64 id, const QString &title) {
    ensureWrapped();
    if (!m_sci)
        return -1;

    if (!m_store) {
        m_lastError = QStringLiteral("数据源未设置");
        emit errorOccurred(m_lastError);
        return -1;
    }

    const QString text = m_store->contentOf(id);

    /*
     * 一个条目一条标签：**点过的条目各占一条标签**，同一条目只占一条。
     *
     *   没开过 -> 新开一条；
     *   已经开着 -> 切回它那条（正文在它自己那份 QsciDocument 里，不重灌）。
     *
     * 原来这里是"复用那条没改过内容的剪贴板标签"：点来点去始终是同一个标签
     * 在换内容，看起来就是"不管点哪个文件都只有一个标签在变"。按 id 认标签
     * 之后，左边点过的条目才一条条攒得下来，而且：
     *   * 正在改的那条不会被下一次点击冲掉 —— 点回它只是切过去，改动原样还在；
     *   * 同一条目不会开出两条一模一样的标签（和 openFile() 按文件路径认标签
     *     是同一套规矩，见 indexOfPath）。
     */
    for (int i = 0; i < m_docs.size(); ++i) {
        const Doc &d = m_docs.at(i);
        if (d.clipboard && d.clipId == id) {
            activateDocument(i);
            return m_current;
        }
    }

    const int target = newDocument();
    if (target < 0)
        return -1;

    QElapsedTimer timer;
    timer.start();

    setContentCurrent(text);

    m_docs[m_current].clipboard = true;
    m_docs[m_current].clipId = id;
    m_docs[m_current].clipTitle = title;
    m_docs[m_current].encoding = QStringLiteral("UTF-8");
    m_docs[m_current].language = QStringLiteral("plain");

    applyStyle();
    m_docs[m_current].modified = false;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);

    m_lastLoadMs = int(timer.elapsed());
    m_lastLoadChars = text.size();

    emitDocumentsState();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
    return m_current;
}

QString EditorViewItem::currentText() const {
    if (!m_sci || !hasDocument())
        return QString();
    /*
     * 走 QScintilla 自己的 text()：它按长度取（bytesAsText(buf, size)），
     * 正文里的内嵌零字节不会被当成字符串结尾截断。
     */
    return m_sci->text();
}

void EditorViewItem::load(qint64 id) { openClipboardItem(id, QString()); }

void EditorViewItem::setContentCurrent(const QString &text) {
    if (!m_sci)
        return;

    /*
     * 灌正文。
     *
     * 先解锁再改内容，改完锁回只读（如果开了只读）。
     *
     * 用 QScintilla 自己的 setText()，而不是直接发 SCI_ADDTEXT：
     * 实测直接发 SCI_ADDTEXT 会在这里崩溃（访问冲突，0xC0000005），
     * setText() 正常（256,499 字符 6ms）。QScintilla 2.14.1 的 setText()
     * 内部走的就是 SCI_ADDTEXT，并且已处理内嵌零字节（见其 ChangeLog），
     * 所以走它既安全又不丢零字节。
     *
     * 注意不要用 SCI_SETTEXT：它收 const char*，遇到内嵌零字节会截断
     * ——实测整篇会被当成 1 行。
     */
    m_bulkLoading = true;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETREADONLY, 0L);
    m_sci->setText(text);
    /* 灌完再确认一次：长行内容插入后换行设置可能被重置 */
    m_sci->setWrapMode(m_wrap ? QsciScintilla::WrapWord : QsciScintilla::WrapNone);
    m_sci->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE, 0L);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET, 0L);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETREADONLY, m_readOnly ? 1L : 0L);
    m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, 0L);
    m_bulkLoading = false;

    m_hasContent = !text.isEmpty();

    if (Doc *d = currentDoc()) {
        d->modified = false;
        d->cursorPos = 0;
        d->firstVisibleLine = 0;
        d->xOffset = 0;
    }
}

bool EditorViewItem::saveCurrent() {
    if (!hasDocument())
        return false;
    if (m_docs.at(m_current).filePath.isEmpty())
        return false;
    return saveDocument(m_current, m_docs.at(m_current).filePath);
}

bool EditorViewItem::saveCurrentAs(const QString &path) {
    if (!hasDocument())
        return false;
    return saveDocument(m_current, path);
}

bool EditorViewItem::saveDocument(int index, const QString &path) {
    if (index < 0 || index >= m_docs.size() || path.isEmpty())
        return false;
    if (!m_sci)
        return false;

    if (index != m_current) {
        activateDocument(index);
        if (m_current != index)
            return false;
    }

    const QString text = m_sci->text();
    const QByteArray bytes = encodeText(text, m_docs.at(index).encoding);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_lastError = QStringLiteral("无法写入文件：%1").arg(file.errorString());
        emit errorOccurred(m_lastError);
        return false;
    }
    const qint64 written = file.write(bytes);
    file.close();
    if (written != bytes.size()) {
        m_lastError = QStringLiteral("写入不完整：%1").arg(path);
        emit errorOccurred(m_lastError);
        return false;
    }

    const QString abs = QFileInfo(path).absoluteFilePath();
    m_docs[index].filePath = abs;
    m_docs[index].clipboard = false;
    m_docs[index].clipId = -1;
    m_docs[index].clipTitle.clear();

    /* 未命名文件另存为之后按扩展名认语言 */
    if (m_docs.at(index).language == QLatin1String("plain")) {
        const QString guess = languageForPath(abs);
        if (guess != QLatin1String("plain")) {
            m_docs[index].language = guess;
            applyLanguageLexer();
            emit languageChanged();
        }
    }

    m_bulkLoading = true;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);
    m_bulkLoading = false;
    m_docs[index].modified = false;

    emit saved(abs);
    emit modifiedChanged();
    emit documentsChanged();
    emit currentChanged();
    return true;
}

void EditorViewItem::closeDocument(int index) {
    if (index < 0 || index >= m_docs.size())
        return;

    if (!m_sci) {
        delete m_docs.at(index).document;
        m_docs.remove(index);
        m_current = m_docs.isEmpty() ? -1 : qMin(m_current, m_docs.size() - 1);
        emitDocumentsState();
        return;
    }

    const bool wasCurrent = (index == m_current);

    /* 关掉的是当前文档时，先挑好接替者 */
    int successor = -1;
    if (wasCurrent) {
        storeViewState();
        if (m_docs.size() > 1)
            successor = (index == m_docs.size() - 1) ? index - 1 : index + 1;
    }

    /*
     * 顺序要紧：先让视图离开这份文档，再删它的 QsciDocument。
     *
     * 反过来的话，QScintilla 的 doc 成员还挂在已经释放的 pdoc 上，
     * 下一次 setDocument / 析构就是读已释放内存。
     */
    if (wasCurrent) {
        if (successor >= 0)
            m_sci->setDocument(*m_docs[successor].document);
        else
            m_sci->setDocument(*m_scratch);
    }

    delete m_docs.at(index).document;
    m_docs.remove(index);

    /* 移除之后下标会前移，把接替者换算成新下标 */
    if (wasCurrent) {
        if (successor > index)
            --successor;
        m_current = successor;
    } else if (index < m_current) {
        --m_current;
    }

    if (m_current >= 0) {
        applyStyle();
        applyViewOptions();
        restoreViewState();
    } else {
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETREADONLY, 1L);
        applyStyle();
    }

    emitDocumentsState();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
}

void EditorViewItem::closeCurrent() {
    if (hasDocument())
        closeDocument(m_current);
}

void EditorViewItem::closeAll() {
    while (!m_docs.isEmpty())
        closeDocument(m_docs.size() - 1);
}

void EditorViewItem::activateDocument(int index) {
    if (index < 0 || index >= m_docs.size() || index == m_current)
        return;
    if (!m_sci)
        return;

    if (hasDocument())
        storeViewState();

    m_current = index;
    m_sci->setDocument(*m_docs[index].document);

    /*
     * 样式和视图设置都是**按文档**存的（Scintilla 的样式表在文档里），
     * 所以切过去之后要整套重装一遍，否则新文档是默认的白底黑字。
     */
    applyStyle();
    applyViewOptions();
    restoreViewState();

    emitDocumentsState();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
    m_sci->viewport()->update();
}

int EditorViewItem::activateNextDocument() {
    if (m_docs.size() < 2)
        return m_current;
    activateDocument((m_current + 1) % m_docs.size());
    return m_current;
}

int EditorViewItem::activatePreviousDocument() {
    if (m_docs.size() < 2)
        return m_current;
    activateDocument((m_current - 1 + m_docs.size()) % m_docs.size());
    return m_current;
}

void EditorViewItem::storeViewState() {
    if (!m_sci || !hasDocument())
        return;
    Doc &d = m_docs[m_current];
    d.cursorPos = long(m_sci->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS));
    d.firstVisibleLine =
        int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
    d.xOffset = int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET));
}

void EditorViewItem::restoreViewState() {
    if (!m_sci || !hasDocument())
        return;
    const Doc &d = m_docs.at(m_current);
    const long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                         qBound(0L, d.cursorPos, docLen));
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                         long(qMax(0, d.firstVisibleLine)));
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET, long(qMax(0, d.xOffset)));
}

void EditorViewItem::emitDocumentsState() {
    emit documentsChanged();
    emit currentChanged();
    emit modifiedChanged();
    emit languageChanged();
    emit encodingChanged();
    emit eolChanged();
    emit cursorChanged();
    emit statsChanged();
    emit undoStateChanged();
}

/* ------------------------------------------------------------------ */
/* 编解码                                                              */
/* ------------------------------------------------------------------ */

QString EditorViewItem::decodeBytes(const QByteArray &bytes,
                                    QString *encodingOut) const {
    if (encodingOut)
        *encodingOut = QStringLiteral("UTF-8");

    if (bytes.startsWith("\xEF\xBB\xBF")) {
        if (encodingOut)
            *encodingOut = QStringLiteral("UTF-8 BOM");
        QStringDecoder dec(QStringConverter::Utf8);
        return dec.decode(bytes.mid(3));
    }
    if (bytes.startsWith("\xFF\xFE")) {
        if (encodingOut)
            *encodingOut = QStringLiteral("UTF-16LE");
        QStringDecoder dec(QStringConverter::Utf16LE);
        return dec.decode(bytes.mid(2));
    }
    if (bytes.startsWith("\xFE\xFF")) {
        if (encodingOut)
            *encodingOut = QStringLiteral("UTF-16BE");
        QStringDecoder dec(QStringConverter::Utf16BE);
        return dec.decode(bytes.mid(2));
    }

    /* 优先按严格 UTF-8 解；解不开就是本地编码（简中 Windows 上是 GBK） */
    QStringDecoder utf8(QStringConverter::Utf8);
    const QString text = utf8.decode(bytes);
    if (!utf8.hasError())
        return text;

    if (encodingOut)
        *encodingOut = QStringLiteral("ANSI");
    QStringDecoder ansi(QStringConverter::System);
    return ansi.decode(bytes);
}

QByteArray EditorViewItem::encodeText(const QString &text,
                                      const QString &encoding) const {
    if (encoding == QLatin1String("UTF-8 BOM")) {
        QByteArray out("\xEF\xBB\xBF", 3);
        out.append(text.toUtf8());
        return out;
    }
    if (encoding == QLatin1String("UTF-16LE")
        || encoding == QLatin1String("UTF-16BE")) {
        const bool le = encoding == QLatin1String("UTF-16LE");
        QByteArray out;
        out.reserve(text.size() * 2 + 2);
        out.append(le ? char(0xFF) : char(0xFE));
        out.append(le ? char(0xFE) : char(0xFF));
        for (const QChar ch : text) {
            const ushort u = ch.unicode();
            if (le) {
                out.append(char(u & 0xFF));
                out.append(char((u >> 8) & 0xFF));
            } else {
                out.append(char((u >> 8) & 0xFF));
                out.append(char(u & 0xFF));
            }
        }
        return out;
    }
    if (encoding == QLatin1String("ANSI")) {
        QStringEncoder enc(QStringConverter::System);
        return enc(text);
    }
    if (encoding == QLatin1String("Latin-1")) {
        QStringEncoder enc(QStringConverter::Latin1);
        return enc(text);
    }
    return text.toUtf8();
}

/* ------------------------------------------------------------------ */
/* 状态读取                                                            */
/* ------------------------------------------------------------------ */

int EditorViewItem::cursorLine() const {
    if (!m_sci || !hasDocument())
        return 0;
    return int(m_sci->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION,
                                    long(m_sci->SendScintilla(
                                        QsciScintillaBase::SCI_GETCURRENTPOS)))) + 1;
}

int EditorViewItem::cursorColumn() const {
    if (!m_sci || !hasDocument())
        return 0;
    return int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN,
                                    long(m_sci->SendScintilla(
                                        QsciScintillaBase::SCI_GETCURRENTPOS)))) + 1;
}

int EditorViewItem::selectionLength() const {
    if (!m_sci || !hasDocument())
        return 0;
    const long start =
        m_sci->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART);
    const long end = m_sci->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND);
    return int(qAbs(end - start));
}

bool EditorViewItem::hasSelection() const { return selectionLength() > 0; }

int EditorViewItem::lineCount() const {
    if (!m_sci)
        return 0;
    return int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETLINECOUNT));
}

int EditorViewItem::charCount() const {
    if (!m_sci || !hasDocument())
        return 0;
    return int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH));
}

bool EditorViewItem::canUndo() const {
    if (!m_sci || !hasDocument())
        return false;
    return m_sci->SendScintilla(QsciScintillaBase::SCI_CANUNDO) != 0;
}

bool EditorViewItem::canRedo() const {
    if (!m_sci || !hasDocument())
        return false;
    return m_sci->SendScintilla(QsciScintillaBase::SCI_CANREDO) != 0;
}

/* ------------------------------------------------------------------ */
/* 编辑命令                                                            *//* ------------------------------------------------------------------ */

void EditorViewItem::undo() {
    if (m_sci && hasDocument() && !m_readOnly)
        m_sci->undo();
}

void EditorViewItem::redo() {
    if (m_sci && hasDocument() && !m_readOnly)
        m_sci->redo();
}

void EditorViewItem::cut() {
    if (m_sci && hasDocument() && !m_readOnly)
        m_sci->cut();
}

void EditorViewItem::copy() {
    if (m_sci && hasDocument())
        m_sci->copy();
}

void EditorViewItem::paste() {
    if (m_sci && hasDocument() && !m_readOnly)
        m_sci->paste();
}

void EditorViewItem::selectAll() {
    if (m_sci && hasDocument())
        m_sci->selectAll();
}

void EditorViewItem::deleteSelection() {
    if (m_sci && hasDocument() && !m_readOnly)
        m_sci->removeSelectedText();
}

void EditorViewItem::duplicateLine() {
    if (!m_sci || !hasDocument() || m_readOnly)
        return;
    m_sci->SendScintilla(QsciScintillaBase::SCI_LINEDUPLICATE);
}

void EditorViewItem::deleteLine() {
    if (!m_sci || !hasDocument() || m_readOnly)
        return;
    m_sci->SendScintilla(QsciScintillaBase::SCI_LINEDELETE);
}

void EditorViewItem::toggleComment(const QString &prefix) {
    if (!m_sci || !hasDocument() || m_readOnly || prefix.isEmpty())
        return;

    int lineFrom = 0, indexFrom = 0, lineTo = 0, indexTo = 0;
    m_sci->getSelection(&lineFrom, &indexFrom, &lineTo, &indexTo);
    if (lineFrom < 0) {
        m_sci->getCursorPosition(&lineFrom, &indexFrom);
        lineTo = lineFrom;
    }
    if (lineTo < lineFrom)
        std::swap(lineFrom, lineTo);

    /* 全部已注释才反注释，否则一律加注释（和主流编辑器一致） */
    bool allCommented = true;
    for (int line = lineFrom; line <= lineTo; ++line) {
        const QString text = m_sci->text(line);
        if (text.trimmed().isEmpty())
            continue;
        if (!text.trimmed().startsWith(prefix)) {
            allCommented = false;
            break;
        }
    }

    const QByteArray prefixBytes = prefix.toUtf8();
    m_sci->SendScintilla(QsciScintillaBase::SCI_BEGINUNDOACTION);

    if (allCommented) {
        for (int line = lineFrom; line <= lineTo; ++line) {
            const QString text = m_sci->text(line);
            const QString trimmed = text.trimmed();
            if (trimmed.isEmpty() || !trimmed.startsWith(prefix))
                continue;
            const int indent = text.indexOf(prefix);
            if (indent < 0)
                continue;
            const long start =
                m_sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, long(line));
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                 start + indent, start + indent + prefixBytes.size());
            m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACESEL, "");
        }
    } else {
        for (int line = lineFrom; line <= lineTo; ++line) {
            const QString text = m_sci->text(line);
            if (text.trimmed().isEmpty())
                continue;
            int indent = 0;
            while (indent < text.size()
                   && (text.at(indent) == QLatin1Char(' ')
                       || text.at(indent) == QLatin1Char('\t')))
                ++indent;
            const long pos =
                m_sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, long(line))
                + indent;
            m_sci->SendScintilla(QsciScintillaBase::SCI_INSERTTEXT,
                                 static_cast<uintptr_t>(pos),
                                 prefixBytes.constData());
        }
    }

    m_sci->SendScintilla(QsciScintillaBase::SCI_ENDUNDOACTION);
    m_sci->viewport()->update();
}

void EditorViewItem::copyCurrentLine() {
    if (!m_sci || !hasDocument())
        return;

    const long line = m_sci->SendScintilla(
        QsciScintillaBase::SCI_LINEFROMPOSITION,
        long(m_sci->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS)));

    /* 用 QScintilla 自己的 text(line)：它会处理好编码与内嵌零字节 */
    const QString text = m_sci->text(int(line));
    if (!text.isEmpty()) {
        if (m_store)
            m_store->copyText(text);
        else
            QGuiApplication::clipboard()->setText(text);
    }
}

void EditorViewItem::copyAll() {
    if (!m_sci || !hasDocument())
        return;

    /* 同上：走 QScintilla 自己的 text()，不直接发 SCI_GETTEXT */
    const QString all = m_sci->text();
    if (!all.isEmpty()) {
        if (m_store)
            m_store->copyText(all);
        else
            QGuiApplication::clipboard()->setText(all);
    }
}

/* ------------------------------------------------------------------ */
/* 查找 / 替换                                                         */
/* ------------------------------------------------------------------ */

long EditorViewItem::searchFrom(long from, long to, const QString &text,
                                bool caseSensitive, bool wholeWord,
                                bool regex) const {
    if (!m_sci || text.isEmpty())
        return -1;

    const QByteArray needle = text.toUtf8();
    long flags = 0;
    if (caseSensitive)
        flags |= kScFindMatchCase;
    if (wholeWord)
        flags |= kScFindWholeWord;
    if (regex)
        flags |= kScFindRegexp;

    const long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    from = qBound(0L, from, docLen);
    to = qBound(0L, to, docLen);
    if (from > to)
        std::swap(from, to);

    m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, from);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, to);
    /*
     * 第二个实参必须显式转成 uintptr_t。
     *
     * 传 unsigned long 的话，这个调用在
     *   (unsigned int, unsigned long, void*) / (unsigned int, unsigned long, const QColor&)
     * 和
     *   (unsigned int, uintptr_t, const char*)
     * 之间是"C2666 重载函数具有类似的转换"：
     * 一个赢在第二个参数、另一个赢在第三个参数，编译器判不出来。
     */
    return m_sci->SendScintilla(QsciScintillaBase::SCI_SEARCHINTARGET,
                                static_cast<uintptr_t>(needle.size()),
                                needle.constData());
}

int EditorViewItem::find(const QString &text, bool caseSensitive, bool wholeWord,
                         bool regex, bool forward) {
    if (!m_sci || !hasDocument() || text.isEmpty())
        return -1;

    const long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    const long selStart =
        m_sci->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONSTART);
    const long selEnd = m_sci->SendScintilla(QsciScintillaBase::SCI_GETSELECTIONEND);

    long hit = -1;

    if (forward) {
        /* 从选区末尾往后找；找不到就从头再找一遍（环绕） */
        const long from = (selEnd > selStart) ? selEnd : selStart;
        hit = searchFrom(from, docLen, text, caseSensitive, wholeWord, regex);
        if (hit < 0)
            hit = searchFrom(0, qMax(0L, selStart), text, caseSensitive,
                             wholeWord, regex);
    } else {
        /*
         * 向后找"上一个命中"最省事的做法是扫一遍取最后一个。
         * 先只扫光标之前；光标前没有才扫全文（环绕到末尾那个）。
         */
        hit = lastMatchBefore(qMax(0L, selStart), text, caseSensitive,
                              wholeWord, regex);
        if (hit < 0)
            hit = lastMatchBefore(docLen, text, caseSensitive, wholeWord, regex);
    }

    if (hit < 0)
        return -1;

    const long hitEnd = m_sci->SendScintilla(QsciScintillaBase::SCI_GETTARGETEND);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSEL, hit, hitEnd);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SCROLLCARET);
    emit cursorChanged();
    return int(m_sci->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, hit)) + 1;
}

long EditorViewItem::lastMatchBefore(long limit, const QString &text,
                                     bool caseSensitive, bool wholeWord,
                                     bool regex) const {
    long last = -1;
    long pos = 0;
    while (pos <= limit) {
        const long hit = searchFrom(pos, limit, text, caseSensitive, wholeWord, regex);
        if (hit < 0)
            break;
        const long hitEnd = m_sci->SendScintilla(QsciScintillaBase::SCI_GETTARGETEND);
        last = hit;
        pos = (hitEnd > hit) ? hitEnd : hit + 1;
    }
    return last;
}

int EditorViewItem::highlightMatches(const QString &text, bool caseSensitive,
                                     bool wholeWord, bool regex) {
    clearHighlights();
    if (!m_sci || !hasDocument() || text.isEmpty())
        return 0;

    const QByteArray needle = text.toUtf8();
    const long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);

    m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT,
                         long(kFindIndicator));

    int count = 0;
    long pos = 0;
    while (pos <= docLen) {
        const long hit = searchFrom(pos, docLen, text, caseSensitive, wholeWord, regex);
        if (hit < 0)
            break;
        const long hitEnd = m_sci->SendScintilla(QsciScintillaBase::SCI_GETTARGETEND);
        if (hitEnd > hit)
            m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE, hit,
                                 (unsigned long)(hitEnd - hit));
        ++count;
        pos = (hitEnd > hit) ? hitEnd : hit + 1;
    }

    m_sci->viewport()->update();
    return count;
}

void EditorViewItem::clearHighlights() {
    if (!m_sci)
        return;
    const long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    if (docLen <= 0)
        return;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT,
                         long(kFindIndicator));
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE, 0L, docLen);
}

bool EditorViewItem::replaceCurrent(const QString &text, const QString &replacement,
                                    bool caseSensitive, bool wholeWord, bool regex) {
    if (!m_sci || !hasDocument() || m_readOnly || text.isEmpty())
        return false;

    const QString selected = m_sci->selectedText();
    bool matches = !selected.isEmpty();
    if (matches) {
        if (regex)
            matches = true;  /* 正则下不比对正文，交给下一次查找 */
        else if (caseSensitive)
            matches = (selected == text);
        else
            matches = (selected.compare(text, Qt::CaseInsensitive) == 0);
    }

    if (!matches) {
        return find(text, caseSensitive, wholeWord, regex, true) >= 0;
    }

    m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACESEL,
                         replacement.toUtf8().constData());
    emit modifiedChanged();
    emit documentsChanged();
    find(text, caseSensitive, wholeWord, regex, true);
    return true;
}

int EditorViewItem::replaceAll(const QString &text, const QString &replacement,
                               bool caseSensitive, bool wholeWord, bool regex) {
    if (!m_sci || !hasDocument() || m_readOnly || text.isEmpty())
        return 0;

    const long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    QVector<QPair<long, long>> hits;
    long pos = 0;
    while (pos <= docLen) {
        const long hit = searchFrom(pos, docLen, text, caseSensitive, wholeWord, regex);
        if (hit < 0)
            break;
        const long hitEnd = m_sci->SendScintilla(QsciScintillaBase::SCI_GETTARGETEND);
        hits.append({hit, hitEnd});
        pos = (hitEnd > hit) ? hitEnd : hit + 1;
    }
    if (hits.isEmpty())
        return 0;

    const QByteArray repl = replacement.toUtf8();

    /*
     * 从后往前替换：位置从后往前不会因为前面变长变短而失效，
     * 不需要每替换一次就重算一遍全部命中。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_BEGINUNDOACTION);
    for (int i = hits.size() - 1; i >= 0; --i) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETSEL, hits.at(i).first,
                             hits.at(i).second);
        m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACESEL, repl.constData());
    }
    m_sci->SendScintilla(QsciScintillaBase::SCI_ENDUNDOACTION);

    m_sci->viewport()->update();
    emit modifiedChanged();
    emit documentsChanged();
    return hits.size();
}

void EditorViewItem::gotoLine(int line) {
    if (!m_sci || !hasDocument())
        return;
    const long total = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLINECOUNT);
    const long target = qBound(1L, (long)line, qMax(1L, total));
    m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOLINE, target - 1);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SCROLLCARET);
    emit cursorChanged();
}

/* ------------------------------------------------------------------ */
/* 视图命令                                                            */
/* ------------------------------------------------------------------ */

void EditorViewItem::updateZoomPercent() {
    if (!m_sci)
        return;
    /*
     * Scintilla 的 zoom 单位是**点**，所以百分比要拿当前字号的**点数**当分母，
     * 不是像素数；而且用"点×100"比，免得 12px→9pt 这种取整把百分比算毛。
     */
    const long zoom = m_sci->SendScintilla(QsciScintillaBase::SCI_GETZOOM);
    const int base = qMax(100, stylePointSize());
    m_zoomPercent = qRound(100.0 * double(base + zoom * 100) / double(base));
}

void EditorViewItem::zoomIn() {
    if (!m_sci)
        return;
    const long zoom = m_sci->SendScintilla(QsciScintillaBase::SCI_GETZOOM);
    if (zoom >= 30)
        return;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETZOOM, zoom + 1);
    updateZoomPercent();
    emit zoomChanged();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
}

void EditorViewItem::zoomOut() {
    if (!m_sci)
        return;
    const long zoom = m_sci->SendScintilla(QsciScintillaBase::SCI_GETZOOM);
    if (zoom <= -8)
        return;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETZOOM, zoom - 1);
    updateZoomPercent();
    emit zoomChanged();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
}

void EditorViewItem::zoomReset() {
    if (!m_sci)
        return;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETZOOM, 0L);
    updateZoomPercent();
    emit zoomChanged();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
}

void EditorViewItem::printDocument() {
    if (!m_sci || !hasDocument())
        return;

    QsciPrinter printer(QPrinter::HighResolution);
    printer.setDocName(displayName());
    printer.setColorMode(QPrinter::GrayScale);
    printer.setWrapMode(QsciScintilla::WrapWord);

    QPrintDialog dialog(&printer, m_sci);
    if (dialog.exec() != QDialog::Accepted)
        return;

    /*
     * 深色主题直接打印会是一整页黑底 —— 先把默认样式临时换成黑字白底
     * 再打印（语法高亮这一步会被抹掉，换来的是打得出来的纸），
     * 打完立刻 applyStyle() 恢复。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETFORE,
                         QsciScintillaBase::STYLE_DEFAULT,
                         static_cast<long>(0x000000));
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETBACK,
                         QsciScintillaBase::STYLE_DEFAULT,
                         static_cast<long>(0xffffff));
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLECLEARALL);

    printer.printRange(m_sci);

    applyStyle();
    m_sci->viewport()->update();
}

void EditorViewItem::requestEditorFocus() {
    if (!m_sciWidget || !m_sci)
        return;

    const auto focusEditor = [this]() {
        if (!m_sciWidget || !m_sci)
            return;
        m_sciWidget->setFocus(Qt::OtherFocusReason);
        m_sci->setFocus(Qt::OtherFocusReason);
    };

    focusEditor();

    /*
     * 再排一次（下一轮事件循环）。
     *
     * 这个函数通常是在"鼠标点击工具栏按钮"的处理里被调用的：点击事件收尾时
     * QQuickWidget 会重新拿到一次焦点，把上面这次 setFocus 覆盖掉 ——
     * 表现就是"点了新建，接着打字却打不进编辑区"。
     * 排到下一轮再落一次，就稳压这些收尾动作。
     */
    QTimer::singleShot(0, this, [focusEditor]() { focusEditor(); });
}

bool EditorViewItem::hasEditorFocus() const {
    if (!m_sci || !m_sciWidget)
        return false;
    return m_sci->hasFocus() || m_sciWidget->hasFocus()
           || (m_sci->viewport() && m_sci->viewport()->hasFocus());
}

int EditorViewItem::caretLineAlpha() const {
    if (!m_sci)
        return -1;
    return int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETCARETLINEBACKALPHA));
}

int EditorViewItem::styleSize(int style) const {
    if (!m_sci)
        return -1;
    return int(m_sci->SendScintilla(QsciScintillaBase::SCI_STYLEGETSIZEFRACTIONAL,
                                    long(style)));
}

/* 自检用：字号换算回像素（设置里写的单位），验证"12 就是 12 像素" */
double EditorViewItem::fontPixelSizeEffective() const {
    return uiFont().pointSizeF() * logicalDpiY() / 72.0;
}

/* 自检用：注释样式的字号（"点 × 100"）。没单独设时和正文字号一样 */
int EditorViewItem::styleCommentPointSize() const {
    return qRound(commentFont().pointSizeF() * 100.0);
}

/* 自检用：某个样式号用的字体名 */
QString EditorViewItem::styleFontName(int style) const {
    if (!m_sci)
        return QString();
    char buffer[128] = {0};
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLEGETFONT, long(style),
                         static_cast<void *>(buffer));
    return QString::fromUtf8(buffer);
}

int EditorViewItem::textLineHeight() const {
    if (!m_sci || !hasDocument())
        return 0;
    return m_sci->textHeight(0);
}

/* 自检用：正文区里的纯白像素数（见头文件里的说明） */
int EditorViewItem::whiteBackgroundPixels() const {
    if (!m_sci || !m_sciWidget || !hasDocument())
        return 0;

    long marginWidth = 0;
    for (long m = 0; m <= 2; ++m)
        marginWidth += m_sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINWIDTHN, m);

    const QImage img = m_sciWidget->grab().toImage();
    if (img.isNull())
        return 0;

    const qreal scale =
        m_sciWidget->width() > 0 ? qreal(img.width()) / qreal(m_sciWidget->width()) : 1.0;
    const int x0 = int(qMax(0.0, double(marginWidth + m_paddingLeft) * scale));

    int count = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = x0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c.red() >= 250 && c.green() >= 250 && c.blue() >= 250)
                ++count;
        }
    }
    return count;
}
void EditorViewItem::releaseEditorFocus() {
    /*
     * 把焦点从原生控件交还 QQuickWidget。
     *
     * 编辑控件是独立的原生子窗口，只要它拿着焦点，按键就不会进 QML
     * （查找栏打不了字）。这里清掉它的焦点，并把宿主里的 QQuickWidget
     * 设为焦点控件，QML 那边的 forceActiveFocus() 才有意义。
     */
    if (m_sci)
        m_sci->clearFocus();
    if (m_sciWidget)
        m_sciWidget->clearFocus();
    if (m_hostWidget) {
        if (auto *qw = m_hostWidget->findChild<QQuickWidget *>())
            qw->setFocus(Qt::OtherFocusReason);
    }
}
