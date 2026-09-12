#include "EditorController.h"

#include <QAction>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeySequence>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QWidget>

namespace {

/*
 * 消息框的灰黑皮肤。
 *
 * 为什么在 C++ 里给 QMessageBox 挂样式表，而不是等系统的深色主题：
 * 这些框是 QtWidgets 画的，取色走的是应用调色板；主界面之所以是深色，是因为
 * QML 自己刷的色，跟调色板没关系。所以裸的 QMessageBox 永远是浅灰底，在深色
 * 窗口里像个贴错的补丁（"快捷键"框和"关于"框都是这种）。
 *
 * 颜色取主界面同一套（见 Main.qml 的卡片/边框色），前景色显式写出来 ——
 * 只改背景的话，Fusion 仍会拿浅色的 WindowText 去画正文，深底黑字看不见。
 *
 * 注意只挂在 QMessageBox 自身上，别挂 qApp：QFileDialog 是原生对话框，
 * 全局样式表会影响它的布局。
 */
const char *const kDialogStyle = R"qss(
QMessageBox {
    background-color: #2b2d30;
}
QMessageBox QLabel {
    color: #e6e8ea;
    background: transparent;
}
QMessageBox QPushButton {
    color: #e6e8ea;
    background-color: #3a3e42;
    border: 1px solid #4b4d4f;
    border-radius: 3px;
    padding: 4px 14px;
    min-width: 64px;
}
QMessageBox QPushButton:hover {
    background-color: #45494e;
}
QMessageBox QPushButton:pressed {
    background-color: #313438;
}
QMessageBox QPushButton:default {
    border: 1px solid #c8503c;
}
QInputDialog {
    background-color: #2b2d30;
}
QInputDialog QLabel {
    color: #e6e8ea;
    background: transparent;
}
QInputDialog QSpinBox {
    color: #e6e8ea;
    background-color: #1e2023;
    border: 1px solid #4b4d4f;
    padding: 3px 6px;
}
QInputDialog QPushButton {
    color: #e6e8ea;
    background-color: #3a3e42;
    border: 1px solid #4b4d4f;
    border-radius: 3px;
    padding: 4px 14px;
    min-width: 64px;
}
QInputDialog QPushButton:hover {
    background-color: #45494e;
}
QInputDialog QPushButton:pressed {
    background-color: #313438;
}
)qss";

}  // namespace

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

int EditorController::confirmSave(const QString &name) {
    QMessageBox box(m_widget);
    box.setStyleSheet(QString::fromLatin1(kDialogStyle));  /* 灰黑底，见文件头的说明 */
    box.setWindowTitle(tr("SmartClip"));
    box.setIcon(QMessageBox::Question);
    box.setText(tr("“%1”有未保存的修改。").arg(name.isEmpty() ? tr("当前文件") : name));
    box.setInformativeText(tr("要保存这些修改吗？"));

    QPushButton *saveButton = box.addButton(tr("保存"), QMessageBox::AcceptRole);
    box.addButton(tr("不保存"), QMessageBox::DestructiveRole);
    QPushButton *cancelButton = box.addButton(tr("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(saveButton);
    box.setEscapeButton(cancelButton);
    box.exec();

    if (box.clickedButton() == saveButton)
        return 0;
    if (box.clickedButton() == cancelButton)
        return 2;
    return 1;
}

void EditorController::alert(const QString &title, const QString &text) {
    QMessageBox box(m_widget);
    box.setStyleSheet(QString::fromLatin1(kDialogStyle));  /* 灰黑底，见文件头的说明 */
    box.setWindowTitle(title.isEmpty() ? tr("SmartClip") : title);
    box.setIcon(QMessageBox::Warning);
    box.setText(text);
    box.addButton(tr("知道了"), QMessageBox::AcceptRole);
    box.exec();
}

bool EditorController::confirm(const QString &title, const QString &text) {
    QMessageBox box(m_widget);
    box.setStyleSheet(QString::fromLatin1(kDialogStyle));  /* 灰黑底，见文件头的说明 */
    box.setWindowTitle(title.isEmpty() ? tr("SmartClip") : title);
    box.setIcon(QMessageBox::Question);
    box.setText(text);
    QPushButton *yes = box.addButton(tr("确定"), QMessageBox::AcceptRole);
    QPushButton *no = box.addButton(tr("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(yes);
    box.setEscapeButton(no);
    box.exec();
    return box.clickedButton() == yes;
}

int EditorController::askLineNumber(int maxLine, int currentLine) {
    QInputDialog dialog(m_widget);
    dialog.setStyleSheet(QString::fromLatin1(kDialogStyle));  /* 灰黑底，见文件头的说明 */
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
