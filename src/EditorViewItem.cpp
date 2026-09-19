#include "EditorViewItem.h"

#include "ClipboardStore.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QPair>
#include <QPainter>
#include <QHash>
#include <QPalette>
#include <QPixmap>
#include <QPolygonF>
#include <QPrintDialog>
#include <QPrinter>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QScrollBar>
#include <QStringConverter>
#include <QStyle>
#include <QTimer>
#include <QToolTip>
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
#include <utility>       /* std::as_const */

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
QVector<QPointer<EditorViewItem>> EditorViewItem::s_all;
QVector<std::weak_ptr<EditorViewItem::Doc>> EditorViewItem::s_pool;
int EditorViewItem::s_untitledCounter = 0;
int EditorViewItem::s_nextDocId = 0;

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
/* INDIC_SQUIGGLE：波浪线（IDE 里标问题那种下划线），值取自 Scintilla 5.x */
constexpr long kIndicSquiggle = 1;

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
/* 校验波浪线的两档色（错误 / 警告），和界面里那两档严重度一个色 */
const QColor kCheckError(0xff, 0x6b, 0x68);
const QColor kCheckWarn(0xd7, 0xa8, 0x5b);
/*
 * 编辑器里所有竖线的颜色：行号右边那条分隔线、字数参考线、缩进参考线，**同一个色**。
 *
 * 以前是三套色号（分隔线 #333840、字数参考线 #4b515a、缩进参考线 #3e4247），
 * 用户的要求是"这些竖线跟行号右边那条一样"，于是统一成这一个常量。
 * 比底色（#1e1f22）亮一点点：看得出层次，又不跟正文抢注意力。
 *
 * 注意：颜色统一之后，"是哪条线"只能按**位置**分辨 —— 分隔线在边距里（正文区
 * 左边），字数参考线在正文区第 N 列，缩进参考线在正文区各缩进位上。自检里的
 * 像素统计也是按位置分开数的（见 marginPixelStats / rulerPixelStats），
 * 别再写成"扫到这个颜色就是那条线"。
 */
const QColor kGuideLine(0x33, 0x38, 0x40);
/*
 * 折叠箭头两边各留多少空隙。
 *
 * 折叠栏宽度 = 箭头宽 + 2 × 这个值（见 applyFoldMarkers）—— 14px 的默认宽度
 * 在深色主题下箭头贴着行号和分隔线，看着很挤。
 */
constexpr int kFoldIconGap = 5;
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
    /* contains(..., Qt::CaseInsensitive)：不区分大小写地找，省掉 toLower() 那份拷贝 */
    return description.contains(QLatin1String("comment"), Qt::CaseInsensitive);
}

bool isKeywordDescription(const QString &description) {
    return description.contains(QLatin1String("keyword"), Qt::CaseInsensitive);
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
    /*
     * s_instance（"当前编辑器是谁"）**不在这里抢**。
     *
     * 原来写的是"谁最后构造谁就是当前编辑器"，分栏之后这条规矩就错了：
     * QML 把两个 EditorViewItem（editorView / mirrorPane）建出来的**顺序不保证**
     * 和声明顺序一致（实测过：镜像那个反而在后），于是 s_instance 会落在
     * 只读的镜像上 —— 自检 700 多项里 61 项当场变红（量到的全是那块没文档、
     * 宽高为负的镜像）。
     *
     * 现在由 QML 显式指定：主栏那份写 `mainEditor: true`（见下面的 setter），
     * 谁是主编辑器一目了然，和创建顺序无关。
     */
    s_all.append(this);

    /*
     * 两栏之间只连"提醒对方重画标签"这一路信号（见 tabsChanged）。
     *
     * 正文**不需要**同步：两栏看的是池子里同一份 Doc，底层就是同一个
     * Scintilla 文档。这里连的是"标题 / 修改标记变了""池子里多了一份"这类
     * 界面层的事 —— 对方收到就重算自己的标签栏。
     *
     * 遍历用**下标**：s_all / s_pool / m_docs 都是隐式共享的 QList，范围 for
     * 在非 const 容器上会调 begin() 而把共享**脱开**（多一次深拷贝，clazy 的
     * range-loop-detach 报的就是这个）；下标 + at() 是 const 的，一眼能看出
     * "这是只读遍历"。
     */
    for (int i = 0; i < s_all.size(); ++i) {
        EditorViewItem *other = s_all.at(i).data();
        if (!other || other == this)
            continue;
        connect(other, &EditorViewItem::tabsChanged, this,
                [this]() { refreshTabs(); }, Qt::QueuedConnection);
        connect(this, &EditorViewItem::tabsChanged, other,
                [other]() { if (other) other->refreshTabs(); }, Qt::QueuedConnection);
    }

    /*
     * 别处（另一栏）已经有文档了，而这一栏是后来才建出来的吗？
     * 不用管 —— 新栏开局是空的，池子里的东西不进它的标签栏。
     * "分栏时把当前这一份拉过来"由 QML 显式调 openPoolDocument()。
     */
}

EditorViewItem::~EditorViewItem() {
    if (s_instance == this)
        s_instance = nullptr;
    s_all.removeAll(QPointer<EditorViewItem>(this));
    detach();
}

namespace {

/*
 * 过渡期把编辑区那些原生子窗摘出屏幕（见 EditorViewItem.h 的 setAllNativeVisible）。
 *
 * 为什么是个文件级开关而不是逐个控件的状态：摘出去之后，QML 那一拍还会连着好几次
 * geometryChange → applyGeometry()，每次都"顺手把控件 show 回来"，那样这个开关
 * 一放下去下一帧就失效了。所以摆坐标照做、show 那一步统一看这个标志。
 */
bool nativeSuppressed = false;

/*
 * 摆坐标冻住了没有（见 EditorViewItem.h 的 setGeometryFrozen）。
 *
 * 和上面那个开关各管一样：nativeSuppressed 管"显不显示"，这个管"动不动位置"。
 */
bool geometryFrozen = false;

}  // namespace

void EditorViewItem::setGeometryFrozen(bool frozen)
{
    geometryFrozen = frozen;
}

/*
 * 让所有编辑器实例按当前 QML 布局重新摆一次原生控件。
 *
 * 调用点在 WindowHelper::applyState：主窗口换几何**之前**调一次 —— 这样
 * 窗口变大的那一拍，编辑器已经在目标坐标上了（它挂在宿主 QWidget 上，
 * 坐标超出旧窗口那部分是裁掉的，看不出来；窗口一变就是正好那个位置）。
 */
void EditorViewItem::syncAllGeometry() {
    /* 下标遍历（理由同上：s_all 是隐式共享的 QList，只读就别脱开它） */
    for (int i = 0; i < s_all.size(); ++i) {
        if (EditorViewItem *item = s_all.at(i).data())
            item->applyGeometry();
    }
}

void EditorViewItem::setAllNativeVisible(bool on)
{
    if (nativeSuppressed == !on)
        return;   /* 已经是目标状态：别再来一遍（hide/show 各是一次整窗重画） */

    nativeSuppressed = !on;

    if (on) {
        /* 放回去：按当前 QML 布局重摆，该显示的由 applyGeometry 自己 show */
        syncAllGeometry();
        return;
    }

    for (int i = 0; i < s_all.size(); ++i) {
        EditorViewItem *item = s_all.at(i).data();
        if (item && item->m_sciWidget)
            item->m_sciWidget->hide();
    }
}

/* ------------------------------------------------------------------ */
/* 原生控件生命周期                                                    */
/* ------------------------------------------------------------------ */

namespace {

/*
 * 编辑区最底下那一行（横向滚动条占的那一条）上的"补线"。
 *
 * 为什么需要它：两条竖线分别画在 Scintilla 的边距和正文区里，而正文区
 * （viewport）到横向滚动条上沿就截止了 —— 横条一出现，线就在离底边 12px 的地方
 * 断掉（用户报的"有时候没撑满纵向屏幕"）。Scintilla 没有画到滚动条区域的接口，
 * 所以用一个透明小控件把那一小段补上：
 *   * 只画那两条 1px 的线，别的什么都不画（WA_NoSystemBackground，也不填背景），
 *     底下的滚动条该什么样还什么样；
 *   * WA_TransparentForMouseEvents：鼠标事件全放过去，横条照样能拖。
 */
class BottomLines : public QWidget {
public:
    explicit BottomLines(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAutoFillBackground(false);
        setFocusPolicy(Qt::NoFocus);
    }

    /*
     * 要补的线：控件坐标的 x + 颜色（宽度固定 1px）。
     *
     * 不是 const（外面算完要整个赋进来，见 updateBottomLines），所以
     * paintEvent 里用**下标**遍历：QVector 是隐式共享的，范围 for 在非 const
     * 成员上会调 begin() 而把共享脱开（clazy 的 range-loop-detach）。
     */
    QVector<QPair<int, QColor>> lines;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        for (int i = 0; i < lines.size(); ++i) {
            const QPair<int, QColor> &line = lines.at(i);
            painter.fillRect(QRect(line.first, 0, 1, height()), line.second);
        }
    }
};

}  // namespace

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

    /*
     * 盯着右键事件：编辑器里那个菜单要换成 QML 那套（见 eventFilter）。
     *
     * 装在控件**和它的 viewport** 上：右键是先落到 viewport 上的（QAbstractScrollArea
     * 的规矩），而 Scintilla 自己的菜单就是从那条路上弹出来的。
     */
    m_sci->installEventFilter(this);
    if (m_sci->viewport())
        m_sci->viewport()->installEventFilter(this);

    /*
     * 两条滚动条也要盯着（见 eventFilter 里那段）。
     *
     * Qt 自带的滚动条右键菜单是浅色底 + 英文条目（"Scroll here / Left edge /
     * Page left / …"），和这个界面里其它菜单完全不是一个样子，所以在这里
     * 把它换掉。滚动条是 QAbstractScrollArea 自己创建的，只能挂事件过滤器。
     */
    if (auto *hb = m_sci->horizontalScrollBar())
        hb->installEventFilter(this);
    if (auto *vb = m_sci->verticalScrollBar())
        vb->installEventFilter(this);

    /*
     * 底下那条"补线"控件：横向滚动条出现时，用它把两条竖线补到控件底边
     * （见 BottomLines 的说明）。挂在编辑控件上、排在最后创建，所以它画在
     * 滚动条之上；又因为只画那两条线，滚动条照常看得见。
     */
    m_bottomLines = new BottomLines(m_sci);
    m_bottomLines->hide();

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
        /* 另一栏标签上那个"未保存"的点也要跟着亮 / 灭 */
        emit tabsChanged();
    });

    connect(m_sci, &QsciScintilla::textChanged, this, [this]() {
        /*
         * 正文一改（哪怕是我们自己灌进去的），上一次校验画的那几条波浪线就
         * 不作数了 —— 行号 / 列号全都会跟着挪。统一清掉，等用户再点一次校验。
         */
        if (!m_checkIssues.isEmpty())
            clearCheckIssues();
        if (m_bulkLoading)
            return;
        applyMargins();
        emit statsChanged();
        /*
         * 另一栏如果正看着**同一份文档**，它的画面也得重画（两栏是同一个
         * 底层文档，Scintilla 不会自己通知另一个视图 —— 不通知就是"在左边
         * 打字，右边那半屏还是旧的"）。走 tabsChanged 那条广播，见
         * Main.qml 里两个编辑器的 Connections。
         */
        emit tabsChanged();
    });

    connect(m_sci, &QsciScintilla::linesChanged, this, [this]() {
        applyMargins();
        emit statsChanged();
    });

    connect(m_sci, &QsciScintilla::cursorPositionChanged, this, [this](int, int) {
        /* 光标是**这一栏自己的**视图状态（另一栏可能正看着别的文件） */
        if (hasDocument())
            m_docs[currentDocSlot()].cursorPos =
                long(m_sci->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS));
        emit cursorChanged();
    });

    /*
     * 横向滚动时参考线跟着正文一起挪，底下那条补线得跟着重画；
     * 横条的显隐（范围变化）也要重排。
     */
    if (auto *hb = m_sci->horizontalScrollBar()) {
        connect(hb, &QScrollBar::valueChanged, this, [this]() { updateBottomLines(); });
        connect(hb, &QScrollBar::rangeChanged, this, [this]() { updateBottomLines(); });
    }

    connect(m_sci, &QsciScintilla::selectionChanged, this, [this]() {
        emit cursorChanged();
        emit undoStateChanged();
    });

    /* 分栏之后"用户在哪一栏干活"由 eventFilter 里的 MouseButtonPress 报（见那里） */

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
     * 现在正文句柄是**共享**的（池子里那份 Doc 用 shared_ptr 持有，
     * 每个栏的 DocRef 各拷一份 QsciDocument）—— 所以这里只把**自己**那几份
     * 壳子丢掉，池子里那份由 shared_ptr 的最后一个持有者负责（见 unbind /
     * releaseDocument 的说明）。自己这份先脱离视图，别的栏照样显示得好好的。
     *
     * lexer 的父对象就是 m_sci，随它一起销毁，所以这里只清表、不删对象。
     */
    if (m_sci) {
        m_sci->setDocument(QsciDocument());

        /* 自己那份壳子放掉（引用计数减一，不是删文档） */
        for (DocRef &r : m_docs)
            r.m_doc = QsciDocument();

        delete m_scratch;
    }

    m_docs.clear();
    m_open.clear();
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

    /*
     * 行高放在最后：自然行高是"按当前所有样式里最大的 ascent+descent"算的，
     * 换字号 / 换字体 / 装 lexer（关键字变粗体、注释变斜体）都会改这个数，
     * 所以每次刷完样式都要按倍数重算一遍额外上下空白。
     */
    applyLineSpacing();

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

    /*
     * 第 2 条边距是"紧贴正文左边的那条分隔竖线"，底色单独用分隔色。
     * 上面那轮先把三条都刷成编辑区底色，这里再把它压回来 —— 顺序不能反。
     * 关掉这个开关时（宽度 0）颜色无所谓，跟着底色走就行。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINBACKN, 2L,
                         m_gutterLine ? scColor(kGuideLine) : paper);

    /* 折叠边距自己有颜色设置（0 = 跟随默认），改成跟底色一致 */
    m_sci->setFoldMarginColors(m_paperColor, m_paperColor);

    /*
     * 缩进参考线的颜色。
     *
     * 它走的是 STYLE_INDENTGUIDE(37) 的前景色，而 STYLECLEARALL 会把它刷成
     * 正文色（#d6d7da）—— 那样竖线亮得跟正文一样抢眼。这里压成和另外两条竖线
     * 一样的 kGuideLine（用户要的就是"这几条竖线一个样"）。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETFORE,
                         QsciScintillaBase::STYLE_INDENTGUIDE,
                         scColor(kGuideLine));
    m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETBACK,
                         QsciScintillaBase::STYLE_INDENTGUIDE, paper);

    /*
     * 这里原来还配了"当前行行号高亮"的样式号和 marginStyleOffset。去掉的原因
     * 见 applyMargins()：数字边距根本不看逐行样式；换成文本边距那条路也试过，
     * 会让 QML 侧认不到编辑区对象，已一并回退。
     */
}

