#include "EditorController.h"
#include "Theme.h"


#include <QAbstractNativeEventFilter>
#include <QAction>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
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
 * 给的是 QTextDocument 的 defaultStyleSheet —— 也就是**渲染时**用的样式，
 * 而不是渲染完再往上糊一层（toHtml() 会把 defaultStyleSheet 原样带出去，
 * 所以预览那边的 Text 拿到的就已经是这份配色了）。
 *
 * 为什么每一处颜色都要写死：QML 的 Text 只认行内的这些属性，它**不读**
 * QTextDocument 那一套调色板。默认样式表是一份白纸黑字，留着不管的话，
 * 深色界面上就是一片黑字（几乎等于什么都没显示）。颜色值和界面其它地方
 * 对齐：正文 / 标题用 #d6d7da 系，次要文字 #9aa0a6，代码块底 #26282c。
 *
 * 段间距用 em 而不是 px：字号是用户在设置里能改的，跟着字号缩放才协调。
 */
const char *const kMarkdownCss = R"CSS(
body { color: #d6d7da; }
p { color: #d6d7da; margin-top: 0.45em; margin-bottom: 0.45em; }
h1, h2, h3, h4, h5, h6 { color: #e8e8e8; font-weight: bold;
                         margin-top: 0.9em; margin-bottom: 0.4em; }
h1 { font-size: 1.7em; }
h2 { font-size: 1.45em; }
h3 { font-size: 1.25em; }
h4 { font-size: 1.12em; }
h5, h6 { font-size: 1em; }
a { color: #4c96d8; }
code { font-family: Consolas, "Courier New", monospace; color: #d7ba7d; }
pre { font-family: Consolas, "Courier New", monospace; color: #d6d7da;
      background-color: #26282c; }
blockquote { color: #9aa0a6; margin-left: 1.2em; }
li { color: #d6d7da; }
table { border-width: 1px; border-style: solid; border-color: #4b4d4f; }
th { background-color: #2b2d30; color: #e8e8e8; font-weight: bold;
     border-width: 1px; border-style: solid; border-color: #4b4d4f; }
td { border-width: 1px; border-style: solid; border-color: #4b4d4f; }
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
QString EditorController::markdownHtml(const QString &markdown, const QString &baseDir) {
    if (markdown.trimmed().isEmpty())
        return QString();
    return markdownToPreviewHtml(markdown, baseDir);
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
