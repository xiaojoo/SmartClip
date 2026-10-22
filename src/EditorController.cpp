#include "EditorController.h"
#include "Theme.h"


#include <QAbstractNativeEventFilter>
#include <QAction>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QRegularExpression>
#include <QSettings>
#include <QTextDocument>
#include <QUrl>
#include <QWidget>

#if defined(Q_OS_WIN)
#  include <windows.h>
#endif

namespace {

/*
 * 要额外注册成**系统级**热键（Windows 的 RegisterHotKey）的那几条。
 *
 * 为什么只有这几条：它们的作用不是"编辑当前文件"，而是"把某个东西从桌面上
 * 叫出来"—— 收进托盘 / 主窗口被压着时也得响（用户报的"最小化之后截图快捷键
 * 不能用"就是这个）。别的命令（保存 / 查找）离开主窗口没有意义，不该占系统热键。
 *
 * id 是 WM_HOTKEY 的回执号，每条一个、不能重（见 Windows 的 RegisterHotKey）。
 *
 * 放在 Q_OS_WIN 外面：非 Windows 那半边也要按这张表去问"这个命令有没有
 * 全局热键"（m_hotkeys 里不会有记录，所以答案恒为 false，见
 * globalHotkeyActiveFor）。
 */
struct GlobalHotkeyEntry {
    int id;
    const char *name;   /* 必须和 kShortcutTable 里的名字一致 */
};

const GlobalHotkeyEntry kGlobalHotkeys[] = {
    {0x5C01, "shot"},
    {0x5C02, "note"},
    /* 翻译卡片 / 便签都是"把一个工具从桌面上叫出来"，同样值得有系统级热键 */
    {0x5C03, "translate"},
};

/*
 * Markdown 预览那份 HTML 的样式（见 EditorController::markdownToPreviewHtml）。
 *
 * 这一份是**渲染完贴回去**的：插在 toHtml() 出来的 </head> 前面。为什么不能靠
 * setDefaultStyleSheet：实测 toHtml() 并不把它带出来，head 里只剩 Qt 自己那段
 * `p, li { white-space: pre-wrap; }`。
 *
 * 为什么每一处颜色都要写死：QML 的 Text 只认行内的这些属性，它**不读**
 * QTextDocument 那一套调色板。默认样式表是一份白纸黑字，留着不管的话，
 * 深色界面上就是一片黑字（几乎等于什么都没显示）。颜色值和界面其它地方
 * 对齐：正文 / 标题用 #d6d7da 系，次要文字 #9aa0a6，代码块底 #26282c。
 *
 * 间距那一套是照 Typora 的 github 主题量的（本机 D:\Program\Typora 里那份 CSS），
 * 用真实管线一段一段量出来对表。三条要记的换算：
 *   1) **必须先把 toHtml() 写的行内 margin-top/bottom:0px 摘掉**（见下面那个正则），
 *      行内样式压过样式表，不摘的话这里的 margin 一条都不生效。
 *   2) Qt 不认 rem，而 em 是**按本元素自己的字号**解析的 —— 想要"标题上下留常量
 *      12.5px"，每一档字号都得各除一遍，所以下面 h1~h6 那五个数不一样。
 *   3) Qt 的 line-height 乘的是**自然行高**（雅黑 14px 是 18px），浏览器乘的是字号
 *      —— Typora 的 1.6 在这边要写 1.3 才落到同一个 23px 行距。
 * 标题的 margin-bottom 写了也没用（Qt 富文本不吃，10.5 / 7.5 / 6px 三档量出来一样），
 * 所以只写 margin-top；标题下面那条缝由行盒余量 + 下一块的 margin-top 决定。
 * pre 的上下 margin 在这里保持 0：块外那圈缝是给**表格**的（见 kCodeGapTopPx），
 * 给到 pre 上落不进渲染（行内写 13px，块内只量到 3px —— 块级 margin 塌到格子外面）。
 */
/*
 * 代码块四边的内衬像素数（写在 <pre> 的行内 margin 上，见 markdownToPreviewHtml）。
 *
 * 钉的是 Typora 那张参考图的实测值：内容↔背景 左 13 / 上 12 / 下 12。
 */
constexpr int kCodePadPx = 13;

/*
 * 代码块**外面**上下留的缝（底色和正文之间，不是块内部）。
 *
 * 之前是 0：样式表里 `pre { margin-top:0; margin-bottom:0 }` 写死，因为那时一行代码
 * 一个 <pre>，给了就把代码行距顶开。现在一块围栏只有一个 <pre>，这笔可以给了。
 *
 * 两头分开是因为上下邻居不一样（都是真机量的，目标 = Typora 那张的 19 px）：
 *   上：上面多半是标题，而标题的 margin-bottom 在 Qt 里**完全不生效**，这一头只能块自己出；
 *   下：下面是正文段，它自带 0.62em（≈8.7 px）的上边距，块再加 14 就顶到 36 px 去了。
 */
constexpr int kCodeGapTopPx = 16;
constexpr int kCodeGapBottomPx = 2;

/*
 * 代码块的行高倍数（乘的是字体的自然行高）。这个 1.43 是真机反推的：行内写 1.3 时
 * 抓屏量到 21 px（band tops 20/21/20/21/21/21/21/20），说明这条路上 Consolas 14px
 * 的自然行高只有 21 / 1.3 ≈ 16.1 px，要对上 Typora 那一档的 23 px 就得 23 / 16.1。
 * 样式表里那一档在包进表格之后不落渲染（抓屏量到 15~17 px），所以这个数是以
 * **行内样式**写进 <pre> 的（见 markdownToPreviewHtml 里 openTag 那段）。
 */
constexpr double kCodeLineHeight = 1.43;

const char *const kMarkdownCss = R"CSS(
body { color: #d6d7da; }
p { color: #d6d7da; margin-top: 0.62em; margin-bottom: 0.62em; line-height: 1.3; }
h1, h2, h3, h4, h5, h6 { color: #e8e8e8; font-weight: bold; line-height: 1.15; }
h1 { font-size: 1.7em;  margin-top: 0.525em; }
h2 { font-size: 1.45em; margin-top: 0.616em; }
h3 { font-size: 1.25em; margin-top: 0.714em; }
h4 { font-size: 1.12em; margin-top: 0.797em; }
h5, h6 { font-size: 1em; margin-top: 0.893em; }
a { color: #4c96d8; }
code { font-family: Consolas, "Courier New", monospace; color: #d7ba7d; }
pre { font-family: Consolas, "Courier New", monospace; color: #d6d7da;
      background-color: transparent; line-height: 1.3;
      margin-top: 0px; margin-bottom: 0px; }
blockquote { color: #9aa0a6; margin-left: 1.2em;
             margin-top: 0.62em; margin-bottom: 0.62em; line-height: 1.3; }
li { color: #d6d7da; line-height: 1.3; }
ul, ol { margin-top: 0.62em; margin-bottom: 0.62em; line-height: 1.3; }
table { border-width: 1px; border-style: solid; border-color: #4b4d4f;
        margin-top: 0.62em; margin-bottom: 0.62em; line-height: 1.3; }
th { background-color: #2b2d30; color: #e8e8e8; font-weight: bold;
     border-width: 1px; border-style: solid; border-color: #4b4d4f; }
td { border-width: 1px; border-style: solid; border-color: #4b4d4f; }
/*
 * 代码块的四边内衬只能这么给。
 *
 * CSS 的 padding 和 border 在 Qt 的**块**上都不生效（各量过一档：加 padding:12px
 * 与不加逐位相同，连 8px 的红框都没画出来），类选择器也不认（给 table.codeblock
 * 或 td.codecell 写 background-color，代码区整块是白的）—— 能画出"底色往里缩一圈"
 * 的只有**表格单元格 + 元素选择器**。所以生成 HTML 时把整块 <pre> 包进一个 1x1 的
 * table，底色写在那格的内联样式上（见 markdownToPreviewHtml 里那个 wrapper，
 * 它单独过一遍 themedCss，浅色档跟着翻）。
 *
 * 这里**不能**给 td 写 background-color：那条会连正文里的 markdown 表格一起涂灰。
 */
hr { color: #4b4d4f; }
)CSS";

/*
 * 预览那份 HTML 的样式表整段过一遍主题表。
 *
 * 为什么单独处理：这些颜色在 **CSS 字符串**里，既不是 QML 字面量也不是 QColor，
 * 那两套机械替换都扫不到 —— 浅色档下预览就是白底上一片 #e8e8e8 淡字
 * （他截图圈的就是这个）。按 #rrggbb 逐个查表，深色档恒等。
 *
 * 切主题后必须由 QML 再调一次 refreshMarkdown()（见 Main.qml 那个
 * Connections { target: Theme }）：HTML 是生成出来的字符串，不吃绑定。
 */
QString themedCss(const QString &css) {
    static const QRegularExpression kHexes(QStringLiteral("#[0-9a-fA-F]{6}"));
    QString out;
    int last = 0;
    QRegularExpressionMatchIterator it = kHexes.globalMatch(css);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += css.mid(last, int(m.capturedStart()) - last);
        out += AppTheme::instance() ? AppTheme::instance()->c(m.captured(0)) : m.captured(0);
        last = int(m.capturedEnd());
    }
    return out + css.mid(last);
}

/*
 * 挂在 QApplication 上的事件过滤器：把按键转给 DoubleShiftDetector
 * （那个判定类在 EditorController.h 里，自检直接驱动它）。
 *
 * 为什么是 qApp 级而不是主窗口的 keyPressEvent：编辑区是原生 QScintilla
 * 子窗口，它自己就是焦点所有者，按键先到它那儿；QML 的 Shortcut 又够不着
 * 原生控件（见 EditorController.h 开头讲 QAction 那一段）。挂在 qApp 上是
 * 唯一一处"焦点在 QML 还是在原生控件都看得见"的位置。
 *
 * **永远返回 false**：这个过滤器一次都不许消费事件。Shift 是打字要用的键，
 * 吃掉一下就是"大写字母打不出来"那种事故。
 */
class DoubleShiftFilter final : public QObject {
public:
    explicit DoubleShiftFilter(EditorController *owner)
        : QObject(owner), m_owner(owner) {}

    bool eventFilter(QObject *, QEvent *event) override {
        const bool press = event->type() == QEvent::KeyPress;
        if (press || event->type() == QEvent::KeyRelease) {
            auto *ke = static_cast<QKeyEvent *>(event);
            if (m_detector.feed(ke->key(), press, ke->isAutoRepeat(),
                                QDateTime::currentMSecsSinceEpoch()))
                m_owner->activateCommand(QStringLiteral("findInFiles"));
        }
        return false;
    }

private:
    EditorController *m_owner;
    DoubleShiftDetector m_detector;
};

#if defined(Q_OS_WIN)

/*
 * Qt 的组合键 -> Windows 的 (修饰键, 虚拟键)。
 *
 * 只认常用的三类：字母、数字、F1-F24。认不出来的（小键盘、媒体键……）就
 * 不注册全局的 —— 程序内那条 QAction 照样能用，只是"后台也能按"这个增强没了。
 */
bool winHotkey(const QKeySequence &seq, UINT *mods, UINT *vk) {
    if (seq.count() != 1)
        return false;
    const QKeyCombination combo = seq[0];
    const Qt::KeyboardModifiers km = combo.keyboardModifiers();
    UINT m = 0;
    if (km & Qt::ControlModifier)
        m |= MOD_CONTROL;
    if (km & Qt::AltModifier)
        m |= MOD_ALT;
    if (km & Qt::ShiftModifier)
        m |= MOD_SHIFT;
    if (km & Qt::MetaModifier)
        m |= MOD_WIN;
    /* 一个修饰键都不带的键不注册全局：那样系统里按一下 A 就开截图了 */
    if (m == 0)
        return false;

    const int k = combo.key();
    UINT v = 0;
    if (k >= Qt::Key_A && k <= Qt::Key_Z)
        v = UINT('A' + (k - Qt::Key_A));
    else if (k >= Qt::Key_0 && k <= Qt::Key_9)
        v = UINT('0' + (k - Qt::Key_0));
    else if (k >= Qt::Key_F1 && k <= Qt::Key_F24)
        v = UINT(VK_F1 + (k - Qt::Key_F1));
    else
        return false;

    *mods = m;
    *vk = v;
    return true;
}

/*
 * 系统级热键的回调。
 *
 * 为什么不用 QAction 自带的快捷键：那个的上下文是 WindowShortcut，只有主窗口
 * 是活动窗口时才响 —— 程序收进托盘 / 被别的窗口压着时按 Ctrl+Alt+A 没反应，
 * 用户报的"最小化之后截图快捷键不能用"就是这个。RegisterHotKey 是系统级的，
 * 前台是谁都收得到，消息走 WM_HOTKEY。
 *
 * 现在有两条（截图 / 新建便签，见 kGlobalHotkeys），按 wParam 里那个 id 分辨，
 * 翻成命令名交给 EditorController —— 走的是和 QAction 完全同一条路
 * （commandRequested -> QML 的 dispatch），所以界面上的行为和点菜单一模一样。
 */
class GlobalHotkeyFilter final : public QAbstractNativeEventFilter {
public:
    explicit GlobalHotkeyFilter(EditorController *owner) : m_owner(owner) {}

    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *) override {
        if (type != QByteArrayLiteral("windows_generic_MSG"))
            return false;
        auto *msg = static_cast<MSG *>(message);
        if (msg->message != WM_HOTKEY)
            return false;

        for (const GlobalHotkeyEntry &entry : kGlobalHotkeys) {
            if (msg->wParam != WPARAM(entry.id))
                continue;
            m_owner->activateCommand(QString::fromLatin1(entry.name));
            return true;
        }
        return false;
    }

private:
    EditorController *m_owner = nullptr;
};

#endif  // Q_OS_WIN


}  // namespace

void EditorController::activateCommand(const QString &name) {
    emit commandRequested(name);
}

void EditorController::applyGlobalHotkey() {
#if defined(Q_OS_WIN)
    if (!m_widget)
        return;
    if (!m_hotkeyFilter) {
        m_hotkeyFilter = new GlobalHotkeyFilter(this);
        qApp->installNativeEventFilter(m_hotkeyFilter);
    }

    const HWND hwnd = reinterpret_cast<HWND>(m_widget->winId());

    /* 先全摘掉再按现在的键位重新注册：改键 / 解绑都走这一条路 */
    for (const GlobalHotkeyEntry &entry : kGlobalHotkeys) {
        if (m_hotkeys.contains(entry.id) && m_hotkeys.value(entry.id)) {
            UnregisterHotKey(hwnd, entry.id);
            m_hotkeys[entry.id] = false;
        }
    }

    for (const GlobalHotkeyEntry &entry : kGlobalHotkeys) {
        UINT mods = 0;
        UINT vk = 0;
        /* 空串 = 用户主动解绑了这个键，那就别注册全局的 */
        const QString key = shortcutFor(QString::fromLatin1(entry.name));
        if (key.isEmpty() || !winHotkey(QKeySequence(key, QKeySequence::PortableText), &mods, &vk))
            continue;

        /*
         * 注册失败不是错误（组合键可能被别的程序占了）：程序内那条 QAction
         * 还在。结果记下来给自检看（globalHotkeyActive / globalHotkeyActiveFor）。
         */
        m_hotkeys[entry.id] =
            RegisterHotKey(hwnd, entry.id, mods | MOD_NOREPEAT, vk) != FALSE;
    }
#endif
}

bool EditorController::globalHotkeyActiveFor(const QString &name) const {
    for (const GlobalHotkeyEntry &entry : kGlobalHotkeys) {
        if (name == QLatin1String(entry.name))
            return m_hotkeys.value(entry.id, false);
    }
    return false;
}

EditorController::~EditorController() {
    /*
     * 回调是挂在 qApp 上的（见 applyGlobalHotkey），本对象先没掉的话它就悬空了
     * —— 系统消息还会往一个已经析构的对象上打。摘干净。
     */
#if defined(Q_OS_WIN)
    if (m_widget) {
        const HWND hwnd = reinterpret_cast<HWND>(m_widget->winId());
        for (const GlobalHotkeyEntry &entry : kGlobalHotkeys) {
            if (m_hotkeys.value(entry.id, false))
                UnregisterHotKey(hwnd, entry.id);
        }
    }
#endif
    if (m_hotkeyFilter) {
        qApp->removeNativeEventFilter(m_hotkeyFilter);
        delete m_hotkeyFilter;
        m_hotkeyFilter = nullptr;
    }
}

EditorController::EditorController(QObject *parent) : QObject(parent) {
    /*
     * 上一次打开文件所在目录。
     *
     * WindowHelper 那边有 QSettings 的话可以直接复用，这里单独存一份
     * 与界面无关的（文件对话框用）。
     */
    m_lastDir = QSettings().value(settingsKey(QStringLiteral("lastDir"))).toString();

    /*
     * 双击 Shift -> 「在文件中查找」。
     *
     * 挂在构造函数而不是 attachWidget()：那个可能被调多次（挂一次就够，
     * 挂两次会把一次双击判成两次）。
     */
    qApp->installEventFilter(new DoubleShiftFilter(this));
}

QString EditorController::settingsKey(const QString &key) {
    return QStringLiteral("editor/") + key;
}

void EditorController::attachWidget(QWidget *widget) {
    m_widget = widget;
    if (m_widget)
        registerShortcuts();
}

/*
 * 快捷键注册表。
 *
 * 只放"QScintilla 自己没有"的组合键：
 *   * 撤销 / 重做 / 剪切 / 复制 / 粘贴 / 全选 交给 Scintilla 自己的键表，
 *     它的处理比我们转发一层更完整（撤销分组、行选区、虚拟空格…）；
 *   * 剩下的文件、查找、视图类命令在这里注册。
 *
 * Alt+Z（自动换行）是主流编辑器的习惯键位，但 Windows 上 Alt+字母
 * 有可能被系统当成菜单助记键吃掉；所以同一条命令在工具栏和"视图"菜单里
 * 都留了入口，不依赖这个键位。
 *
 * 这张表同时是设置面板的"出厂默认"：registerShortcuts() 建完之后
 * restoreShortcuts() 会用 QSettings 里存过的组合键覆盖它。
 */
namespace {

struct ShortcutEntry {
    const char *name;      /* commandRequested 里的名字 */
    const char *label;     /* 设置面板里显示的中文名 */
    const char *group;     /* 设置面板里的分组 */
    const char *sequence;  /* 出厂默认组合键（Qt PortableText） */
};

const ShortcutEntry kShortcutTable[] = {
    {"new",           "新建",       "文件", "Ctrl+N"},
    {"open",          "打开",       "文件", "Ctrl+O"},
    {"save",          "保存",       "文件", "Ctrl+S"},
    {"saveAs",        "另存为",     "文件", "Ctrl+Shift+S"},
    {"shot",          "截图",       "文件", "Ctrl+Alt+A"},
    {"note",          "新建便签",   "文件", "Ctrl+Alt+N"},
    {"notesArrange",  "排列便签",   "文件", ""},
    {"translate",     "翻译卡片",   "文件", "Ctrl+Alt+T"},
    {"saveAll",       "全部保存",   "文件", "Ctrl+Alt+S"},
    {"print",         "打印",       "文件", "Ctrl+P"},
    {"closeTab",      "关闭标签",   "文件", "Ctrl+W"},
    {"nextTab",       "下一个标签", "文件", "Ctrl+Tab"},
    {"prevTab",       "上一个标签", "文件", "Ctrl+Shift+Tab"},
    {"find",          "查找",       "查找", "Ctrl+F"},
    /*
     * 「在文件中查找」那扇弹框（见 qml/components/FileSearchPanel.qml）。
     *
     * 出厂默认**不给组合键**：召唤它的手势是双击 Shift（见 DoubleShiftFilter），
     * 而 Ctrl+Shift+F 这一档在出厂时是"格式化代码"，用户点名不动它。
     * 留这一条在表里是为了让它出现在设置面板的可改键清单里 —— 想要一个
     * 真组合键的人可以自己在那儿绑上，绑了之后 QAction 和手势两条路都通。
     */
    {"findInFiles",   "在文件中查找", "查找", ""},
    {"replace",       "替换",       "查找", "Ctrl+H"},
    {"findNext",      "查找下一个", "查找", "F3"},
    {"findPrev",      "查找上一个", "查找", "Shift+F3"},
    {"goto",          "转到行",     "查找", "Ctrl+G"},
    {"toggleComment", "切换注释",   "编辑", "Ctrl+/"},
    /* Markdown 预览开关（见 qml/components/MarkdownView.qml） */
    {"toggleMarkdownPreview", "Markdown 预览", "编辑", "Ctrl+Shift+V"},
    /* 代码格式化（右键菜单那一条，见 src/Formatter.h） */
    {"formatCode",    "格式化代码", "编辑", "Ctrl+Shift+F"},
    /* 编辑区校验（中文用词 / 代码语法，见 src/Checker.h） */
    {"checkFile",     "校验当前文件", "编辑", "Ctrl+Shift+K"},
    /* 分栏（同一份文档摆在两栏里，见 qml/components/EditorArea.qml） */
    {"splitRight",    "左右分栏",   "视图", "Alt+Shift+2"},
    {"splitDown",     "上下分栏",   "视图", "Alt+Shift+3"},
    {"zoomIn",        "放大",       "视图", "Ctrl+="},
    {"zoomOut",       "缩小",       "视图", "Ctrl+-"},
    {"zoomReset",     "重置缩放",   "视图", "Ctrl+0"},
    {"toggleWrap",    "自动换行",   "视图", "Alt+Z"},
    /* 底部终端面板（见 qml/components/TerminalPanel.qml）；和 VS Code 同一个键 */
    {"toggleTerminal", "终端面板",  "视图", "Ctrl+`"},
};

}  // namespace


void EditorController::registerShortcuts() {
    for (const ShortcutEntry &entry : kShortcutTable) {
        const QString name = QString::fromLatin1(entry.name);
        const QString sequence = QString::fromLatin1(entry.sequence);

        auto *action = new QAction(this);
        action->setShortcut(QKeySequence(sequence, QKeySequence::PortableText));
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this,
                [this, name]() { emit commandRequested(name); });
        m_widget->addAction(action);

        m_actions.insert(name, action);
        m_defaults.insert(name, sequence);
        m_current.insert(name, sequence);
    }

    /*
     * Ctrl+= 在多数键盘上要按 Ctrl+Shift+= 才出得来 "+"，两个都给上，
     * 和主流编辑器一致。
     *
     * 这一条不放进设置面板（它只是 "zoomIn" 的备选键位），
     * 所以不进 m_actions / m_current。
     */
    auto *zoomInPlus = new QAction(this);
    zoomInPlus->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Plus));
    zoomInPlus->setShortcutContext(Qt::WindowShortcut);
    connect(zoomInPlus, &QAction::triggered, this,
            [this]() { emit commandRequested(QStringLiteral("zoomIn")); });
    m_widget->addAction(zoomInPlus);

    restoreShortcuts();
}

/*
 * 把 QSettings 里存过的组合键盖回 QAction。
 *
 * 存的是 PortableText（"Ctrl+Shift+S"），和界面显示、设置面板抓到的键
 * 是同一套写法，换机器 / 换语言都不会因为本地化写法（"Ctrl++"）对不上。
 * 存了空串 = 用户主动解绑，保持无快捷键。
 */
void EditorController::restoreShortcuts() {
    QSettings settings;
    for (auto it = m_actions.constBegin(); it != m_actions.constEnd(); ++it) {
        const QString key = shortcutSettingsKey(it.key());
        if (!settings.contains(key))
            continue;

        const QString sequence = settings.value(key).toString();
        m_current[it.key()] = sequence;
        it.value()->setShortcut(QKeySequence(sequence, QKeySequence::PortableText));
    }

    /* 截图键还要额外注册成系统级的：改键 / 重置最后都汇到这条漏斗上 */
    applyGlobalHotkey();
}


QAction *EditorController::actionFor(const QString &name) const {
    return m_actions.value(name, nullptr);
}

QString EditorController::shortcutSettingsKey(const QString &name) {
    return QStringLiteral("editor/shortcut/") + name;
}

QString EditorController::shortcutFor(const QString &name) const {
    return m_current.value(name);
}

QVariantList EditorController::shortcutItems() const {
    /*
     * 撞键检测：两个动作绑同一个组合键时，QAction 的触发是"谁先注册谁赢"，
     * 后绑的那个实际上按不出来。这里如实标出来（conflict），
     * 让设置面板提醒用户 —— 但不禁用输入，用户可能正打算改掉另一个。
     */
    QHash<QString, int> counts;
    for (auto it = m_current.constBegin(); it != m_current.constEnd(); ++it) {
        if (!it.value().isEmpty())
            counts[it.value()] += 1;
    }

    QVariantList out;
    for (const ShortcutEntry &entry : kShortcutTable) {
        const QString name = QString::fromLatin1(entry.name);
        const QString current = m_current.value(name);

        QVariantMap item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("label"), QString::fromUtf8(entry.label));
        item.insert(QStringLiteral("group"), QString::fromUtf8(entry.group));
        item.insert(QStringLiteral("shortcut"), current);
        item.insert(QStringLiteral("default"), m_defaults.value(name));
        item.insert(QStringLiteral("custom"), current != m_defaults.value(name));
        item.insert(QStringLiteral("conflict"),
                    !current.isEmpty() && counts.value(current) > 1);
        out.append(item);
    }
    return out;
}

QString EditorController::setShortcut(const QString &name, const QString &key) {
    QAction *action = actionFor(name);
    if (!action)
        return tr("未知命令：%1").arg(name);

    const QKeySequence sequence(key, QKeySequence::PortableText);
    /* 空串 = 解绑（用户按了退格想清掉），和"恢复默认"是两回事 */
    const bool cleared = key.isEmpty();
    if (!cleared && sequence.isEmpty())
        return tr("无法识别的组合键：%1").arg(key);

    /*
     * 和别的动作撞键：直接拒绝，而不是"后改的赢"。
     * 静默覆盖会让另一个命令凭空失效，是最难查的一类问题。
     * saveAll 没在表里（菜单里没有），这里也不特判。
     */
    if (!cleared) {
        for (auto it = m_current.constBegin(); it != m_current.constEnd(); ++it) {
            if (it.key() != name && it.value() == key) {
                return tr("%1 已经用在「%2」上了")
                    .arg(key, labelForName(it.key()));
            }
        }
    }

    action->setShortcut(cleared ? QKeySequence() : sequence);
    m_current[name] = key;
    QSettings().setValue(shortcutSettingsKey(name), key);
    emit shortcutsChanged();
    return QString();
}

void EditorController::resetShortcut(const QString &name) {
    QAction *action = actionFor(name);
    if (!action)
        return;

    const QString fallback = m_defaults.value(name);
    action->setShortcut(QKeySequence(fallback, QKeySequence::PortableText));
    m_current[name] = fallback;
    /* 删掉这一条而不是存默认值：以后改默认键位，老用户能跟着变 */
    QSettings().remove(shortcutSettingsKey(name));
    emit shortcutsChanged();
}

void EditorController::resetAllShortcuts() {
    QSettings settings;
    for (auto it = m_actions.constBegin(); it != m_actions.constEnd(); ++it) {
        const QString fallback = m_defaults.value(it.key());
        it.value()->setShortcut(QKeySequence(fallback, QKeySequence::PortableText));
        m_current[it.key()] = fallback;
        settings.remove(shortcutSettingsKey(it.key()));
    }
    emit shortcutsChanged();
}

QString EditorController::labelForName(const QString &name) {
    for (const ShortcutEntry &entry : kShortcutTable) {
        if (name == QLatin1String(entry.name))
            return QString::fromUtf8(entry.label);
    }
    return name;
}

QString EditorController::openFileDialog() {
    const QString start =
        m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir;

    const QString path = QFileDialog::getOpenFileName(
        m_widget, tr("打开文件"), start,
        tr("文本文件 (*.txt *.md *.markdown *.log *.json *.xml *.html *.htm *.css "
           "*.js *.mjs *.ts *.qml *.cpp *.cc *.cxx *.h *.hpp *.c *.cs *.java "
           "*.py *.sql *.sh *.bat *.cmd *.yml *.yaml *.ini *.conf *.properties "
           "*.cmake *.diff *.patch *.tex);;所有文件 (*.*)"));

    if (!path.isEmpty()) {
        m_lastDir = QFileInfo(path).absolutePath();
        QSettings().setValue(settingsKey(QStringLiteral("lastDir")), m_lastDir);
    }
    return path;
}

QString EditorController::saveFileDialog(const QString &suggestedName) {
    const QString start = m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir;
    const QString suggested =
        start + QLatin1Char('/')
        + (suggestedName.isEmpty() ? QStringLiteral("未命名.txt") : suggestedName);

    const QString path = QFileDialog::getSaveFileName(
        m_widget, tr("另存为"), suggested,
        tr("文本文件 (*.txt);;Markdown (*.md);;所有文件 (*.*)"));

    if (!path.isEmpty()) {
        m_lastDir = QFileInfo(path).absolutePath();
        QSettings().setValue(settingsKey(QStringLiteral("lastDir")), m_lastDir);
    }
    return path;
}

QString EditorController::chooseFolderDialog(const QString &title, const QString &startDir) {
    const QString start = !startDir.isEmpty() ? startDir
                         : (m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir);

    const QString dir = QFileDialog::getExistingDirectory(
        m_widget, title.isEmpty() ? tr("选择文件夹") : title, start,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!dir.isEmpty()) {
        m_lastDir = dir;
        QSettings().setValue(settingsKey(QStringLiteral("lastDir")), m_lastDir);
    }
    return dir;
}

QString EditorController::chooseFileDialog(const QString &title, const QString &filter) {
    const QString start = m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir;

    const QString path = QFileDialog::getOpenFileName(
        m_widget, title.isEmpty() ? tr("选择文件") : title, start,
        filter.isEmpty() ? tr("所有文件 (*.*)") : filter);

    if (!path.isEmpty()) {
        m_lastDir = QFileInfo(path).absolutePath();
        QSettings().setValue(settingsKey(QStringLiteral("lastDir")), m_lastDir);
    }
    return path;
}

void EditorController::revealInExplorer(const QString &path) {
    if (path.isEmpty())
        return;

    /*
     * 传文件就打开它所在的目录（并尽量选中它），传目录就直接打开 ——
     * 一个入口应付"在文件夹中显示"和"打开保存位置"两件事。
     */
    const QFileInfo info(path);
    const QString dir = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    if (dir.isEmpty())
        return;

    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

/*
 * Markdown 预览：把原文（加上这份文档所在目录拼出来的绝对图片路径）交给
 * QTextDocument 渲染，取回**它自己吐出来的那份 HTML**。
 *
 * 为什么不自己拼 HTML：QTextDocument 的 markdown 读入器 + toHtml 走的是
 * 同一份文档模型 —— 读进来什么样、吐出来就是什么样，标签是它自己写的，
 * 永远配平。拿第三方 md->html 或者手写替换，迟早在某段怪格式上吐出一份
 * 半截标签，Text 那侧就整段不显示。
 *
 * toHtml() 出来的是 Qt 的"受控 HTML 子集"（只有 p / span / table / img /
 * a 那几样 + 一份基础 CSS），所以不用担心用户文件里塞了什么怪东西 ——
 * 它的原始 HTML 块会被当成纯文本，而不是当成标签执行。
 */

/*
 * QTextDocument 读 markdown 时，遇到 ![](xxx.png) 会**自己去加载那张图** ——
 * 加载不到就把整张图丢掉（HTML 里连 <img> 都没有）。默认那套加载走的是
 * QTextDocument 的资源缓存（要提前 addResource），它不认我们拼出来的
 * file:// 路径，所以预览里图片一律看不见（实测）。
 *
 * 这个子类只干一件事：把资源加载接到**磁盘**上 —— 是个本地文件就用
 * QImage 读出来，读不到返回空（QTextDocument 会当成"图没了"，不影响别的）。
 * 图片引用仍然在 markdown 里就改写成绝对路径（见调用处），这里只是让它
 * 真的能读出来。
 */
class MarkdownPreviewDocument final : public QTextDocument {
public:
    explicit MarkdownPreviewDocument(QObject *parent = nullptr) : QTextDocument(parent) {}

protected:
    QVariant loadResource(int type, const QUrl &name) override {
        if (type == QTextDocument::ImageResource) {
            const QString local = name.isLocalFile() ? name.toLocalFile() : name.toString();
            QImage image;
            if (!local.isEmpty() && image.load(local))
                return image;
        }
        return QTextDocument::loadResource(type, name);
    }
};

QString EditorController::markdownToPreviewHtml(const QString &markdown,
                                                const QString &baseDir) {
    /*
     * 1) 相对图片路径 -> 绝对路径。
     *
     * 笔记里的引用是 `![](assets/xxx.png)` —— 相对的是**这份 md 所在的那个目录**。
     * 笔记目录和程序工作目录没有任何关系，不在这里手动拼绝对路径的话，
     * QTextDocument 会按 JOB 的当前目录去解析，预览里就全是裂图（踩过）。
     * 渲染完之后文档自己的 baseUrl 一律清成空（见下面），所以路径必须在这一步
     * 就落成绝对的。
     */
    static const QRegularExpression kImage(
        QStringLiteral(R"(!\[([^\]]*)\]\(\s*(<[^>]*>|[^)\s]+)((?:\s+(?:"[^"]*"|'[^']*'|\([^)]*\)))?\s*)\))"));

    const QString base = baseDir.trimmed();
    const bool haveBase = !base.isEmpty();

    QString source = markdown;
    if (haveBase) {
        /*
         * 从后往前替换：改一处不会让前面那些匹配的下标失效。
         * 全局匹配是一次性拿全的，所以就算不改下标其实也不会错位，
         * 倒着走只是把这件事做得再明显一点。
         */
        QList<QRegularExpressionMatch> hits;
        auto it = kImage.globalMatch(markdown);
        while (it.hasNext())
            hits.append(it.next());
        for (int i = hits.size() - 1; i >= 0; --i) {
            const QRegularExpressionMatch &m = hits.at(i);
            QString raw = m.captured(2);
            if (raw.startsWith(QLatin1Char('<')) && raw.endsWith(QLatin1Char('>')))
                raw = raw.mid(1, raw.size() - 2);
            /* 已经写死成绝对路径 / URL 的，原样不动 */
            if (raw.isEmpty() || raw.contains(QLatin1String("://"))
                || raw.startsWith(QLatin1String("data:")))
                continue;
            const QString abs = QDir(base).absoluteFilePath(raw);
            source.replace(m.capturedStart(2), m.capturedLength(2),
                           QStringLiteral("<")
                               + QUrl::fromLocalFile(abs).toString(QUrl::FullyEncoded)
                               + QStringLiteral(">"));
        }
    }

    /*
     * 2) 渲染，然后把配色**插进 head 里的那段 <style>**。
     *
     * setDefaultStyleSheet 只在 QTextDocument 自己用的时候生效：toHtml() 出来
     * 的那份 HTML 里**没有**它（实测：head 里只有一段 `p, li { white-space:
     * pre-wrap; }`）。而 QML 那边是个 Text/TextArea，只认行内样式表 ——
     * 不把配色写进去，预览就是默认的白纸黑字，在深色界面上等于什么都看不见。
     *
     * 插在 **</head> 之前**，而不是塞到那一段 <style> 里面：head 里那段是我们
     * 现在拿到的这个字符串的一部分，往里插要处理它自己的转义；另起一段
     * <style> 更简单也更稳（浏览器 / Qt 的富文本都按后者覆盖前者来算）。
     */
    /* 子类：图片从磁盘读（见上面 MarkdownPreviewDocument 的说明） */
    MarkdownPreviewDocument doc;
    doc.setMarkdown(source);
    doc.setBaseUrl(QUrl(QString()));    /* 上面已经落成绝对路径，这里不再兜底解析 */

    QString html = doc.toHtml();

    /*
     * 行内样式必须先把**上下 margin** 摘掉，否则下面那段样式表一条 margin 都不生效。
     *
     * toHtml() 给每一个块都写了 `style=" margin-top:0px; margin-bottom:0px; ..."`，
     * 而行内样式的优先级高于 <style> —— 实测段与段之间只剩 6px（全是行盒自带的余量），
     * 同一个位置 Typora 是 19px。给 CSS 加 !important 也只救回一半（量到 11px）。
     * 只摘 margin-top / margin-bottom：**左右要留着**，列表缩进（-qt-block-indent）
     * 和引用那 40px 的行内 margin-left 还指着它。
     */
    static const QRegularExpression kInlineVMargin(
        QStringLiteral("(margin-top|margin-bottom)\\s*:\\s*[0-9.]+px;?\\s*"));
    html.remove(kInlineVMargin);

    /*
     * 把相邻的 <pre> 并成一块 —— 这才是"代码块中间那道缝"的根因。
     *
     * toHtml() 是**一行代码一个 <pre>**：一份 4 行的围栏出来 4 个块，每个块的底色
     * 只盖住自己那一行的字身，行距（line-height 1.3）多出来的那 6px 余量没人涂底色
     * （实测：行与行之间 6 行没有底色 → 看着就是一根一根的条纹）。
     * 并成一块之后底色是一整张，行距的余量落在块内部，缝就没了 —— 和 Typora
     * 那份 .md-fences 一个连续色块的做法一致。
     *
     * 只并**紧邻**的：两个围栏中间隔着正文/空行时，中间会有 <p>，不会被并到一起。
     * 中间要补的是 `<br />` 而不是换行符 —— 补换行的话 Qt 的 HTML 读入器会把
     * `</span>` 与 `<span>` 之间的空白**折叠掉**，四行塌成一行（量过：文档高
     * 116 → 42，底色行数 76 → 19）。
     */
    static const QRegularExpression kAdjacentPre(
        QStringLiteral("</pre>[\\r\\n]*<pre[^>]*>"));
    html.replace(kAdjacentPre, QStringLiteral("<br />"));

    /*
     * 去掉 toHtml() 写在每个代码 span 上的行内 `font-family:'Courier New'`。
     *
     * 行内样式压过样式表，所以 kMarkdownCss 里 `pre { font-family: Consolas… }`
     * 从来没生效过 —— 抓屏量过：预览里代码 6.97 px/字，那是 Courier New 12px 的宽度
     * （Consolas 14px 实测 7.37）。去掉之后代码跟着样式表走，和编辑区同一套字。
     */
    static const QRegularExpression kCourierSpan(
        QStringLiteral("font-family:'Courier New';[ ]*"));
    html.remove(kCourierSpan);

    /*
     * 每块 <pre> 包进一个 1x1 表格：底色只有表格单元格吃（Qt 的块不吃 padding、
     * 不吃 border、也不吃背景），td 上那串 border-width:0 是压样式表里通用的
     * `td { border-width:1px }` —— 不压掉，代码块外面会多出一圈框线。
     */
    {
        static const QRegularExpression kPreBlock(
            QStringLiteral("(<pre[^>]*>)(.*?)</pre>"),
            QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression kZeroSideMargin(
            QStringLiteral("margin-(left|right):0px;[ ]*"));
        /*
         * 内衬**不靠格子的 padding**：cellpadding、格子的 inline padding、格子的 inline
         * border 三条都单独试过，界面上量出来全是 1px（真机抓屏）。左右靠 <pre> 的
         * margin-left，而它在格子里是**把底色往左撑**（字不动）：表本来占 353..1413
         * （= 正文那一栏），pre 左 margin 13 ⇒ 底色左沿到 340、代码字在 354。
         *
         * 两条**不要再加**的账，都是量过才知道走不通的：
         *   · 表格自己 `margin-left:13px` —— 那会把整张表连底色一起推到正文栏右边
         *     （量到底色左沿 353 vs 标题 342，左边多出一条不该有的缝）；
         *   · pre 的 `margin-right` —— 底色右沿会被撑到正文栏外面去。左右只留左这一笔。
         *
         * 上下两头也不能用 margin：块级 margin 会塌到格子外面（行内写 margin-top:13
         * 之后，底色上沿到第一行字只量到 3px）。**更不能拆成三行表格**：Qt 的格子底色
         * 只画到里面各块自己的矩形，行与行之间会断出一条 7px 的预览底色（真机量到
         * y 385..391）。所以上下各在 <pre> 里加一行空行 —— 它是这个块自己的一行，
         * 底色连着，代价是内衬只能是一整个行高（25 px，Typora 那一档 12）。
         */
        const QString cellWrap = themedCss(QStringLiteral(
            "<table width=\"100%\" cellspacing=\"0\" border=\"0\""
            " style=\"margin-top:%1px; margin-bottom:%2px;\">"
            "<tr><td style=\"background-color:#26282c; border-width:0; border-style:none;\">")
            .arg(kCodeGapTopPx)
            .arg(kCodeGapBottomPx));
        const QString cellEnd = QStringLiteral("</td></tr></table>");
        QString built;
        int last = 0;
        QRegularExpressionMatchIterator it = kPreBlock.globalMatch(html);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            built += html.mid(last, int(m.capturedStart()) - last);
            /*
             * toHtml() 在 <pre> 的行内样式里写了 margin-left:0px / margin-right:0px，
             * 行内压过样式表 —— 而且**同一个 style 串里后写的赢**，我注入的左右 margin
             * 写在前面，不先清掉这两个 0px 就会被它们盖回去。
             * 只清 pre 标签上、值为 0 的那两个：引用的 40px 缩进和列表缩进是**非零**
             * 的，别的块上也指着它们。
             *
             * 同一处注入行高：包进表格之后，样式表里 `pre { line-height }` 那一档在
             * 界面上不落渲染（抓屏量到 15~17 px，Typora 23 px），行内这一份量到了 23 px。
             */
            QString openTag = QString(m.captured(1)).remove(kZeroSideMargin);
            openTag.replace(QStringLiteral("style=\""),
                            QStringLiteral("style=\"margin-left:%1px; line-height:%2; ")
                                .arg(kCodePadPx)
                                .arg(kCodeLineHeight));
            /* 上下内衬 = 块内各一行空行（底色连着，代价是一整个行高） */
            static const QString kPadLine = QStringLiteral("<br />");
            built += cellWrap + openTag + kPadLine + m.captured(2) + kPadLine
                   + QStringLiteral("</pre>") + cellEnd;
            last = int(m.capturedEnd());
        }
        html = built + html.mid(last);
    }

    /*
     * 把 toHtml() 写在 <body> 行内样式里的 `font-size:9pt` 摘掉。
     *
     * 9pt 在 96 DPI 下就是 12px —— 行内样式压过 TextEdit 自己的 font.pixelSize，
     * 所以整份预览（正文和代码一起）都比编辑区小一号（抓屏量到代码 6.95 px/字，
     * 编辑区 Consolas 14px 是 7.37）。摘掉之后字号由 MarkdownView 那个
     * font.pixelSize 定，而它绑的是编辑区生效字号 —— 两边就同步了。
     * 只动 <body ...> 这一个标签：标题那些 `font-size:xx-large` 的行内样式要留着。
     */
    {
        const int bodyAt = html.indexOf(QStringLiteral("<body"));
        const int bodyEnd = html.indexOf(QLatin1Char('>'), bodyAt);
        if (bodyAt >= 0 && bodyEnd > bodyAt) {
            static const QRegularExpression kBodyFontSize(
                QStringLiteral("font-size:[0-9.]+pt;?[ ]*"));
            QString tag = html.mid(bodyAt, bodyEnd - bodyAt + 1);
            tag.remove(kBodyFontSize);
            html.replace(bodyAt, tag.size(), tag);
        }
    }

    const QString styleTag =
        QStringLiteral("<style type=\"text/css\">") + themedCss(QString::fromLatin1(kMarkdownCss))
        + QStringLiteral("</style>");
    const int headEnd = html.indexOf(QStringLiteral("</head>"));
    if (headEnd >= 0)
        html.insert(headEnd, styleTag);
    else
        html.prepend(styleTag);     /* 没有 head（不该发生）：至少别把配色丢了 */

    /*
     * 兜底：图片。
     *
     * Qt 的 markdown 读入器**会把图片丢掉**（实测：`![](a.png)` 读进文档之后
     * toHtml() 里连 <img> 都没有，只留一个空段落；上面那个 loadResource 也救
     * 不回来，因为它压根不去解析那个片段 —— 整个 <img> 在 Qt 6.11 的 markdown
     * 读入器里就没落地）。所以笔记里的插图只能我们自己补。
     *
     * 位置是**近似**的：补出来的图在整篇末尾单列一块（"文档里的插图"），
     * 而不是嵌在正文那一行下面 —— 读入器把图整个丢了，原文里那个位置已经没有
     * 锚点可用了。列表 + 说明至少让用户看得见图，而不是一片空白。
     */
    {
        QList<QRegularExpressionMatch> hits;
        auto it = kImage.globalMatch(source);
        while (it.hasNext())
            hits.append(it.next());

        QString figures;
        for (const QRegularExpressionMatch &m : hits) {
            QString raw = m.captured(2);
            if (raw.startsWith(QLatin1Char('<')) && raw.endsWith(QLatin1Char('>')))
                raw = raw.mid(1, raw.size() - 2);
            if (raw.isEmpty())
                continue;

            /*
             * 拼出最终要写进 src 的那条 URL。没给 baseDir 时相对路径原样保留，
             * 但它没有基准目录可解析 —— 补出来也是裂图，所以跳过。
             */
            QString url;
            if (raw.contains(QLatin1String("://")) || raw.startsWith(QLatin1String("data:")))
                url = raw;
            else if (haveBase)
                url = QUrl::fromLocalFile(QDir(base).absoluteFilePath(raw))
                          .toString(QUrl::FullyEncoded);
            if (url.isEmpty())
                continue;

            const QString alt = m.captured(1);
            /*
             * width 给个上限：笔记里的截图常是整屏的，原尺寸会把预览撑爆。
             * QML 那边的 Text/TextArea 认这个属性。
             */
            figures += QStringLiteral("<p><img src=\"%1\" width=\"520\" />%2</p>")
                           .arg(url.toHtmlEscaped(),
                                alt.isEmpty()
                                    ? QString()
                                    : QStringLiteral("<br /><i>%1</i>").arg(alt.toHtmlEscaped()));
        }
        if (!figures.isEmpty()) {
            const QString block = QStringLiteral("<hr /><p><b>文档里的插图</b></p>") + figures;
            const int bodyEnd = html.lastIndexOf(QStringLiteral("</body>"));
            if (bodyEnd >= 0)
                html.insert(bodyEnd, block);
            else
                html += block;
        }
    }
    return html;
}


/*
 * 预览里点链接（MarkdownView 的 linkActivated -> Main.qml -> 这里）。
 *
 * 只放行这几种协议，其余一律拒绝：预览的内容来自用户自己的文件，
 * 理论上不会有别的东西，但"渲染器放出来什么就照着执行什么"不是个好习惯。
 * 返回有没有真的交出去（自检拿它钉这条白名单）。
 */
bool EditorController::openExternal(const QString &url) {
    const QString trimmed = url.trimmed();
    if (trimmed.isEmpty())
        return false;

    /*
     * 纯本地路径（"C:/x/y.md" 这种）：QUrl 会把它认成 scheme "c"，
     * 按下面的白名单会直接被拒。先按"这是不是一个真实存在的本地路径"试一次。
     */
    const QUrl asUrl(trimmed, QUrl::StrictMode);
    const QString scheme = asUrl.scheme().toLower();

    static const QStringList kAllowed{
        QStringLiteral("http"), QStringLiteral("https"),
        QStringLiteral("mailto"), QStringLiteral("file")};

    if (!scheme.isEmpty() && kAllowed.contains(scheme)) {
        if (scheme == QLatin1String("file") && asUrl.isLocalFile())
            return QDesktopServices::openUrl(QUrl::fromLocalFile(asUrl.toLocalFile()));
        return QDesktopServices::openUrl(asUrl);
    }

    /* 没有协议头：当成本地路径（存在才开，免得对着一串乱码弹系统报错框） */
    if (scheme.isEmpty() || scheme.size() == 1) {
        const QFileInfo info(trimmed);
        if (info.exists())
            return QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
    }
    return false;
}

/*
 * 预览入口（QML 调的就是这个）。
 *
 * 原文是空的就直接返回空串，连渲染都不做：QML 那边拿到空串会显示
 * "没有可预览的内容"，这也让"空文件"和"渲染失败"在界面上是同一句话 ——
 * 而不是一块什么都没有的空白（用户分不清是卡住了还是本来就没内容）。
 */
/*
 * 把围栏代码块里的超长行按像素宽度硬切开，续行补 4 个空格。
 *
 * 为什么要切（而不是让它自然折行）：Qt 的富文本整份文档只有一个换行选项，
 * 没有"按块关换行"的口子 —— 一条 230 字符的日志会被软折成 3 行，续行**顶格**，
 * 看着就像另一条日志。给 pre 加悬挂缩进也试过，量下来不行：能把续行推开的
 * 只有 margin-left，而它会把整块底色一起推开，首行反而跑到色块外面
 * （padding-left 在 Qt 的块上是死的，加与不加逐位相同）。
 *
 * 所以断行只能发生在**内容**上：按 Consolas 14px 的真实字宽切，
 * 续行前面补 4 个空格（pre 里前导空格照原样画出来，这就是那道悬挂缩进）。
 * 只动 ``` / ~~~ 围栏内部的行；正文段落照旧交给 Qt 折。
 * 断点优先落在空格上（日志都是空格分开的字段），一个词本身就超宽才按字硬切。
 *
 * wrapPx 是预览区能放字的像素宽度（0 = 不切，比如窗口还没量出来）；
 * fontPx 是预览实际用的字号 —— 它现在跟编辑区同源，所以字宽要按它量，
 * 不能拿 14px 的度量去套别的档位。
 */
QString wrapLongCodeLines(const QString &markdown, int wrapPx, int fontPx)
{
    if (wrapPx <= 80)
        return markdown;

    QFont codeFont(QStringLiteral("Consolas"));
    codeFont.setPixelSize(qBound(6, fontPx, 72));   // 和 kMarkdownCss 里 pre 那套字体一致
    const QFontMetricsF fm(codeFont);
    const int kContIndentPx = qRound(fm.horizontalAdvance(QLatin1String("    ")));
    /* wrapPx 是预览区的字宽（正文那一栏）。代码块只在**左边**吃掉 kCodePadPx 的内衬
       —— 右边不留（底色右沿要和正文栏右沿对齐），所以只扣一笔 */
    const int firstBudget = wrapPx - kCodePadPx - 2;
    const int restBudget = firstBudget - kContIndentPx;
    if (restBudget <= 0)
        return markdown;

    QStringList out;
    bool inFence = false;
    QString fenceMark;
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        if (t.startsWith(QLatin1String("```")) || t.startsWith(QLatin1String("~~~"))) {
            if (!inFence) {
                inFence = true;
                fenceMark = t.left(3);
            } else if (t.startsWith(fenceMark)) {
                inFence = false;
            }
            out << line;
            continue;
        }
        if (!inFence || fm.horizontalAdvance(line) <= firstBudget) {
            out << line;
            continue;
        }

        int lead = 0;
        while (lead < line.size()
               && (line.at(lead) == QLatin1Char(' ') || line.at(lead) == QLatin1Char('\t')))
            ++lead;
        const QString indent = line.left(lead);
        QString rest = line.mid(lead);
        bool first = true;
        while (!rest.isEmpty()) {
            const QString prefix = first ? indent : indent + QLatin1String("    ");
            const int budget = first ? firstBudget : restBudget;
            double w = fm.horizontalAdvance(prefix);
            int cut = 0;            // 放得下的最后一个下标（含）
            int lastSpace = -1;
            for (int i = 0; i < rest.size(); ++i) {
                const double cw = fm.horizontalAdvance(rest.at(i));
                if (w + cw > budget)
                    break;
                w += cw;
                cut = i + 1;
                if (rest.at(i) == QLatin1Char(' '))
                    lastSpace = i + 1;
            }
            if (cut >= rest.size()) {          // 剩下的整段放得下
                out << prefix + rest;
                break;
            }
            const int brk = lastSpace > 0 ? lastSpace : qMax(1, cut);
            QString piece = prefix + rest.left(brk);
            while (piece.endsWith(QLatin1Char(' ')))
                piece.chop(1);
            out << piece;
            rest = rest.mid(brk);
            first = false;
        }
    }
    return out.join(QLatin1Char('\n'));
}

QString EditorController::markdownHtml(const QString &markdown, const QString &baseDir,
                                       int wrapPx, int fontPx) {
    if (markdown.trimmed().isEmpty())
        return QString();
    return markdownToPreviewHtml(wrapLongCodeLines(markdown, wrapPx, fontPx), baseDir);
}

void EditorController::copyText(const QString &text) {
    if (auto *clip = QGuiApplication::clipboard())
        clip->setText(text);
}

/*
 * 提示 / 确认 / 未保存改动三处的 QMessageBox 都退场了。
 *
 * 它们原来在这里 exec()：模态、系统自绘，弹出时整个程序点不动，长相也和
 * 界面对不上（用户报的是"关闭键那个框和别处不一样"）。现在这三处都是 QML 那侧
 * 的 AskCard —— 一块只占自己一小块的原生小窗，底下的界面原封不动。C++ 只负责
 * 把话转成信号（下面这个），或者由 QML 自己在流程里弹（关闭标签那个队列）。
 * 为什么不能在这里同步等回答，见 qml/components/AskCard.qml 开头。
 */
void EditorController::alert(const QString &title, const QString &text) {
    emit alertRequested(title, text);
}

/*
 * "要用户敲字"的那三类（重命名 / 转到行 / 字数参考线列）以前在这里用
 * QInputDialog 同步 exec()，系统标题栏 + 英文 OK/Cancel，和界面两套观感。
 * 现在它们是 QML 那张带输入框的卡片（qml/components/AskCard.qml 的
 * askInput，调用方在 Main.qml 的 window.askInput）—— 卡片是异步的，
 * 值走回调，所以这里连"拿返回值"的接口都不需要了。
 */

QString EditorController::recall(const QString &key, const QString &fallback) const {    const QVariant value = QSettings().value(settingsKey(key));
    if (!value.isValid())
        return fallback;
    const QString text = value.toString();
    return text.isEmpty() ? fallback : text;
}

void EditorController::remember(const QString &key, const QString &value) const {
    QSettings().setValue(settingsKey(key), value);
}

QString EditorController::fileNameOf(const QString &path) const {
    return QFileInfo(path).fileName();
}