/*
 * 折叠箭头（细线尖括号）的位图。
 *
 * 为什么自绘，而不是用 Scintilla 的箭头标记：自带的那两个（SC_MARK_ARROW /
 * SC_MARK_ARROWDOWN）是**实心三角**（LineMarker.cpp:182 一带），画出来是 ▼ / ▶；
 * 而这一版 QScintilla 的 FoldStyle 又没有 ArrowFoldStyle（qsciscintilla.h 的
 * enum 只到 BoxedTreeFoldStyle）。想要"细线尖括号"只能自己画。
 *
 * 画好之后走 QsciScintilla::markerDefine(QPixmap, n)：Qt 版的 XPM 类是
 * "把传进来的指针当 QPixmap 收下"（XPM.cpp:26 的 reinterpret_cast），所以这条路
 * 是通的 —— 位图被拷进标记里（QPixmap 隐式共享，临时对象也没问题），画的时候在
 * 边距里居中（PlatQt.cpp:530 DrawXPM）。
 *
 * box 是位图边长（逻辑像素），尖括号画在方框正中：down = "⌄"，否则是 "›"。
 */
static QPixmap foldChevron(bool down, const QColor &colour, int box)
{
    QPixmap pm(box, box);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    QPen pen(colour);
    pen.setWidthF(1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);

    const qreal mid = box / 2.0;
    const qreal arm = box * 0.30;    // 尖括号张开的一半（横向 / 纵向各一份）
    const qreal half = arm * 0.6;    // 折点到两端的落差
    QPolygonF poly;
    if (down) {
        poly << QPointF(mid - arm, mid - half)
             << QPointF(mid, mid + half)
             << QPointF(mid + arm, mid - half);
    } else {
        poly << QPointF(mid - half, mid - arm)
             << QPointF(mid + half, mid)
             << QPointF(mid - half, mid + arm);
    }
    p.drawPolyline(poly);
    return pm;
}

/*
 * 折叠标记：向右 / 向下的细线尖括号，不带方框、不带树线，左右各留 5px。
 *
 * setFolding() 该做的都做了（边距类型 = SC_MARGIN_SYMBOL、掩码 = SC_MASK_FOLDERS、
 * 可点击、7 个折叠标记号都配好），紧随其后把这些标记号换成自己要的样子：
 *
 *   FOLDER / FOLDEREND          折叠着的那一行 -> "›"
 *   FOLDEROPEN / FOLDEROPENMID  展开着的那一行 -> "⌄"
 *   FOLDERSUB / FOLDERTAIL / FOLDERMIDTAIL  折块内部的树线 -> 空白
 *
 * 位图标记的颜色是画进位图里的（跟着箭头的颜色走），所以不用再设
 * SCI_MARKERSETFORE / BACK。
 */
void EditorViewItem::applyFoldMarkers() {
    if (!m_sci)
        return;

    const int box = foldIconSize();
    const QPixmap closed = foldChevron(false, QColor(0x9a, 0xa0, 0xa8), box);
    const QPixmap open = foldChevron(true, QColor(0x9a, 0xa0, 0xa8), box);

    const struct {
        int mark;
        long shape;
    } kPlainShapes[] = {
        {QsciScintillaBase::SC_MARKNUM_FOLDERSUB,     QsciScintillaBase::SC_MARK_EMPTY},
        {QsciScintillaBase::SC_MARKNUM_FOLDERTAIL,    QsciScintillaBase::SC_MARK_EMPTY},
        {QsciScintillaBase::SC_MARKNUM_FOLDERMIDTAIL, QsciScintillaBase::SC_MARK_EMPTY},
    };
    for (const auto &s : kPlainShapes)
        m_sci->SendScintilla(QsciScintillaBase::SCI_MARKERDEFINE, long(s.mark), s.shape);

    m_sci->markerDefine(closed, QsciScintillaBase::SC_MARKNUM_FOLDER);
    m_sci->markerDefine(closed, QsciScintillaBase::SC_MARKNUM_FOLDEREND);
    m_sci->markerDefine(open, QsciScintillaBase::SC_MARKNUM_FOLDEROPEN);
    m_sci->markerDefine(open, QsciScintillaBase::SC_MARKNUM_FOLDEROPENMID);

    /*
     * 边距宽度 = 箭头宽 + 左右各 5px。
     *
     * 位图标记是**居中**画的（PlatQt.cpp:530：x = rc.left + (rc.Width() - 位图宽)/2），
     * 所以边距比位图宽出 10px，箭头两边就各是 5px。位图边长跟着字号走
     * （见 foldIconSize），换字号时这里跟着重算。
     */
    if (m_folding)
        m_sci->setMarginWidth(1, box + 2 * kFoldIconGap);
}

int EditorViewItem::foldIconSize() const {
    /*
     * 12px 字号 → 10px 的尖括号；字号放大 / 缩小它跟着走，夹在 8~16 之间
     * （再小看不清，再大就比行高还高了）。
     */
    return qBound(8, qRound(m_fontPixelSize * 0.8), 16);
}

QVariantList EditorViewItem::foldIconPixelStats() const {
    const int box = foldIconSize();
    const QImage closed = foldChevron(false, QColor(0x9a, 0xa0, 0xa8), box).toImage();
    const QImage open = foldChevron(true, QColor(0x9a, 0xa0, 0xa8), box).toImage();

    /* 量墨迹的包围盒（透明底上 alpha > 0 的像素） */
    auto inkBounds = [](const QImage &img) {
        int minX = img.width(), maxX = -1, minY = img.height(), maxY = -1;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                if (qAlpha(img.pixel(x, y)) > 0) {
                    minX = qMin(minX, x);
                    maxX = qMax(maxX, x);
                    minY = qMin(minY, y);
                    maxY = qMax(maxY, y);
                }
            }
        }
        if (maxX < 0)
            return QPair<int, int>(0, 0);
        return QPair<int, int>(maxX - minX + 1, maxY - minY + 1);
    };

    const QPair<int, int> c = inkBounds(closed);
    const QPair<int, int> o = inkBounds(open);
    return QVariantList{c.first, c.second, o.first, o.second};
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
    QVariantList out{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
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

    /*
     * 三条边距从左到右：0 行号 / 1 折叠 / 2 分隔线。
     * 所以 [numbersEnd, foldEnd) 是折叠栏，[foldEnd, gutterEnd) 是那条分隔线，
     * 正文从 gutterEnd 开始。
     */
    const int mw0 = int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINWIDTHN, 0L) * scale);
    const int mw1 = int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINWIDTHN, 1L) * scale);
    const int mw2 = int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINWIDTHN, 2L) * scale);
    const int numbersEnd = qMin(img.width(), mw0);
    const int foldEnd = qMin(img.width(), numbersEnd + mw1);
    const int gutterEnd = qMin(img.width(), foldEnd + mw2);

    const QColor paper = m_paperColor;

    /*
     * 缩进参考线的墨迹：只数**正文区**里那些"竖线色"的像素。
     *
     * 三条竖线同一个颜色（见 kGuideLine），所以不能"扫到这个颜色就算"，得按位置分：
     *   * 分隔线在 [foldEnd, gutterEnd)，那是边距，不算缩进参考线；
     *   * 字数参考线是正文区里**通到底**的那一列（整幅图高都亮），单独排掉 ——
     *     不然"关掉缩进参考线后一个像素都没有"那条断言会被它顶红。
     */
    int numberInk = 0, foldInk = 0, white = 0, guideInk = 0;
    const int fullHeight = qMax(1, img.height() * 4 / 5);   // 通到底的判据：8 成高
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < numbersEnd; ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c == QColor(Qt::white))
                ++white;
            else if (qAbs(c.red() - paper.red()) + qAbs(c.green() - paper.green())
                         + qAbs(c.blue() - paper.blue()) > 90)
                ++numberInk;
        }
        for (int x = numbersEnd; x < foldEnd; ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c == QColor(Qt::white))
                ++white;
            else if (qAbs(c.red() - paper.red()) + qAbs(c.green() - paper.green())
                         + qAbs(c.blue() - paper.blue()) > 90)
                ++foldInk;
        }
    }
    for (int x = gutterEnd + 1; x < img.width(); ++x) {
        int ink = 0;
        for (int y = 0; y < img.height(); ++y) {
            if (img.pixelColor(x, y) == kGuideLine)
                ++ink;
        }
        if (ink > 0 && ink < fullHeight)   // 通到底的那一列是字数参考线，不算
            guideInk += ink;
    }

    /*
     * 那条分隔竖线：在**边距那一带**（正文区左边）扫它的颜色，而不是整幅图扫。
     * 三条竖线颜色一样之后，整幅图扫会把缩进参考线 / 字数参考线一起数进来。
     * 边距宽度是逻辑像素、抓图是设备像素，高 DPI 下两者差一个缩放系数，
     * 所以这里按"正文区左边"当上界，不去抠那 1 像素。
     */
    int gutterInk = 0, gutterX = -1, gutterMaxY = -1;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x <= gutterEnd; ++x) {
            if (img.pixelColor(x, y) == kGuideLine) {
                ++gutterInk;
                if (gutterX < 0)
                    gutterX = x;
                gutterMaxY = y;
            }
        }
    }

    /*
     * 分隔线右边缘到"正文区第一个墨迹列"的距离（设备像素）。
     *
     * 线和正文之间还有没有空档全看这个数：折叠栏（14px）原来夹在中间，加上
     * 左留白，线和字之间空出 26px（用户圈着那段空白提过）。现在折叠栏挪到线的
     * 左边，这个距离应该只剩左留白那几像素。
     * 扫的是"任何不是底色、不是分隔线、不是缩进参考线的像素" —— 正文第一个
     * 字形（有左侧留白，所以会差一两像素）。
     */
    int textInkX = -1;
    if (gutterX >= 0) {
        for (int x = gutterX + 2; x < img.width() && textInkX < 0; ++x) {
            for (int y = 0; y < img.height(); ++y) {
                const QColor c = img.pixelColor(x, y);
                if (c == paper || c == kGuideLine)
                    continue;
                textInkX = x;
                break;
            }
        }
    }

    out[0] = numberInk;
    out[1] = foldInk;
    out[2] = white;
    out[3] = gutterInk;
    out[7] = mw0;
    out[8] = mw1;
    out[9] = mw2;
    out[10] = gutterX;
    out[11] = (textInkX >= 0 && gutterX >= 0) ? textInkX - (gutterX + 1) : -1;
    out[12] = guideInk;
    /* 分隔线最低那一点离控件底边还有几像素（0 = 补到底了；横条出现时靠补线） */
    out[13] = (gutterMaxY >= 0) ? (img.height() - 1 - gutterMaxY) : -1;
    return out;
}

/*
 * 自检用：编辑区底边那块补线控件的状态（见 BottomLines / updateBottomLines）。
 *   [0] 可见   [1] 鼠标穿透（横条还能拖）   [2] 不画背景（滚动条还看得见）
 *   [3] 自动填背景   [4] 补了几条线   [5] 控件高度
 *
 * 为什么量属性而不是量像素：横条那一行是滚动条控件自己画的（stylesheet 里背景是
 * transparent，靠"父控件已经画过的内容"透出来），控件 grab() 出来的图里那一条
 * 本来就是没画过的底色，量它是量不准的。
 */
