#include "EditorController.h"

#include "DialogStyle.h"

#include <QAbstractNativeEventFilter>
#include <QAction>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeySequence>
#include <QSettings>
#include <QUrl>
#include <QWidget>

#if defined(Q_OS_WIN)
#  include <windows.h>
#endif

namespace {

#if defined(Q_OS_WIN)

/* 系统级热键的 id：WM_HOTKEY 靠它分辨是哪一个（本程序只注册这一个） */
constexpr int kShotHotkeyId = 0x5C01;

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
 */
class ShotHotkeyFilter final : public QAbstractNativeEventFilter {
public:
    explicit ShotHotkeyFilter(EditorController *owner) : m_owner(owner) {}

    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *) override {
        if (type != QByteArrayLiteral("windows_generic_MSG"))
            return false;
        auto *msg = static_cast<MSG *>(message);
        if (msg->message != WM_HOTKEY || msg->wParam != kShotHotkeyId)
            return false;
        m_owner->activateCommand(QStringLiteral("shot"));
        return true;
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
        m_hotkeyFilter = new ShotHotkeyFilter(this);
        qApp->installNativeEventFilter(m_hotkeyFilter);
    }

    const HWND hwnd = reinterpret_cast<HWND>(m_widget->winId());
    if (m_hotkeyRegistered) {
        UnregisterHotKey(hwnd, kShotHotkeyId);
        m_hotkeyRegistered = false;
    }

    UINT mods = 0;
    UINT vk = 0;
    /* 空串 = 用户主动解绑了这个键，那就别注册全局的 */
    const QString key = shortcutFor(QStringLiteral("shot"));
    if (key.isEmpty() || !winHotkey(QKeySequence(key, QKeySequence::PortableText), &mods, &vk))
        return;

    /*
     * 注册失败不是错误（组合键可能被别的程序占了）：程序内那条 QAction 还在。
     * 结果记下来给自检看（globalHotkeyActive）。
     */
    m_hotkeyRegistered = RegisterHotKey(hwnd, kShotHotkeyId, mods | MOD_NOREPEAT, vk) != FALSE;
#endif
}

EditorController::~EditorController() {
    /*
     * 回调是挂在 qApp 上的（见 applyGlobalHotkey），本对象先没掉的话它就悬空了
     * —— 系统消息还会往一个已经析构的对象上打。摘干净。
     */
#if defined(Q_OS_WIN)
    if (m_widget && m_hotkeyRegistered)
        UnregisterHotKey(reinterpret_cast<HWND>(m_widget->winId()), kShotHotkeyId);
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
    {"zoomIn",        "放大",       "视图", "Ctrl+="},
    {"zoomOut",       "缩小",       "视图", "Ctrl+-"},
    {"zoomReset",     "重置缩放",   "视图", "Ctrl+0"},
    {"toggleWrap",    "自动换行",   "视图", "Alt+Z"},
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

QString EditorController::askText(const QString &title, const QString &label,
                                  const QString &text) {
    QInputDialog dialog(m_widget);
    dialog.setStyleSheet(QString::fromLatin1(dialogStyle()));  /* 灰黑底，见文件头的说明 */
    dialog.ensurePolished();
    dialog.adjustSize();
    applyDarkTitleBar(&dialog);
    dialog.setWindowTitle(title.isEmpty() ? tr("SmartClip") : title);
    dialog.setLabelText(label);
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setTextValue(text);
    if (dialog.exec() != QDialog::Accepted)
        return QString();
    return dialog.textValue().trimmed();
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

int EditorController::askLineNumber(int maxLine, int currentLine) {
    QInputDialog dialog(m_widget);
    dialog.setStyleSheet(QString::fromLatin1(dialogStyle()));  /* 灰黑底，见文件头的说明 */
    dialog.ensurePolished();
    dialog.adjustSize();
    applyDarkTitleBar(&dialog);
    dialog.setWindowTitle(tr("转到行"));
    dialog.setLabelText(tr("行号（1 - %1）：").arg(qMax(1, maxLine)));
    dialog.setInputMode(QInputDialog::IntInput);
    dialog.setIntRange(1, qMax(1, maxLine));
    dialog.setIntStep(1);
    dialog.setIntValue(qBound(1, currentLine, qMax(1, maxLine)));
    if (dialog.exec() != QDialog::Accepted)
        return -1;
    return dialog.intValue();
}

int EditorController::askRulerColumn(int current) {
    QInputDialog dialog(m_widget);
    dialog.setStyleSheet(QString::fromLatin1(dialogStyle()));  /* 灰黑底，见文件头的说明 */
    dialog.ensurePolished();
    dialog.adjustSize();
    applyDarkTitleBar(&dialog);
    dialog.setWindowTitle(tr("字数参考线"));
    dialog.setLabelText(tr("在第几个字后面画竖线（1 - 500）："));
    dialog.setInputMode(QInputDialog::IntInput);
    dialog.setIntRange(1, 500);
    dialog.setIntStep(1);
    dialog.setIntValue(qBound(1, current, 500));
    if (dialog.exec() != QDialog::Accepted)
        return -1;
    return dialog.intValue();
}

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