QVariantList EditorViewItem::bottomLinesState() const {
    QVariantList out{false, false, false, true, 0, 0};
    auto *overlay = static_cast<BottomLines *>(m_bottomLines.data());
    if (!overlay)
        return out;

    out[0] = overlay->isVisible();
    out[1] = overlay->testAttribute(Qt::WA_TransparentForMouseEvents);
    out[2] = overlay->testAttribute(Qt::WA_NoSystemBackground);
    out[3] = overlay->autoFillBackground();
    out[4] = overlay->lines.size();
    out[5] = overlay->height();
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
     *
     * 轨道（groove）的颜色也要**写死**，不能写 transparent。
     *
     * transparent 是"这一层不画"，露出来的是底下那一层 —— 而滚动条是
     * QAbstractScrollArea 的子控件，它那块底由谁画、画成什么色是平台/样式
     * 说了算。滑块一直是深色的（样式表确实生效了），轨道却在有的机器上露出
     * 一条 12px 的 #f2f2f2 白带（用户报的"这个滚动条白色背景去掉"就是它）。
     * 所以 %3 直接把底色刷上去，不再指望别人。
     */
    const QString bar = QStringLiteral(
        "QScrollBar:vertical {\n"
        "    background: %3;\n"
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
        "    background: %3;\n"
        "}\n"
        "QScrollBar:horizontal {\n"
        "    background: %3;\n"
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
        "    background: %3;\n"
        "}\n")
        .arg(QStringLiteral("#4b4d4f"),          // 滑块
             QStringLiteral("#5f6266"),          // 悬停
             m_paperColor.name());               // 轨道（= 正文底色）

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
     * 校验结果的波浪线（错误一条红的、警告一条黄的，见 setCheckIssues）。
     *
     * 和上面那条一样是容器指示器（8 起），所以要配 SCI_INDICSETUNDER；
     * 波浪线的颜色就是 INDICSETFORE 那个色，Alpha 对它不起作用。
     */
    for (const QPair<int, QColor> &pair :
         { QPair<int, QColor>(kCheckErrorIndicator, kCheckError),
           QPair<int, QColor>(kCheckWarnIndicator, kCheckWarn) }) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE, long(pair.first),
                             long(kIndicSquiggle));
        m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETUNDER, long(pair.first), 1L);
        m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE, long(pair.first),
                             scColor(pair.second));
    }

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
     * 代码折叠：第 1 列做成折叠边距（具体设置在 applyMargins 里）。
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

    /*
     * 两条竖线：
     *   * 字数参考线（第 N 个字那条）—— view 级的 edge 设置，只有开关 / 列号
     *     变了才需要重发，所以平时不在这里刷（applyViewOptions 只在构造和
     *     这些开关联动时被调用）；
     *   * 行号栏右侧那条分隔线 —— 它是边距宽度 + 底色，跟着 applyMargins 走
     *     （见那里和 applyMarginTheme）。
     */
    applyRuler();

    applyMargins();
}

void EditorViewItem::applyMargins() {
    if (!m_sci)
        return;

    const int lines = qMax(1, lineCount());
    const int digits = QString::number(lines).size() + 1;
    const QFontMetrics fm(uiFont());

    /*
     * 行号栏宽度 = 一点左边空档 + 位数 × 字符宽。
     *
     * 位数写「实际位数 + 1」：多预留一位，行数从 9 涨到 10、99 涨到 100 时
     * 栏宽不跳，正文不会跟着挪一下（跳变只发生在跨过预留的那一位时）。
     *
     * 前面那个常数原来是 10，现在收到 4：行号要贴着卡片左边缘（见
     * EditorArea 的 cardLeftInset），这一段空档直接决定行号离左边有多远。
     * 数字在栏里是**右对齐**画的（MarginView.cpp：xpos = 栏右边 -
     * 数字宽 - marginNumberPadding），所以实际看到的左边空档 =
     * 这个常数 + marginNumberPadding 里那 3px + 没用到的那几位（每位约 7px）。
     * 实测（48 行的文件）：两位数行号的墨点左边缘离编辑器左边缘约 10px。
     */
    const int width = 4 + digits * fm.horizontalAdvance(QLatin1Char('9'));

    m_sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINWIDTHN, 0,
                         m_lineNumbers ? long(width) : 0L);

    /*
     * 代码折叠：第 1 列做成折叠边距（行号 = 第 0 列，分隔线 = 第 2 列）。
     *
     * 用 QScintilla 现成的 BoxedTreeFoldStyle —— 它把这条边距设成
     * SC_MARGIN_SYMBOL + SC_MASK_FOLDERS + 敏感（点击即可折叠），并配好
     * "方框 + 竖线"那套树形标记（和主流编辑器观感一致）。
     * 折叠层级是 lexer 着色时算出来的：QScintilla 的 setLexer() 会给文档设置
     * fold=1 属性，所以换了语言（装了 lexer）就有折叠点。
     *
     * 为什么折叠栏在**分隔线左边**（以前在右边）：折叠栏是 14px 宽的一列，
     * 摆在分隔线和正文之间，线和正文之间就空出那 14px，看着像"线飘在沟里"
     * （用户圈着那段空白提过）。挪到行号和分隔线之间之后，顺序就是
     *     行号 | 折叠箭头 | 分隔线 | 正文
     * —— 和 VS Code 的行号栏一样：折叠加号在数字右边，分隔线紧贴正文。
     */
    if (m_folding)
        m_sci->setFolding(QsciScintilla::BoxedTreeFoldStyle, 1);
    else
        m_sci->setFolding(QsciScintilla::NoFoldStyle, 1);

    /*
     * 第 2 条边距：行号栏右侧、紧贴正文左边的那条分隔竖线。
     *
     * 留 1 像素宽、背景刷成分隔色（见 applyMarginTheme），就得到一条从顶到底
     * 的竖线。不用在 QML 里按边距宽度贴一个 Rectangle：边距宽度是随行号位数、
     * 字体、折叠开关变的，QML 那边算不准；而且编辑区是原生子窗口，QML 的浮层
     * 本来就盖不到它上面。
     *
     * 它**必须排在最后一条边距**：正文左边缘 = 各条边距宽度之和 + 左留白
     * （SCI_SETMARGINLEFT），线要贴着正文，就只能放在正文的紧左边。
     *
     * 这条边距以前放过"当前行蓝色竖条"（3px 符号边距 + SC_MARK_FULLRECT），
     * 已按使用意见去掉 —— 当前行有正文那层底色加光标就够醒目了。
     *
     * 类型必须是 **SC_MARGIN_COLOUR**，不能留默认的符号边距：Scintilla 画边距
     * 背景是按类型分支的（MarginView.cpp:205 一带）——
     *   SC_MARGIN_BACK / FORE  -> 取正文样式的前/底色
     *   SC_MARGIN_COLOUR       -> 取这条边距自己的底色（SCI_SETMARGINBACKN）
     *   其余（含默认的符号边距）-> 取 **STYLE_LINENUMBER** 的底色
     * 一开始就是这么写的：边距宽 1px、底色设了，画出来却跟底色一样（实测抓图里
     * 那一条 0 个分隔色像素）—— 因为默认类型的底色根本不看 SCI_SETMARGINBACKN。
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
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINTYPEN, 2L,
                         QsciScintillaBase::SC_MARGIN_COLOUR);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETMARGINWIDTHN, 2L,
                         m_gutterLine ? 1L : 0L);

    /* 颜色最后压：装 lexer 时那次 STYLECLEARALL 会把行号样式刷回白底 */
    applyFoldMarkers();
    applyMarginTheme();

    /* 边距宽度变了 → 底边那条补线的位置也得跟着挪 */
    updateBottomLines();
}

void EditorViewItem::applyLanguageLexer() {
    if (!m_sci)
        return;

    const QString lang = hasDocument() ? m_docs.at(currentDocSlot()).doc->language
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

/*
 * Scintilla 眼里的“一页文本宽” —— 也正是它判横向滚动条显隐用的那个数。
 *
 * 出处（third/qscintilla/src/ScintillaQt.cpp 的 ModifyScrollBars）：
 *     int hNewPage = GetTextRectangle().Width();
 *     hMax = (scrollWidth > hNewPage) ? scrollWidth - hNewPage : 0;
 * 横条是 AsNeeded 策略，"hMax > 0 就露出来"。
 *
 * 优先直接读横条自己的 pageStep：那正是上面那个 hNewPage，每次 SetScrollBars
 * 都会写进去。滚动条还没摆过（pageStep 还是 QAbstractSlider 的默认值 10）时
 * 按 Scintilla 的公式兜个底：
 *     GetTextRectangle().Width() = viewport 宽 - fixedColumnWidth - 右留白
 *     fixedColumnWidth = 左留白(SCI_SETMARGINLEFT) + 各条边距宽度之和
 *                       （ViewStyle::CalculateMarginWidthAndMask）
 * 注意它**比 viewport 窄**：差的是行号/折叠那几条边距 + 左右留白。
 */
long EditorViewItem::horizontalPageWidth() const {
    if (!m_sci)
        return 0;

    if (auto *hb = m_sci->horizontalScrollBar()) {
        const int step = hb->pageStep();
        if (step > 1)
            return step;
    }

    const long viewWidth = m_sci->viewport() ? m_sci->viewport()->width() : 0;
    if (viewWidth <= 1)
        return 0;

    long fixed = m_paddingLeft + m_paddingRight;
    for (int m = 0; m < 3; ++m) {   // 0 行号 / 1 空 / 2 折叠
        const int w = marginWidth(m);
        if (w > 0)
            fixed += w;
    }
    return qMax(1L, viewWidth - fixed);
}

void EditorViewItem::updateHorizontalScroll() {
    if (!m_sci)
        return;

    /*
     * 只有内容真的放不下时才显示横向滚动条。
     *
     * 为什么不能偷懒：
     *   * Scintilla 的 scrollWidth 默认是 2000 像素（Editor.cpp:155），
     *     横向范围 = scrollWidth - 一页宽，只要一页比 2000 窄就恒 > 0，
     *     于是短内容也一直挂着横条；
     *   * SCI_SETSCROLLWIDTH 要求 wParam > 0（Editor.cpp:6659），
     *     传 0 无效；
     *   * SCI_SETHSCROLLBAR 0 是永久关闭，长内容也滚不了。
     *
     * 做法：**实际量**最长行的像素宽度。
     *   1) 先按字符数找最长行（只比长度，很便宜）；
     *   2) 只对那一行量一次实际像素宽度；
     *   3) scrollWidth 就设成这个内容宽度 —— 放得下时它比一页窄（hMax = 0，
     *      横条隐藏），超了时它比一页宽（hMax > 0，横条出现），两件事都由
     *      Scintilla 那一次减法判，这里不再自己比"放不放得下"。
     *      **别**改成"放得下时设成一页宽"：那样 scrollWidth 就把"当时面板多宽"
     *      记了进去，拖动分栏把它变窄的那一拍会闪出横条，见下面那段说明。
     *
     * 注意不能用"字符数 × 字符宽 × 系数"估算：那个系数会多算一截，
     * 结果就是横条出现、还能向右滚正好多算的那些像素（实测过）。
     *
     * 也别拿 **viewport 宽度**当"放得下"的界 —— 这里踩过坑：
     * 短内容时把 scrollWidth 设成 viewport 宽，它仍然比 hNewPage
     * （GetTextRectangle().Width()）大一截，多出来的正好是行号栏 + 左右留白
     * 那几十像素：hMax 恒 > 0，于是"正文只有几个字，横条却一直在，还能向右滚
     * 一点点"（用户报的就是这个）。判据要用同一个口径，见 horizontalPageWidth()。
     */
    const long pageWidth = horizontalPageWidth();

    /* 自动换行时内容折起来，永远不超过一页宽，横条恒隐藏
       （Scintilla 自己也会把横条策略设成 AlwaysOff） */
    if (m_wrap) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTH,
                             (unsigned long)qMax(1L, pageWidth));
        return;
    }

    if (pageWidth <= 1)
        return;

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
            /*
             * 末尾那 8px 是"量出来的"和"画出来的"之间的余量（斜体出格、字体回退
             * 之类的零头）。留着它是为了让"其实差一点点"的长行仍然出现横条 ——
             * 横条该多出现一次，也不能让正文尾巴够不着。反过来多出来的这点余量
             * 只有 8px，撑不满屏幕的内容不会因为它挂上横条。
             */
            contentWidth = (long)fm.horizontalAdvance(line) + 8;
        }
    }
    m_lastContentWidth = contentWidth;

    /*
     * scrollWidth 只由**内容宽度**决定 —— 放得下时它天然比一页窄，范围就是 0。
     *
     * 为什么不能"放得下就写成一页文本宽"（原来就是这么写的）：那样这个值顺手
     * 把"设它的那一刻面板有多宽"也记了进去。拖动分栏分隔线 / 拉窗口把它变窄的
     * **那一拍**，Scintilla 那边
     *     hMax = scrollWidth - 新的 hNewPage
     * 就成了正数（见 third/qscintilla/src/ScintillaQt.cpp 的 ModifyScrollBars），
     * 横条当场冒出来闪一下，等下一拍 updateHorizontalScroll() 才压回去 ——
     * 用户报的"拖分隔线时底下横条一闪一闪、正文根本没超出屏幕"就是这个。
     *
     * 换成内容宽度之后，这个值跟面板宽度无关：变宽变窄都不动它，"够不够放"
     * 全交给 Scintilla 那次减法判。拖动过程中它一直是同一个数，横条自然不闪。
     *
     * 一个字都没有时给 1：SCI_SETSCROLLWIDTH 要求 wParam > 0（Editor.cpp:6659），
     * 而 hNewPage 至少也有几十像素，hMax 仍然是 0。
     */
    const long scrollWidth = qMax(1L, contentWidth);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTH,
                         (unsigned long)scrollWidth);

    /*
     * 横条可能刚出现 / 刚收回去 —— 那一条的高度变了，补线要跟着排。
     * 横条的显隐是 Scintilla 在 SetScrollBars 里改的，几何要等这一轮事件处理完
     * 才落定，所以再排一次到下一轮。
     */
    updateBottomLines();
    QTimer::singleShot(0, this, [this]() { updateBottomLines(); });
}

/* ------------------------------------------------------------------ */
/* 几何                                                                */
/* ------------------------------------------------------------------ */

/*
 * 把两条竖线补到编辑控件的最底边（横向滚动条那一条）。
 *
 * 两种情况下这一条是空的：横条没出现（正文区一直铺到底）、或者控件还没布局。
 * 其余情况就按当前几何算两条线的 x，交给 BottomLines 画。要重算的时机：
 * 改窗口大小、横条出现/消失、横向滚动（参考线的 x 跟着挪）、改列号 / 边距 / 字号。
 */
void EditorViewItem::updateBottomLines() {
    auto *overlay = static_cast<BottomLines *>(m_bottomLines.data());
    if (!m_sci || !overlay)
        return;

    if (!m_sci->isVisible()) {
        overlay->hide();
        return;
    }

    /*
     * 正文区（viewport）下面的那一条，就是横向滚动条占的那一行。
     * 横条没出现时 viewport 一直铺到控件底边，这条高度是 0，没什么可补的。
     */
    const QRect vp = m_sci->viewport()->geometry();
    const int top = vp.bottom() + 1;
    const int height = m_sci->height() - top;
    if (top <= 0 || height <= 0 || m_sci->width() <= 0) {
        overlay->hide();
        return;
    }

    const int m0 = marginWidth(0);
    const int m1 = marginWidth(1);
    const int m2 = marginWidth(2);
    const int textStart = m0 + m1 + m2 + m_paddingLeft;

    QVector<QPair<int, QColor>> lines;

    /* 行号右边那条分隔线：它在 [m0 + m1, +m2) 那一条边距上 */
    if (m_gutterLine && m0 >= 0 && m1 >= 0) {
        const int x = m0 + m1;
        if (x < m_sci->width())
            lines.append({x, kGuideLine});
    }

    /*
     * 字数参考线：Scintilla 画 edge 的公式（x = 列号 × 空格宽 + 正文左边缘，
     * 横滚时整体左移），见 rulerPixelStats 里的同一份算法。线只画在正文区里，
     * 所以算出来落在正文左边缘左边（横滚滚出去了）就不补。
     */
    if (m_rulerVisible) {
        const long xOffset =
            m_sci->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET);
        const QFontMetrics fm(uiFont());
        const int x = textStart - int(xOffset)
                      + m_rulerColumn * fm.horizontalAdvance(QLatin1Char(' '));
        if (x >= textStart && x < m_sci->width())
            lines.append({x, kGuideLine});
    }

    overlay->lines = lines;
    overlay->setGeometry(0, top, m_sci->width(), height);
    overlay->raise();
    overlay->show();
    overlay->update();

    /*
     * 等一拍再抬一次。
     *
     * 横条"由隐变显"的时候，QAbstractScrollArea 自己会重排一次（layoutChildren），
     * 顺手把两条滚动条抬到最上层 —— 正好压在补线控件头上。以前滚动条的轨道是
     * transparent（这一层不画），抬上去也看不出底下的线被盖住，所以没露馅；
     * 现在轨道刷了实打实的底色（见 styleChrome 里那条说明），就会盖住。
     * 布局是这一轮事件之后才落定的，所以下一拍再抬一次才稳。
     */
    QTimer::singleShot(0, this, [this]() {
        if (auto *o = static_cast<BottomLines *>(m_bottomLines.data()))
            o->raise();
    });
}

/*
 * 盯住**所有祖先**的位置 / 尺寸变化，一有就重摆原生控件。
 *
 * 原生子窗口是按**场景坐标**摆的（applyGeometry 把 item 的场景位置换算成宿主
 * 控件坐标），而 item 自己的 geometryChange 只在它相对父项的几何变化时才发：
 * 分栏那两个占位壳（mainPane / mirrorPaneHolder）**整体挪位置、尺寸不变**时
 * （上下分栏布局落定那一下，下面那一栏的 y 变、高不变）它不发 ——
 * 原生子窗口就停在旧位置，压住中间那条分隔线，屏幕上看着就是"没分开"
 * （用户报的"上下分栏没有展开"）。
 *
 * 祖先的 x / y / width / height 都是现成的 NOTIFY 信号，接上就是同步的，
 * 不落后任何一拍。链子很短（占位壳 → 正文卡片 → 编辑区根），接一次就够；
 * 父项换了（ItemParentHasChanged）再走一遍。
 */
void EditorViewItem::watchAncestorGeometry() {
    for (QQuickItem *p = parentItem(); p; p = p->parentItem()) {
        if (m_watchedAncestors.contains(p))
            continue;
        connect(p, &QQuickItem::xChanged, this, &EditorViewItem::applyGeometry);
        connect(p, &QQuickItem::yChanged, this, &EditorViewItem::applyGeometry);
        connect(p, &QQuickItem::widthChanged, this, &EditorViewItem::applyGeometry);
        connect(p, &QQuickItem::heightChanged, this, &EditorViewItem::applyGeometry);
        m_watchedAncestors.append(p);
    }
}

void EditorViewItem::applyGeometry() {
    if (!m_sci || !m_sciWidget || !m_hostWidget || !window())
        return;

    /* 尺寸还没定下来就先不动 */
    if (width() < 2 || height() < 2)
        return;

    /* 预热那一段里别跟着 QML 撑大（见 EditorViewItem.h 的 setGeometryFrozen） */
    if (geometryFrozen)
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

    /* 过渡期里坐标照摆，但先别放回屏幕（见文件上面那个开关的说明） */
    if (!nativeSuppressed && !m_sciWidget->isVisible())
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

    /* 几何变了，底下那条补线也得跟着重排（见 updateBottomLines） */
    updateBottomLines();
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

/*
 * 编辑区右键：把 Scintilla 自带那个菜单换成 QML 的下拉菜单。
 *
 * 为什么不用 Scintilla 那个（原来的样子）：它是 QtWidgets 的 QsciSciPopup，
 * 英文条目（Undo / Redo / Cut…）、QtWidgets 样式画出来的观感，和整个深色
 * QML 界面是两套东西（用户截图报过两轮）。QML 那边本来就有一份"编辑"菜单
 * （qml/components/DropdownMenu.qml + js/EditorMenus.js 的 editMenu），
 * 条目、图标、快捷键、可用状态都是现成的 —— 直接拿它当右键菜单，
 * 观感和菜单栏那个"编辑"一模一样。
 *
 * 拦截点：右键事件先落到编辑控件的 viewport 上（QAbstractScrollArea 的规矩），
 * Scintilla 就是在那儿把自己的菜单弹出来的。事件过滤器比控件自己的事件处理
 * **先**跑，所以在这里吃掉它、改发一个信号给 QML 就行。
 *
 * 坐标：先换算成控件局部坐标（控件正好铺在这个 Item 上，两者原点相同），
 * 再交给 Qt Quick 的 mapToItem 换成场景坐标 —— 场景坐标就是 QML 窗口内容区
 * 坐标，和 Popup 的 x/y 同一套（DropdownMenu.openAtPoint 要的就是它）。
 */
bool EditorViewItem::eventFilter(QObject *watched, QEvent *event) {
    /*
     * 滚动条自己冒出来 / 改尺寸之后，把底下那块补线控件重新抬到它上面。
     *
     * 原因见 updateBottomLines 末尾：QAbstractScrollArea 重排时会把滚动条
     * 抬到最上层，横条出现的这一下正好盖住补线控件画的那两条线。
     */
    if (qobject_cast<QScrollBar *>(watched)) {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::Resize:
            QTimer::singleShot(0, this, [this]() { updateBottomLines(); });
            break;
        default:
            break;
        }
    }

    /*
     * 鼠标按下 / 键盘焦点进来 = "用户在这一栏里干活"（分栏之后命令发给谁就看它）。
     *
     * 用 eventFilter 接 MouseButtonPress，不用 Scintilla 的信号：后者没有
     * "被点了一下"这种东西，而且点在空白处（光标没动）时 cursorPositionChanged
     * 也不会发。这里**不吃掉**事件（返回 false 的走法在下面统一处理）。
     */
    if (event->type() == QEvent::MouseButtonPress && watched == m_sci && m_sci
        && isVisible())
        emit paneFocused();

    /*
     * 鼠标停在**校验波浪线**上：弹一个说明框（和 IDE 里把鼠标移到出错的地方一样）。
     *
     * 走的 Qt 那套悬浮事件：鼠标停住一会儿之后，Qt 会把 ToolTip 事件发给光标下
     * 那个控件，过滤器比控件自己先拿到它。停的位置换算成文档位置，看它落在哪条
     * 问题上（见 checkIssueAt），有就弹详情、没有就把上一个收掉
     * —— 鼠标从波浪线上挪开时那个框不该赖着不走。
     *
     * 提示框用 QToolTip：深色底是 main.cpp 里设的全局调色板给的，和界面一致。
     */
    if (event->type() == QEvent::ToolTip && m_sci
        && (watched == m_sci || watched == m_sci->viewport())) {
        auto *he = static_cast<QHelpEvent *>(event);
        const QPoint inView = m_sci->viewport()->mapFromGlobal(he->globalPos());
        const QString tip = checkTipAtPoint(inView.x(), inView.y());
        if (!tip.isEmpty())
            QToolTip::showText(he->globalPos(), tip, m_sci);
        else
            QToolTip::hideText();
        return true;
    }

    if (event->type() == QEvent::ContextMenu && m_sci && isVisible() && isEnabled()) {
        auto *ce = static_cast<QContextMenuEvent *>(event);

        /*
         * 滚动条上的右键：换成 QML 那套菜单（和编辑区里的右键同一个组件）。
         *
         * 这里必须**吃掉**事件（返回 true）：Qt 的 QScrollBar::contextMenuEvent()
         * 就是从这个事件里弹它自己那个菜单的，放行过去就两套菜单一起出来了。
         *
         * "滚动到这里"要的那个值也在这里算好：右键点在滚动条上的哪个比例，
         * 就滚到哪个位置 —— 和 Qt 自带菜单的口径一致（QStyle::sliderValueFromPosition）。
         */
        if (auto *bar = qobject_cast<QScrollBar *>(watched)) {
            const QPoint inBar = bar->mapFromGlobal(ce->globalPos());
            const bool horizontal = bar->orientation() == Qt::Horizontal;
            const int span = horizontal ? bar->width() : bar->height();
            const int pos = horizontal ? inBar.x() : inBar.y();
            m_scrollMenuValue = QStyle::sliderValueFromPosition(bar->minimum(), bar->maximum(),
                                                               pos, span);
            m_scrollMenuHandled = true;

            const QPointF inSci = m_sci->mapFromGlobal(ce->globalPos());
            const QPointF scene = mapToItem(nullptr, inSci);
            emit scrollBarContextMenuRequested(horizontal, scene.x(), scene.y());
            return true;
        }

        const QPointF inItem = m_sci->mapFromGlobal(ce->globalPos());
        const QPointF scene = mapToItem(nullptr, inItem);
        emit contextMenuRequested(scene.x(), scene.y());
        return true;    /* 吃掉：别让 Scintilla 再弹它自己那个原生菜单 */
    }

    /*
     * 键盘上的复制 / 剪切。
     *
     * 这两条是 Scintilla 自己在 keyPressEvent 里处理的，不经过 cut() / copy()
     * 那两个包装函数，所以"这次剪贴板变化是我们自己造成的"要在这里补一次 ——
     * 否则用户在编辑区里 Ctrl+C 一段正文，几毫秒后它又作为"新剪贴板内容"
     * 被采集进当天的 md 里。
     *
     * 只**真有选区**时才置标记：按了 Ctrl+C 而没有选中任何东西是个空操作，
     * 剪贴板根本不会变，那个标记就会一直挂到下一次外部复制上（标记本身
     * 也有 2 秒有效期兜底，见 ClipboardStore::takeSkipNextCapture）。
     */
    if (event->type() == QEvent::KeyPress && m_store && watched == m_sci
        && m_sci->hasSelectedText()) {
        auto *ke = static_cast<QKeyEvent *>(event);
        const Qt::KeyboardModifiers mods = ke->modifiers();
        const int key = ke->key();
        const bool ctrl = mods.testFlag(Qt::ControlModifier);
        const bool shift = mods.testFlag(Qt::ShiftModifier);
        if ((ctrl && (key == Qt::Key_C || key == Qt::Key_X || key == Qt::Key_Insert))
            || (shift && key == Qt::Key_Delete))
            m_store->markOwnCopy();
    }

    return QQuickItem::eventFilter(watched, event);
}

void EditorViewItem::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickItem::itemChange(change, value);

    switch (change) {
    case ItemSceneChange:
        if (value.window) {
            ensureWrapped();
            watchAncestorGeometry();
            applyGeometry();
        } else if (m_sciWidget) {
            m_sciWidget->hide();
        }
        break;
    /*
     * 场景位置变了也要重新摆那块原生控件。
     *
     * 为什么 geometryChange 不够：那个只在**本 item 自己**的几何（相对父项）
     * 变的时候才发。分栏之后两栏各自挂在一个占位壳里（mainPane /
     * mirrorPaneHolder），壳**只挪位置、尺寸不变**时（上下分栏那次布局落定：
     * 下面那一栏的 y 从中间值收到最终值），壳里的 item 相对坐标一点没变 ——
     * geometryChange 不发，原生子窗口就停在旧位置上，压住中间那条分隔线，
     * 屏幕上看着就是"上面那一栏铺满了、没分开"（用户报的"上下分栏没有展开"）。
     *
     * Qt Quick 6 没有"场景位置变了"这种 change（QQuickItem::ItemChange 里没有
     * 这一项），所以改成盯**所有祖先**的 x / y / width / height 信号
     * （见 watchAncestorGeometry）：祖先一动，立刻按场景坐标重摆。
     */
    case ItemParentHasChanged:
        watchAncestorGeometry();
        applyGeometry();
        break;
    case ItemVisibleHasChanged:
        if (m_sciWidget) {
            if (value.boolValue) {
                applyGeometry();
                if (!nativeSuppressed)
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

int EditorViewItem::lineHeight() const {
    if (m_sci && hasDocument())
        return textLineHeight();
    /*
     * 没有文档时按字体度量估一份。
     *
     * 设置面板要显示"当前行高 xx px"，那个绑定是在面板建出来的时候算的 ——
     * 应用刚起来还没有标签，这里要是直接返回 0，面板上就一直挂着 "0 px"
     * （字号那个不受影响，所以只有行高露馅）。字体度量和 Scintilla 用的
     * 是同一份上升/下降值，自然行高估出来和实际一样（Consolas 12px = 15px）。
     */
    return qRound(double(naturalLineHeight()) * m_lineHeightFactor);
}

int EditorViewItem::naturalLineHeight() const {
    if (m_sci && hasDocument())
        return qMax(0, m_sci->textHeight(0) - m_sci->extraAscent() - m_sci->extraDescent());
    /*
     * 没文档：按字体度量估一份。
     *
     * descent 要 +1 才和 Scintilla 一致 —— PlatQt.cpp 的 SurfaceImpl::Descent()
     * 就是这么给的（"Qt doesn't include the baseline in the descent, so add it"），
     * 少这 1px 的话设置面板上算出来的 px 会比编辑器里真实的行高少 1。
     */
    const QFontMetrics fm(uiFont());
    return fm.ascent() + fm.descent() + 1;
}

void EditorViewItem::applyLineSpacing() {
    if (!m_sci)
        return;

    /*
     * 行高怎么落到 Scintilla 上。
     *
     * Scintilla 没有"把行高设成 N 像素"的消息，只有给每一行加**额外上下空白**
     * （SCI_SETEXTRAASCENT / SCI_SETEXTRADESCENT，见 ViewStyle::Refresh：
     * lineHeight = maxAscent + maxDescent + extraAscent + extraDescent）。
     *
     * 所以这里按倍数算差额，再**平均分到上下两侧** —— 全加在上面（或下面）
     * 会把整行的字推到偏上 / 偏下，正文在行里就不居中了。
     *
     * 差额取 max(0, ...)：倍数 < 1.0 时行高比字形还小，行与行的字会叠在一起，
     * 所以只往松的方向拉；setLineHeightFactor() 也把倍数夹在 [1.0, 3.0]。
     */
    const int natural = naturalLineHeight();
    if (natural <= 0)
        return;

    const int extra = qMax(0, int(qRound(natural * m_lineHeightFactor)) - natural);
    const int above = extra / 2;
    m_sci->setExtraAscent(above);
    m_sci->setExtraDescent(extra - above);
}

void EditorViewItem::setLineHeightFactor(qreal factor) {
    /* 比字形还紧会压字，比 3 倍还松没有意义，两头都夹住 */
    factor = qBound(1.0, factor, 3.0);
    if (qFuzzyCompare(factor, m_lineHeightFactor))
        return;
    m_lineHeightFactor = factor;

    applyLineSpacing();
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

void EditorViewItem::setGutterLineVisible(bool on) {
    if (m_gutterLine == on)
        return;
    m_gutterLine = on;
    /*
     * 这条线是"第 1 条边距的宽度 + 它的背景色"，所以走 applyMargins()
     * （末尾会调 applyMarginTheme()，颜色在那里压）。
     */
    applyMargins();
    emit gutterLineChanged();
}

void EditorViewItem::setRulerVisible(bool on) {
    if (m_rulerVisible == on)
        return;
    m_rulerVisible = on;
    applyRuler();
    emit rulerChanged();
}

void EditorViewItem::setRulerColumn(int column) {
    /*
     * 夹到 1 ~ 2000。
     *
     * 下限给 1 而不是 0：列号 0 的线会压在正文左边缘上（看着像正文的边框），
     * 想关掉这条线用 rulerVisible，别用"列号设 0"这种隐式写法。
     * 上限 2000 是"再宽的屏幕也够用"的兜底，防止设置文件里写进离谱的值。
     */
    column = qBound(1, column, 2000);
    if (m_rulerColumn == column)
        return;
    m_rulerColumn = column;
    applyRuler();
    emit rulerChanged();
}

/*
 * 字数参考线（Scintilla 的 edge）。
 *
 * 为什么用 edge 而不是自己画：位置是 Scintilla 按 vs.spaceWidth（当前默认样式
 * 的空格宽）算的（见 EditView.cpp 的 DrawEdgeLine），换字体 / 改字号 / 缩放
 * 之后它自己就落回"第 80 个字"的位置，不用我们跟着重算；正文下方的空白区
 * 也一起画，所以线是通到底的。
 *
 * 注意 Scintilla 只有**一条** edge，所以它专门管"第 N 个字"这条；行号右边
 * 那条分隔线走边距（见 applyMargins）。
 *
 * 颜色走 setEdgeColor(QColor)：QScintilla 的 QColor 重载会正确打包 BGR。
 * 三发消息内部都会 InvalidateStyleRedraw()（Editor.cpp:7574 一带），不用自己
 * 再 update()。
 */
void EditorViewItem::applyRuler() {
    if (!m_sci)
        return;

    m_sci->setEdgeColumn(m_rulerColumn);
    m_sci->setEdgeColor(kGuideLine);
    m_sci->setEdgeMode(m_rulerVisible ? QsciScintilla::EdgeLine
                                      : QsciScintilla::EdgeNone);

    /* 参考线的位置/开关变了，底边那条补线也要重画 */
    updateBottomLines();
}

int EditorViewItem::rulerEdgeMode() const {
    if (!m_sci)
        return -1;
    return int(m_sci->edgeMode());
}

int EditorViewItem::rulerEdgeColumn() const {
    if (!m_sci)
        return -1;
    return m_sci->edgeColumn();
}

int EditorViewItem::rulerEdgeColor() const {
    if (!m_sci)
        return -1;
    /* edgeColor() 已经把 BGR 还原成 QColor，这里再按 Scintilla 的打包规则返回，
       自检就能和 marginBack / styleBack 一样用 packed() 直接比 */
    return int(scColor(m_sci->edgeColor()));
}

QVariantList EditorViewItem::rulerPixelStats() const {
    QVariantList out{-1, -1, -1};   // { found, expected, bottomGap }
    if (!m_sci || !m_sciWidget || !hasDocument())
        return out;

    const QImage img = m_sciWidget->grab().toImage();
    if (img.isNull())
        return out;

    const qreal scale =
        m_sciWidget->width() > 0 ? qreal(img.width()) / qreal(m_sciWidget->width()) : 1.0;

    long margins = 0;
    for (int m = 0; m <= 2; ++m)
        margins += marginWidth(m);

    /*
     * Scintilla 画 edge 的公式（EditView.cpp: DrawEdgeLine）：
     *     x = 列号 × 空格宽 + xStart（xStart 就是正文左边缘，横滚时跟着挪）
     * 而正文左边缘 = 各条边距宽度 + 左留白（SCI_SETMARGINLEFT）。
     * 空格宽用 QFontMetrics 量：和 Scintilla 量的是同一个字体（正文默认样式
     * 用的就是 uiFont()），等宽字体下就是字符宽。
     */
    const QFontMetrics fm(uiFont());
    const int space = fm.horizontalAdvance(QLatin1Char(' '));
    const int expected =
        int(qRound(double(margins + m_paddingLeft + m_rulerColumn * space) * scale));

    /*
     * 在算出来的位置附近找那一条：要求它是**通到底**的一列（整幅图高都亮）。
     *
     * 不能"从最左边扫到的第一个竖线色就是它"：三条竖线颜色一样（见 kGuideLine），
     * 分隔线和缩进参考线都会先被扫到。字数参考线是通到底的（正文下方也画，
     * EditView.cpp 的 rcBeyondEOF 那一支），缩进参考线只跟到缩进块结束，用这个
     * 区分。容差留 6：高 DPI 下 1px 的线落在半像素上会被轻微混色。
     */
    const int fullHeight = qMax(1, img.height() * 4 / 5);
    int found = -1;
    const int from = qMax(0, expected - 6);
    const int to = qMin(img.width() - 1, expected + 6);
    auto isRulerColumn = [&img, fullHeight](int x) {
        int ink = 0;
        for (int yy = 0; yy < img.height(); ++yy) {
            const QColor c = img.pixelColor(x, yy);
            if (qAbs(c.red() - kGuideLine.red()) <= 6
                && qAbs(c.green() - kGuideLine.green()) <= 6
                && qAbs(c.blue() - kGuideLine.blue()) <= 6)
                ++ink;
        }
        return ink >= fullHeight;
    };
    for (int x = from; x <= to && found < 0; ++x) {
        if (isRulerColumn(x))
            found = x;
    }

    out[0] = found;
    out[1] = expected;
    /*
     * 第三个：这条线最低的那一点离控件底边还有几像素。横条出现时正文区画不到
     * 那一行，全靠底下那块补线控件（见 updateBottomLines），这里量它有没有补上。
     */
    int maxY = -1;
    if (found >= 0) {
        for (int yy = 0; yy < img.height(); ++yy) {
            const QColor c = img.pixelColor(found, yy);
            if (qAbs(c.red() - kGuideLine.red()) <= 6
                && qAbs(c.green() - kGuideLine.green()) <= 6
                && qAbs(c.blue() - kGuideLine.blue()) <= 6)
                maxY = yy;
        }
    }
    out[2] = (maxY >= 0) ? (img.height() - 1 - maxY) : -1;
    return out;
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
    const int slot = currentDocSlot();
    if (slot < 0)
        return nullptr;
    return m_docs.at(slot).doc.get();
}

/* 当前标签对应池子里的第几份（-1 = 这一栏没打开任何文档） */
int EditorViewItem::currentDocSlot() const {
    if (m_current < 0 || m_current >= m_open.size())
        return -1;
    return m_open.at(m_current);
}

int EditorViewItem::slotOfTab(int tabIndex) const {
    if (tabIndex < 0 || tabIndex >= m_open.size())
        return -1;
    return m_open.at(tabIndex);
}

int EditorViewItem::tabOfSlot(int slot) const {
    return m_open.indexOf(slot);
}

int EditorViewItem::openDocumentById(int docId, bool activate) {
    const int slot = tabIndexOfDocId(docId);
    if (slot < 0)
        return -1;
    const int existing = tabOfSlot(slot);
    if (existing >= 0) {
        if (activate)
            activateDocument(existing);
        return existing;
    }
    m_open.append(slot);
    const int tab = m_open.size() - 1;
    if (activate)
        activateDocument(tab);
    else
        emit documentsChanged();
    return tab;
}

QString EditorViewItem::displayName() const {
    if (!hasDocument())
        return QString();
    return titleOf(*m_docs.at(currentDocSlot()).doc);
}

QString EditorViewItem::filePath() const {
    if (!hasDocument())
        return QString();
    return m_docs.at(currentDocSlot()).doc->filePath;
}

bool EditorViewItem::modified() const {
    if (!hasDocument())
        return false;
    return m_docs.at(currentDocSlot()).doc->modified;
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
    m_docs[currentDocSlot()].doc->modified = false;
    emit modifiedChanged();
    emit documentsChanged();
    emit tabsChanged();
}

QString EditorViewItem::language() const {
    if (!hasDocument())
        return QStringLiteral("plain");
    return m_docs.at(currentDocSlot()).doc->language;
}

void EditorViewItem::setLanguage(const QString &id) {
    if (!hasDocument() || id.isEmpty())
        return;
    Doc *doc = m_docs.at(currentDocSlot()).doc.get();
    if (doc->language == id)
        return;
    doc->language = id;

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
    emit tabsChanged();
}

QString EditorViewItem::encoding() const {
    if (!hasDocument())
        return QStringLiteral("UTF-8");
    return m_docs.at(currentDocSlot()).doc->encoding;
}

void EditorViewItem::setEncoding(const QString &name) {
    if (!hasDocument() || name.isEmpty())
        return;
    Doc *doc = m_docs.at(currentDocSlot()).doc.get();
    if (doc->encoding == name)
        return;
    doc->encoding = name;
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
    /*
     * 只列**这一栏打开着**的文档（不是整个池子）。
     *
     * 两个栏各有自己的标签栏 —— 这正是"像 VS Code 那样，两栏是独立的 tab"
     * 那件事；池子里有哪几份是全局的，界面上看到的是"这一栏开着哪几份"。
     * 对外那个 index 是**标签下标**（0 = 最左边那条），和 QML 里点标签、
     * 关标签用的是同一套。
     */
    QVariantList out;
    for (int i = 0; i < m_open.size(); ++i) {
        const Doc &d = *m_docs.at(m_open.at(i)).doc;
        QVariantMap m;
        m.insert(QStringLiteral("index"), i);
        m.insert(QStringLiteral("title"), titleOf(d));
        m.insert(QStringLiteral("filePath"), d.filePath);
        m.insert(QStringLiteral("modified"), d.modified);
        m.insert(QStringLiteral("active"), i == m_current);
        m.insert(QStringLiteral("language"), d.language);
        /* 文档号：分栏那边认"两栏看的是不是同一份"用它（见 currentDocId） */
        m.insert(QStringLiteral("docId"), d.id);
        out.append(m);
    }
    return out;
}

QString EditorViewItem::titleOf(const Doc &d) const {
    if (!d.filePath.isEmpty())
        return QFileInfo(d.filePath).fileName();
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
    /*
     * 找的是**这一栏标签栏里**的那一条（返回值是标签下标，QML 拿它切标签）。
     * 池子是两栏共用的，所以"池子里有"不等于"这一栏开着"。
     */
    const QString abs = QFileInfo(path).absoluteFilePath();
    for (int tab = 0; tab < m_open.size(); ++tab) {
        const QString known = m_docs.at(m_open.at(tab)).doc->filePath;
        if (!known.isEmpty() && QFileInfo(known).absoluteFilePath() == abs)
            return tab;
    }
    return -1;
}

int EditorViewItem::newDocument() {
    ensureWrapped();
    if (!m_sci)
        return -1;

    /* 先把当前文档的滚动位置和光标记下来，再往标签列表里加新文档 */
    if (hasDocument())
        saveCurrentViewState();

    auto doc = std::make_shared<Doc>();
    doc->language = QStringLiteral("plain");
    doc->untitledNo = ++s_untitledCounter;

    /* 进池子（广播给所有栏）+ 进这一栏的标签栏 + 切过去 */
    appendPoolDocument(doc);
    const int tab = openDocumentById(doc->id, true);
    if (tab < 0)
        return -1;

    DocRef &ref = m_docs[m_open.at(tab)];
    ref.m_doc = doc->m_doc;
    ref.shown = true;
    m_sci->setDocument(ref.m_doc);
    applyStyle();
    applyViewOptions();

    ref.cursorPos = 0;
    ref.firstVisibleLine = 0;
    ref.xOffset = 0;

    emitDocumentsState();
    emit tabsChanged();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
    return tab;
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

    /*
     * 二进制不往编辑器里灌。
     *
     * 左树现在把导入目录里的东西**原样**列出来（见 ClipboardStore::scanFolder），
     * 里面难免有 png / exe / 压缩包。灌进 Scintilla 就是一屏乱码，更糟的是用户
     * 顺手 Ctrl+S 会把原文件写坏 —— 宁可不打开。判据和 store 那边一致：开头
     * 有没有 NUL 字节。
     *
     * 这里只把原因写进 lastError，不 emit errorOccurred：两个调用方
     * （Main.qml 的 openFile / openTreeFile）拿到 -1 都会自己把 lastError
     * 弹出来，再 emit 一次就是两张卡片叠在一起。
     */
    {
        QFile probe(abs);
        if (probe.open(QIODevice::ReadOnly)) {
            const QByteArray head = probe.read(4096);
            probe.close();
            if (head.contains('\0')) {
                m_lastError = QStringLiteral("这是二进制文件，编辑器不打开：%1")
                                  .arg(fi.fileName());
                return -1;
            }
        }
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

    Doc *doc = m_docs[currentDocSlot()].doc.get();
    doc->filePath = abs;
    doc->encoding = detectedEncoding;
    doc->language = languageForPath(abs);

    m_sci->SendScintilla(QsciScintillaBase::SCI_SETEOLMODE, eol);
    m_sci->SendScintilla(QsciScintillaBase::SCI_CONVERTEOLS, eol);

    applyStyle();
    applyViewOptions();
    doc->modified = false;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);

    emitDocumentsState();
    /*
     * 标题 / 语言刚填上，别的栏的标签栏也得跟着重画（它们在广播那一刻
     * 看到的还是一份"未命名 N"，因为当时路径还没填）。
     */
    emit tabsChanged();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
    return index;
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
        /* 视图状态（光标 / 滚动）是这一栏自己的，记在自己的账上 */
        if (hasDocument()) {
            DocRef &r = m_docs[currentDocSlot()];
            r.cursorPos = 0;
            r.firstVisibleLine = 0;
            r.xOffset = 0;
        }
    }
}

bool EditorViewItem::saveCurrent() {
    if (!hasDocument())
        return false;

    /*
     * 没有路径的文档保存不了，交给 QML 那边转"另存为"（见 Main.saveFile）：
     * 剪贴板内容现在也是真实文件了（日期目录里的 md），从左边点开就带着路径，
     * 所以这里只剩"未命名空白文档"这一种情况。
     */
    const QString path = m_docs.at(currentDocSlot()).doc->filePath;
    if (path.isEmpty())
        return false;
    return saveDocument(m_current, path);
}

bool EditorViewItem::updateDocumentPath(const QString &oldPath, const QString &newPath) {
    if (oldPath.isEmpty() || newPath.isEmpty())
        return false;

    const QString from = QFileInfo(oldPath).absoluteFilePath();
    const QString to = QFileInfo(newPath).absoluteFilePath();

    bool found = false;
    for (int slot = 0; slot < m_docs.size(); ++slot) {
        Doc *d = m_docs.at(slot).doc.get();
        if (QFileInfo(d->filePath).absoluteFilePath() != from)
            continue;
        d->filePath = to;
        /* md -> md 语言不变；别的扩展名顺手跟着认一遍 */
        const QString guess = languageForPath(to);
        if (guess != d->language) {
            d->language = guess;
            if (slot == currentDocSlot()) {
                applyLanguageLexer();
                emit languageChanged();
            }
        }
        found = true;
    }

    if (found) {
        emit documentsChanged();
        emit currentChanged();
        emit tabsChanged();
    }
    return found;
}

bool EditorViewItem::saveCurrentAs(const QString &path) {
    if (!hasDocument())
        return false;
    return saveDocument(m_current, path);
}

bool EditorViewItem::saveDocument(int index, const QString &path) {
    const int slot = slotOfTab(index);
    if (slot < 0 || path.isEmpty())
        return false;
    if (!m_sci)
        return false;

    if (index != m_current) {
        activateDocument(index);
        if (m_current != index)
            return false;
    }

    const QString text = m_sci->text();
    const QByteArray bytes = encodeText(text, m_docs.at(slot).doc->encoding);

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
    Doc *doc = m_docs[slot].doc.get();
    doc->filePath = abs;

    /* 未命名文件另存为之后按扩展名认语言 */
    if (doc->language == QLatin1String("plain")) {
        const QString guess = languageForPath(abs);
        if (guess != QLatin1String("plain")) {
            doc->language = guess;
            applyLanguageLexer();
            emit languageChanged();
        }
    }

    m_bulkLoading = true;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);
    m_bulkLoading = false;
    doc->modified = false;

    emit saved(abs);
    emit modifiedChanged();
    emit documentsChanged();
    emit currentChanged();
    emit tabsChanged();
    return true;
}

/* ------------------------------------------------------------------ */
/* 文档池（两个栏共用一份，见 EditorViewItem.h 里那段说明）              */
/* ------------------------------------------------------------------ */

void EditorViewItem::detachFromCurrentDocument() {
    /*
     * 让视图**先离开**当前文档。
     *
     * QScintilla 的视图（QsciScintilla::doc）一旦析构 / 切走，会去动那份
     * 底层文档；文档已经被放掉之后再动它就是读已释放内存。所以任何一次
     * "放掉一份文档"之前都必须先走这里（原来那个版本在 detach() 的注释里
     * 记了这个坑，现在收成一个函数）。
     */
    if (!m_sci)
        return;
    /* 校验的波浪线是画在这份文档上的，离开之前先擦掉（见 setCheckIssues） */
    clearCheckIssues();
    m_sci->setDocument(*m_scratch);
}

int EditorViewItem::appendPoolDocument(const std::shared_ptr<Doc> &doc) {
    if (!doc)
        return -1;

    if (doc->id <= 0)
        doc->id = ++s_nextDocId;

    s_pool.append(doc);

    /*
     * 广播给**所有**栏（包括自己）：各栏的文档表都跟着加一条（只加账，
     * 不进标签栏 —— 进不进标签栏由这一栏自己决定，见 openPoolDocument）。
     */
    for (int i = 0; i < s_all.size(); ++i) {
        if (EditorViewItem *any = s_all.at(i).data())
            any->syncPoolFromRegistry();
    }
    return tabIndexOfDocId(doc->id);
}

int EditorViewItem::poolCount() const {
    int n = 0;
    for (int i = 0; i < s_pool.size(); ++i)
        if (!s_pool.at(i).expired())
            ++n;
    return n;
}

bool EditorViewItem::hasPoolDocument(int docId) const {
    for (int i = 0; i < m_docs.size(); ++i) {
        const Doc *d = m_docs.at(i).doc.get();
        if (d && d->id == docId)
            return true;
    }
    return false;
}

int EditorViewItem::currentDocId() const {
    const int slot = currentDocSlot();
    if (slot < 0)
        return -1;
    return m_docs.at(slot).doc->id;
}

int EditorViewItem::tabIndexOfDocId(int docId) const {
    for (int i = 0; i < m_docs.size(); ++i)
        if (m_docs.at(i).doc && m_docs.at(i).doc->id == docId)
            return i;
    return -1;
}

void EditorViewItem::syncPoolFromRegistry() {
    /*
     * 把池子里的文档对齐到这一栏的文档表上。
     *
     * 池子是"整个编辑器里开着哪些文档"的唯一真相（s_pool）；每个栏都留一份
     * **同样顺序**的表 —— 这样"这一栏开着哪几份"（m_open）和"池子里第几份"
     * 的下标在两边是一致的，切标签、关标签都只要动自己这份。
     */
    if (m_syncing)
        return;
    m_syncing = true;

    /* 1) 池子里已经没了的（别处关掉了）：从自己的表里去掉 */
    for (int i = m_docs.size() - 1; i >= 0; --i) {
        const Doc *d = m_docs.at(i).doc.get();
        bool alive = false;
        for (int k = 0; k < s_pool.size(); ++k) {
            std::shared_ptr<Doc> held = s_pool.at(k).lock();
            if (held && held.get() == d) {
                alive = true;
                break;
            }
        }
        if (alive)
            continue;

        /* 这一栏还开着它：先让视图离开，再把标签摘掉（文档本身由池子管） */
        const int tab = tabOfSlot(i);
        if (tab >= 0) {
            if (tab == m_current)
                detachFromCurrentDocument();
            m_open.remove(tab);
            if (tab < m_current)
                --m_current;
            else if (tab == m_current)
                m_current = m_open.isEmpty() ? -1
                                             : qBound(0, tab, m_open.size() - 1);
        }

        /* 下标整体前移：自己的标签表和小工具表都要跟着挪 */
        for (int k = 0; k < m_open.size(); ++k)
            if (m_open.at(k) > i)
                --m_open[k];
        m_docs.remove(i);
    }

    /* 2) 池子里新加的：按池子的顺序补进自己的表（**不进**标签栏） */
    for (int s = 0; s < s_pool.size(); ++s) {
        std::shared_ptr<Doc> held = s_pool.at(s).lock();
        if (!held)
            continue;

        bool known = false;
        for (int k = 0; k < m_docs.size(); ++k) {
            if (m_docs.at(k).doc.get() == held.get()) {
                known = true;
                break;
            }
        }
        if (known)
            continue;

        /*
         * 只加账：正文不在这里读（读盘那条路在 openFile 里，它才知道编码、
         * 二进制判断那一套）。这一份进这一栏的**标签栏**是另一件事 ——
         * 由这一栏自己决定（openPoolDocument / activateDocument）。
         */
        m_docs.append(DocRef{held, QsciDocument(), false, 0, 0, 0});
    }

    m_syncing = false;
    emit documentsChanged();
    emit tabsChanged();
}

void EditorViewItem::openPoolDocument(int docId) {
    openDocumentById(docId, true);
}

void EditorViewItem::refreshSharedDocument() {
    if (!m_sci || !hasDocument())
        return;
    /*
     * 重画这一栏的正文区。另一栏刚刚改了同一份文档（同一个 Scintilla 文档），
     * 但两个视图各有自己的画面缓存，必须让这个视图重新算一遍可见行再重画。
     *
     * 行号栏的位数可能也变了（另一栏插了行），所以顺带走一遍 applyMargins，
     * 还有横向滚动条的显隐（见 updateHorizontalScroll）。
     */
    m_sci->viewport()->update();
    applyMargins();
    updateHorizontalScroll();
    emit statsChanged();
    emit modifiedChanged();
}

void EditorViewItem::refreshTabs() {
    if (m_syncing)
        return;
    emit documentsChanged();
    emit modifiedChanged();
    emit currentChanged();
}

void EditorViewItem::releaseDocument(int tab) {
    if (tab < 0 || tab >= m_open.size())
        return;

    /* 关掉的这一份上的校验波浪线跟着走（见 setCheckIssues） */
    clearCheckIssues();

    if (hasDocument())
        saveCurrentViewState();

    const bool wasCurrent = (tab == m_current);
    const int slot = m_open.at(tab);
    DocRef taken = m_docs.at(slot);

    if (m_sci) {
        if (wasCurrent) {
            /*
             * 先挑接替者：优先右边那条标签，没有就左边那条。
             * 挑好之后**先把视图切过去**，再放掉要关的那一份壳子 ——
             * 反过来的话 Scintilla 还挂在一份正在消失的文档上。
             */
            const int successor = (tab + 1 < m_open.size()) ? tab + 1 : tab - 1;
            if (successor >= 0)
                m_sci->setDocument(m_docs.at(m_open.at(successor)).m_doc);
            else
                detachFromCurrentDocument();
        }
    }

    /* 自己那份壳子放掉（引用计数减一，不是删文档） */
    taken.m_doc = QsciDocument();
    taken.shown = false;
    m_open.remove(tab);

    /* 标签下标前移：当前那个要跟着挪 */
    if (wasCurrent) {
        m_current = (tab < m_open.size()) ? tab : m_open.size() - 1;
    } else if (tab < m_current) {
        --m_current;
    }
    if (m_current >= m_open.size())
        m_current = m_open.size() - 1;

    if (m_sci) {
        if (hasDocument()) {
            applyStyle();
            applyViewOptions();
            applyStoredViewState();
        } else {
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETREADONLY, 1L);
            applyStyle();
        }
    }

    emitDocumentsState();
    emit tabsChanged();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
}

void EditorViewItem::releaseAllDocuments() {
    if (hasDocument())
        saveCurrentViewState();
    detachFromCurrentDocument();

    /*
     * 只是**不再显示**它们：文档本身在池子里（别的栏可能还在看），
     * 这一栏的账（m_open）清空，m_docs 留着当"池子的镜像"。
     */
    for (DocRef &r : m_docs) {
        r.m_doc = QsciDocument();
        r.shown = false;
    }
    m_open.clear();
    m_current = -1;

    if (m_sci) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETREADONLY, 1L);
        applyStyle();
    }

    emitDocumentsState();
    emit tabsChanged();
}

void EditorViewItem::closeDocument(int index) {
    releaseDocument(index);
}

void EditorViewItem::closeCurrent() {
    if (hasDocument())
        releaseDocument(m_current);
}

void EditorViewItem::closeAll() {
    releaseAllDocuments();
}

/* ------------------------------------------------------------------ */
/* 分栏：两个栏各自一组标签，共用一份文档池（见 EditorViewItem.h）        */
/* ------------------------------------------------------------------ */

void EditorViewItem::setDocId(int id) {
    if (m_docId == id)
        return;
    m_docId = id;
    emit boundChanged();
}

void EditorViewItem::setMainEditor(bool on) {
    if (m_mainEditor == on)
        return;
    m_mainEditor = on;
    /*
     * 主编辑器 = "当前编辑器"（instance()）。分栏之后"用户点的是哪一栏"
     * 会让它跟着走（见 setPaneFocus），这里只认 on == true 的调用
     * （另一栏写 mainEditor: true 是配置错误，不认）。
     */
    if (on)
        s_instance = this;
    emit boundChanged();
}

void EditorViewItem::setMirror(bool on) {
    if (m_mirror == on)
        return;
    m_mirror = on;
    /*
     * 第二栏不是"只读镜像"了 —— 两栏对等，都能编辑（同一份文档，
     * 改哪边都是改同一个东西）。这个属性现在只用来让界面 / 自检认出
     * "这是第二栏"。
     */
    emit boundChanged();
}

void EditorViewItem::unbind() {
    /* 取消分栏：把自己打开的那些标签放回池子，自己关掉 */
    releaseAllDocuments();
    m_mirror = false;
    emit boundChanged();
}

void EditorViewItem::setPaneFocus(bool on) {
    /*
     * 分栏之后"当前编辑器"要跟着焦点走：这个类里所有命令（撤销 / 查找 /
     * 格式化）都不带"发给哪一栏"这个参数，外面（QML）也拿不到 C++ 的静态指针，
     * 所以在这里顺手把 s_instance 指到刚被点的那一栏上 —— 别的代码问
     * `EditorViewItem::instance()` 时拿到的就是"用户正在用的那个"。
     *
     * 不分栏时只有一栏，这一句等于什么都没做（本来就是它）。
     */
    if (m_paneFocus == on)
        return;
    if (on)
        s_instance = this;
    m_paneFocus = on;
    emit paneFocusChanged();
}


void EditorViewItem::activateDocument(int index) {
    if (index < 0 || index >= m_open.size() || index == m_current)
        return;
    if (!m_sci)
        return;

    /* 要离开的这一份上的校验波浪线先擦掉（它是按那一份的正文算出来的） */
    clearCheckIssues();

    if (hasDocument())
        saveCurrentViewState();

    m_current = index;
    /*
     * 切到这一份（池子里共享的那个底层文档）。两个栏看同一份时，
     * 这一句让**这个**栏也挂上它 —— QsciDocument 的引用计数自己会涨
     * （见 Doc::m_doc 的说明）。
     */
    DocRef &ref = m_docs[m_open.at(index)];
    /*
     * 这一栏第一次显示这一份：把池子里那份壳子拷进来（引用计数 +1）。
     * 拷贝是必须的 —— 两个视图共用一份 QsciDocument 对象的话，
     * 谁先放手就会把 pdoc 连同另一个视图的画面一起带掉（见
     * third/qscintilla/src/qscidocument.cpp 的 nr_attaches）。
     */
    if (!ref.shown) {
        ref.m_doc = ref.doc->m_doc;
        ref.shown = true;
    }
    m_sci->setDocument(ref.m_doc);

    /*
     * 样式和视图设置都是**按文档**存的（Scintilla 的样式表在文档里），
     * 所以切过去之后要整套重装一遍，否则新文档是默认的白底黑字。
     */
    applyStyle();
    applyViewOptions();
    applyStoredViewState();

    emitDocumentsState();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
    m_sci->viewport()->update();
}

int EditorViewItem::activateNextDocument() {
    if (m_open.size() < 2)
        return m_current;
    activateDocument((m_current + 1) % m_open.size());
    return m_current;
}

int EditorViewItem::activatePreviousDocument() {
    if (m_open.size() < 2)
        return m_current;
    activateDocument((m_current - 1 + m_open.size()) % m_open.size());
    return m_current;
}

void EditorViewItem::saveCurrentViewState() {
    if (!m_sci || !hasDocument())
        return;
    DocRef &r = m_docs[currentDocSlot()];
    r.cursorPos = long(m_sci->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS));
    r.firstVisibleLine =
        int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
    r.xOffset = int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET));
}

void EditorViewItem::applyStoredViewState() {
    if (!m_sci || !hasDocument())
        return;
    const DocRef &r = m_docs.at(currentDocSlot());
    const long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                         qBound(0L, r.cursorPos, docLen));
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                         long(qMax(0, r.firstVisibleLine)));
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET, long(qMax(0, r.xOffset)));
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
    if (m_sci && hasDocument() && !m_readOnly) {
        /* 剪切也会写系统剪贴板：这一趟不算"外部复制"，别采集（见 markOwnCopy） */
        if (m_store && m_sci->hasSelectedText())
            m_store->markOwnCopy();
        m_sci->cut();
    }
}

void EditorViewItem::copy() {
    if (m_sci && hasDocument()) {
        if (m_store && m_sci->hasSelectedText())
            m_store->markOwnCopy();
        m_sci->copy();
    }
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

    /*
     * 搜索标志要**单独发一条 SCI_SETSEARCHFLAGS**。
     *
     * Scintilla 的 SCI_SEARCHINTARGET 只收"长度 + 文本"两个参数，区分大小写 /
     * 全字 / 正则这三个开关是通过 SCI_SETSEARCHFLAGS 事先设好的（qsciscintilla
     * 自己的 findFirst 也是这么干的）。原来这里算完 flags 就没人用了 ——
     * 于是三个开关全是摆设（界面上勾了没反应，clang-analyzer 报的 dead store
     * 就是这一条）。标志是"粘住"的：每次搜之前都设一遍，免得被别处的调用带走。
     */
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSEARCHFLAGS,
                         static_cast<unsigned long>(flags));

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

    /* 搜文本那一步在 searchFrom 里（它自己转 UTF-8），这里不用再转一份 */
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

/*
 * 校验结果 -> 编辑区里的波浪线。
 *
 * 整份清单重新画一遍（先擦后画）。位置从**行列**换算过来：走
 * QsciScintilla::positionFromLineIndex，它按**字符**数，中文一个字算一列 ——
 * 校验那边给的行列就是这个口径（自己按字节加列号会在中文行上错位）。
 *
 * 位置和区间同时存一份（m_checkRanges），鼠标停上去时不用再换算一次
 * （见 checkIssueAt）。
 */
void EditorViewItem::setCheckIssues(const QVariantList &issues) {
    clearCheckIssues();
    if (!m_sci || !hasDocument())
        return;

    const long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);

    for (const QVariant &item : issues) {
        const QVariantMap m = item.toMap();
        const int row = m.value(QStringLiteral("row")).toInt();
        const int endRow = m.value(QStringLiteral("endRow")).toInt();
        if (row < 1)
            continue;

        /*
         * 列号夹回那一行里：模型给的列号经常越界（它数的和我们数的不是一个
         * 口径），越界就退成"画到行尾"—— 不夹的话那条波浪线会跨到下一行去，
         * 和说明里写的位置对不上。
         */
        const long lineStart = m_sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE,
                                                    long(row - 1));
        const long lineEnd = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLINEENDPOSITION,
                                                  long(row - 1));
        long start = qBound(lineStart,
                            (long)m_sci->positionFromLineIndex(row - 1,
                                                               qMax(0, m.value(
                                                                   QStringLiteral("col")).toInt())),
                            lineEnd);
        long stop = m_sci->positionFromLineIndex(qMax(0, endRow - 1),
                                                 qMax(0, m.value(QStringLiteral("endCol")).toInt()));
        if (stop > docLen)
            stop = docLen;
        /* 一个字符宽的问题（比如半角逗号）也要看得见那条波浪线 */
        if (stop <= start)
            stop = m_sci->SendScintilla(QsciScintillaBase::SCI_POSITIONAFTER, start);

        const QString severity = m.value(QStringLiteral("severity")).toString();
        const int indicator = severity == QLatin1String("error") ? kCheckErrorIndicator
                                                                 : kCheckWarnIndicator;
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, long(indicator));
        m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE, (unsigned long)start,
                             (unsigned long)qMax(1L, stop - start));

        m_checkIssues.append(item);
        m_checkRanges.append({start, stop});
    }

    m_sci->viewport()->update();
}

void EditorViewItem::clearCheckIssues() {
    m_checkIssues.clear();
    m_checkRanges.clear();

    if (!m_sci)
        return;
    const long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    if (docLen <= 0)
        return;

    for (const int indicator : { kCheckErrorIndicator, kCheckWarnIndicator }) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, long(indicator));
        m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE, 0L, docLen);
    }
    m_sci->viewport()->update();
}

/* 自检用：波浪线的字节区间（见头文件里的说明） */
QVariantList EditorViewItem::checkIssueRanges() const {
    QVariantList out;
    for (const QPair<long, long> &range : m_checkRanges) {
        out.append(qlonglong(range.first));
        out.append(qlonglong(range.second));
    }
    return out;
}

/* 自检用：那条波浪线真的画出来了吗（见头文件里的说明） */
QVariantList EditorViewItem::checkWavePixelStats() const {
    QVariantList out{0, 0, 0, 0};
    if (!m_sci || !m_sciWidget || !hasDocument() || m_checkRanges.isEmpty())
        return out;

    const QImage img = m_sciWidget->grab().toImage();
    if (img.isNull())
        return out;
    const qreal scale =
        m_sciWidget->width() > 0 ? qreal(img.width()) / qreal(m_sciWidget->width()) : 1.0;

    /* 正文从这几条边距右边开始（行号 / 折叠 / 分隔线，和 marginPixelStats 一个口径） */
    int textStart = 0;
    for (int margin = 0; margin < 3; ++margin)
        textStart += int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETMARGINWIDTHN,
                                              long(margin)) * scale);
    textStart = qBound(0, textStart, img.width());

    /*
     * 认色用"红占绝对多数"而不是精确比对：波浪线是抗锯齿画的，边上那些像素
     * 是本色和底色混出来的（混到一半也有 140 多的红）。正文默认色 #d6d7da 和
     * 那几档灰都不满足"红比绿蓝各高 40"。
     */
    auto reddish = [](const QColor &c) {
        return c.red() > 110 && c.red() > c.green() + 40 && c.red() > c.blue() + 40;
    };
    auto amber = [](const QColor &c) {
        return c.red() > 110 && c.green() > 80 && c.red() > c.blue() + 40
               && c.green() > c.blue() + 20;
    };

    int err = 0, warn = 0, fx = 0, fy = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = textStart; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            const bool isErr = reddish(c);
            if (isErr)
                ++err;
            else if (amber(c))
                ++warn;
            else
                continue;
            /* 第一个命中的像素（哪个色都算）：自检拿它当"停上去的那一点" */
            if (fx == 0 && fy == 0) {
                fx = x;
                fy = y;
            }
        }
    }
    out[0] = err;
    out[1] = warn;
    out[2] = fx;
    out[3] = fy;
    return out;
}

QString EditorViewItem::checkTipAtPoint(int x, int y) const {
    if (!m_sci)
        return QString();
    const long pos = m_sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMPOINT,
                                          (unsigned long)x, long(y));
    const int hit = checkIssueAt(pos);
    return hit >= 0 ? checkIssueHtml(hit) : QString();
}

/* 文档位置 pos 落在第几条问题上；不在任何一条里就看它落在哪一行（同一行上也认） */
int EditorViewItem::checkIssueAt(long pos) const {
    for (int i = 0; i < m_checkRanges.size(); ++i) {
        if (pos >= m_checkRanges.at(i).first && pos < m_checkRanges.at(i).second)
            return i;
    }
    if (!m_sci || m_checkRanges.isEmpty())
        return -1;

    /*
     * 波浪线可能只有一两个字符宽，鼠标不一定停得那么准；停在**同一行**上
     * 也把它认下来 —— IDE 里也是这样，停在那一行就能看到那一行的问题。
     */
    const long line = m_sci->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, pos);
    for (int i = 0; i < m_checkRanges.size(); ++i) {
        if (m_sci->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION,
                                 m_checkRanges.at(i).first) == line)
            return i;
    }
    return -1;
}

/* 一条问题的悬浮说明：级别 / 第几行 / 谁报的 / 说明 / 原文片段 / 建议改法 */
QString EditorViewItem::checkIssueHtml(int index) const {
    const QVariantMap m = m_checkIssues.value(index).toMap();
    const QString severity = m.value(QStringLiteral("severity")).toString();
    const QString head = severity == QLatin1String("error") ? QStringLiteral("错误")
                        : severity == QLatin1String("warn") ? QStringLiteral("警告")
                                                            : QStringLiteral("提示");
    const bool model = m.value(QStringLiteral("source")).toString() == QLatin1String("llm");

    QString html = QStringLiteral("<b>%1</b> · 第 %2 行 %3<hr>%4")
                       .arg(head)
                       .arg(m.value(QStringLiteral("row")).toInt())
                       .arg(model ? QStringLiteral("（大模型）") : QStringLiteral("（本地规则）"))
                       .arg(m.value(QStringLiteral("message")).toString().toHtmlEscaped());

    const QString snippet = m.value(QStringLiteral("snippet")).toString();
    if (!snippet.isEmpty())
        html += QStringLiteral("<br>「%1」").arg(snippet.toHtmlEscaped());

    const QString suggestion = m.value(QStringLiteral("suggestion")).toString();
    if (!suggestion.isEmpty())
        html += QStringLiteral(" → <b>%1</b>").arg(suggestion.toHtmlEscaped());

    return html;
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

/*
 * 整份替换（格式化 / 批量改写用）。
 *
 * 走 SCI_BEGINUNDOACTION / ENDUNDOACTION 包成一个 **可撤销的一步**：
 * 用户按 Ctrl+Z 一次就回到替换之前，而不是一步一个字地退。
 * 光标位置也还原（替换之后文档全变了，原来的位置没有意义，就从头上开始）。
 */
void EditorViewItem::setText(const QString &text) {
    if (!m_sci || !hasDocument())
        return;

    m_bulkLoading = true;
    m_sci->SendScintilla(QsciScintillaBase::SCI_BEGINUNDOACTION);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETTEXT, text.toUtf8().constData());
    m_sci->SendScintilla(QsciScintillaBase::SCI_ENDUNDOACTION);
    m_bulkLoading = false;
    /*
     * 这一句**不能加**：SCI_EMPTYUNDOBUFFER 会把整个撤销栈清掉，
     * 包括刚包好的这一步 —— 用户按 Ctrl+Z 就退不回格式化之前了。
     * 想要的是"这一步可撤销"，所以历史照留。
     */

    m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, 0L);
    applyMargins();
    updateHorizontalScroll();

    emit statsChanged();
    emit modifiedChanged();
    /*
     * 只有真改了才标"已修改"：格式化算一次内容改动（用户要自己存盘），
     * 但"格式化完发现其实一样"那种不该把文件标脏。
     */
    if (Doc *d = currentDoc()) {
        if (!d->modified) {
            d->modified = true;
            emit documentsChanged();
        }
    }
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

/*
 * 直接定一个缩放百分比（QML 绑定用，见 Q_PROPERTY 里的说明）。
 *
 * 传进来的是**百分比**（100 = 不缩放）。Scintilla 的 zoom 增量单位是**点**，
 * 所以换算要拿字号的点数当基准：basePoint100 是"点 × 100"（见
 * updateZoomPercent），点数 = basePoint100 / 100，于是
 *     zoom = 点数 × (百分比 - 100) / 100 = basePoint100 × (百分比 - 100) / 10000
 * 夹在 -8 ~ 30，和 zoomIn / zoomOut 一个范围 —— 绑定回写时不会越夹越远。
 */
void EditorViewItem::setZoomPercent(int percent) {
    if (!m_sci)
        return;
    const long zoom = m_sci->SendScintilla(QsciScintillaBase::SCI_GETZOOM);
    const int basePoint100 = qMax(100, stylePointSize());
    const long want = qBound(-8L,
                             long(qRound(double(basePoint100) * (percent - 100) / 10000.0)),
                             30L);
    if (want == zoom) {
        updateZoomPercent();
        return;
    }
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETZOOM, want);
    updateZoomPercent();
    emit zoomChanged();
    QTimer::singleShot(0, this, [this]() { updateHorizontalScroll(); });
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

/* 自检用：两条滚动条自己那块画面的像素统计（见头文件里的说明） */
QVariantList EditorViewItem::scrollBarPixelStats() const {
    QVariantList out{0, 0, false, false, 0, 0};
    if (!m_sci)
        return out;

    /* 量一条：返回近白像素数（不可见 / 抓不到图给 -1），顺手把轨道色带出来 */
    auto scan = [](QScrollBar *bar, QColor *track) -> int {
        if (!bar || !bar->isVisible())
            return -1;
        const QImage img = bar->grab().toImage();
        if (img.isNull() || img.width() <= 0 || img.height() <= 0)
            return -1;
        if (track)
            *track = img.pixelColor(0, 0);
        int white = 0;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const QColor c = img.pixelColor(x, y);
                if (c.red() >= 250 && c.green() >= 250 && c.blue() >= 250)
                    ++white;
            }
        }
        return white;
    };

    QColor vTrack;
    QColor hTrack;
    QScrollBar *vb = m_sci->verticalScrollBar();
    QScrollBar *hb = m_sci->horizontalScrollBar();

    out[0] = scan(vb, &vTrack);
    out[1] = scan(hb, &hTrack);
    out[2] = vb && vb->isVisible();
    out[3] = hb && hb->isVisible();
    out[4] = vTrack.isValid() ? int(vTrack.rgb() & 0x00ffffff) : 0;
    out[5] = hTrack.isValid() ? int(hTrack.rgb() & 0x00ffffff) : 0;
    return out;
}

/* 自检用：横向滚动条的状态（见头文件里的说明） */
QVariantMap EditorViewItem::horizontalScrollState() const {
    QVariantMap state;
    if (!m_sci)
        return state;

    auto *hb = m_sci->horizontalScrollBar();
    const long viewWidth = m_sci->viewport() ? m_sci->viewport()->width() : 0;

    long fixed = m_paddingLeft + m_paddingRight;
    for (int m = 0; m < 3; ++m) {
        const int w = marginWidth(m);
        if (w > 0)
            fixed += w;
    }

    state[QStringLiteral("visible")] = hb ? hb->isVisible() : false;
    state[QStringLiteral("maximum")] = hb ? hb->maximum() : -1;
    /* 当前值：自检量"滚动条右键那几条动作真的滚动了"用 */
    state[QStringLiteral("value")] = hb ? hb->value() : -1;
    state[QStringLiteral("pageStep")] = hb ? hb->pageStep() : -1;
    state[QStringLiteral("pageWidthComputed")] = int(qMax(1L, viewWidth - fixed));
    state[QStringLiteral("viewportWidth")] = int(viewWidth);
    state[QStringLiteral("scrollWidth")] =
        int(m_sci->SendScintilla(QsciScintillaBase::SCI_GETSCROLLWIDTH));
    state[QStringLiteral("contentWidth")] = int(m_lastContentWidth);
    state[QStringLiteral("wrap")] = m_wrap;
    return state;
}

/*
 * 自检用：量"面板被拉窄的那一拍"（见头文件里的说明）。
 *
 * 直接改控件的宽度，等于把一页文本宽 hNewPage 当场改小；Scintilla 在
 * resizeEvent 里就会重算 hMax = scrollWidth - hNewPage（ScintillaQt.cpp 的
 * ModifyScrollBars），所以读 hState 之前**不能**转事件循环 ——
 * 一转，applyGeometry 排的那次重算就把值压回去了，那一拍就看不到了。
 * 量完把宽度还原。
 */
/*
 * 自检用：这一栏的 QML item 和它那块原生控件各摆在哪、多大（见头文件说明）。
 */
QVariantMap EditorViewItem::paneGeometryForTest() const {
    QVariantMap m;
    const QPointF scene = mapToItem(nullptr, QPointF(0, 0));
    m[QStringLiteral("sceneX")] = scene.x();
    m[QStringLiteral("sceneY")] = scene.y();
    m[QStringLiteral("itemW")] = width();
    m[QStringLiteral("itemH")] = height();
    if (m_sciWidget) {
        const QRect g = m_sciWidget->geometry();
        m[QStringLiteral("widgetX")] = g.x();
        m[QStringLiteral("widgetY")] = g.y();
        m[QStringLiteral("widgetW")] = g.width();
        m[QStringLiteral("widgetH")] = g.height();
        m[QStringLiteral("widgetVisible")] = m_sciWidget->isVisible();
    }
    return m;
}

QVariantMap EditorViewItem::horizontalScrollAfterNarrowForTest(int deltaWidth) {
    QVariantMap state;
    if (!m_sci)
        return state;

    const int wasWidth = m_sci->width();
    const int wasHeight = m_sci->height();
    m_sci->resize(qMax(120, wasWidth + deltaWidth), wasHeight);

    state = horizontalScrollState();

    m_sci->resize(wasWidth, wasHeight);
    return state;
}

void EditorViewItem::scrollBarAction(const QString &axis, const QString &what) {
    if (!m_sci)
        return;

    /*
     * 全部落到 QScrollBar::triggerAction() 上 —— 这正是 Qt 自带那个滚动条菜单
     * 内部用的东西（见 QScrollBar::contextMenuEvent），所以语义一模一样：
     * 值一变，QsciScintillaBase 就把 SCI_SETFIRSTVISIBLELINE / SCI_SETXOFFSET
     * 发给 Scintilla（见 third/qscintilla/src/qsciscintillabase.cpp 的
     * connectVerticalScrollBar / connectHorizontalScrollBar）。
     */
    QScrollBar *bar = (axis == QLatin1String("v")) ? m_sci->verticalScrollBar()
                                                  : m_sci->horizontalScrollBar();
    if (!bar || bar->minimum() == bar->maximum())
        return;

    if (what == QLatin1String("here")) {
        bar->setValue(qBound(bar->minimum(), m_scrollMenuValue, bar->maximum()));
    } else if (what == QLatin1String("edgeStart")) {
        bar->triggerAction(QAbstractSlider::SliderToMinimum);
    } else if (what == QLatin1String("edgeEnd")) {
        bar->triggerAction(QAbstractSlider::SliderToMaximum);
    } else if (what == QLatin1String("pageBack")) {
        bar->triggerAction(QAbstractSlider::SliderPageStepSub);
    } else if (what == QLatin1String("pageForward")) {
        bar->triggerAction(QAbstractSlider::SliderPageStepAdd);
    } else if (what == QLatin1String("lineBack")) {
        bar->triggerAction(QAbstractSlider::SliderSingleStepSub);
    } else if (what == QLatin1String("lineForward")) {
        bar->triggerAction(QAbstractSlider::SliderSingleStepAdd);
    }
}

bool EditorViewItem::triggerScrollBarContextMenu(bool horizontal, int pos) {
    if (!m_sci)
        return false;

    QScrollBar *bar = horizontal ? m_sci->horizontalScrollBar() : m_sci->verticalScrollBar();
    if (!bar)
        return false;

    if (pos < 0)
        pos = (horizontal ? bar->width() : bar->height()) / 2;

    const QPoint inBar = horizontal ? QPoint(pos, bar->height() / 2)
                                    : QPoint(bar->width() / 2, pos);
    const QPoint global = bar->mapToGlobal(inBar);

    m_scrollMenuHandled = false;
    QContextMenuEvent event(QContextMenuEvent::Mouse, inBar, global);
    QApplication::sendEvent(bar, &event);
    return m_scrollMenuHandled;
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
