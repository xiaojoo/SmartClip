#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>

class QAction;
class QWidget;

/*
 * 编辑器命令中枢（QML 单例 Cmd）。
 *
 * 为什么命令要绕这一圈而不是全写在 QML 里：
 *
 *  1) 快捷键。编辑区是**原生子窗口**（QScintilla），键盘焦点在它手里，
 *     QML 的 Shortcut 挂在 QQuickWindow 上，能不能收到要看平台怎么派发。
 *     而 QAction 挂在宿主 QWidget 上、上下文是 WindowShortcut，只要这个
 *     顶层窗口是活动窗口就一定触发 —— 和焦点在 QML 还是原生控件无关。
 *     所以快捷键在这里注册，触发后统一发 commandRequested(name)，由 QML
 *     的分发器执行（QML 那边才知道查找栏这类界面要不要跟着动）。
 *
 *  2) 文件对话框 / 消息框。QFileDialog、QMessageBox 都是 QtWidgets 的东西，
 *     而主窗口本来就是 QWidget，用它们比在 QML 里搭一套对话框可靠得多
 *     （原生对话框、中文按钮、模态关系都是现成的）。
 *
 * 注意这里**不注册** Ctrl+Z / Ctrl+X / Ctrl+C / Ctrl+V / Ctrl+A：
 * Scintilla 自己有一套很完整的键盘处理（撤销分组、行选择、多光标粘贴…），
 * 抢过来只会更差。工具栏按钮走 commandRequested 直接调 EditorView 的方法，
 * 不走键盘映射。因此这几个动作也**不在可改键列表**里（见 shortcutItems()）。
 */
class EditorController final : public QObject {
    Q_OBJECT

    /*
     * 是否处于自检模式（`--self-test`）。
     *
     * QML 侧据此**不要**弹模态框：自检里有一条"故意往不存在的路径写"
     * 的检查项，它会触发 errorOccurred，如果照着平时那样弹 QMessageBox，
     * 就会卡在模态框上、桌面上留一个点不掉的窗口（踩过）。
     */
    Q_PROPERTY(bool selfTestMode READ selfTestMode CONSTANT)

    /*
     * 可改的快捷键清单（设置面板用）。
     *
     * 每项：{ name, label, group, shortcut, default, custom, conflict }
     *   name      命令名（commandRequested 里那个）
     *   label     中文显示名
     *   group     分组（文件 / 编辑 / 查找 / 视图）
     *   shortcut  当前生效的组合键（PortableText，如 "Ctrl+Shift+S"）
     *   default   出厂默认
     *   custom    用户改过（true 时设置面板显示"恢复默认"）
     *   conflict  和别的动作撞了（撞了就不生效，只是存着）
     *
     * 改动通过 setShortcut() / resetShortcut() / resetAllShortcuts()，
     * 落盘在 QSettings 的 editor/shortcut/<name> 下，下次启动
     * restoreShortcuts() 会读回来。
     */
    Q_PROPERTY(QVariantList shortcutItems READ shortcutItems NOTIFY shortcutsChanged)

public:
    explicit EditorController(QObject *parent = nullptr);

    void setSelfTestMode(bool on) { m_selfTestMode = on; }
    bool selfTestMode() const { return m_selfTestMode; }

    /*
     * 主窗口由 main.cpp 创建，QAction 挂在它上面。
     * 挂上之后 QML 侧的快捷键才生效。
     */
    void attachWidget(QWidget *widget);

    /* 文件对话框：返回选中的路径；用户取消返回空串 */
    Q_INVOKABLE QString openFileDialog();
    Q_INVOKABLE QString saveFileDialog(const QString &suggestedName = QString());

    /*
     * "有未保存改动"时的三选一。
     * 返回 0 = 保存，1 = 不保存，2 = 取消。
     */
    Q_INVOKABLE int confirmSave(const QString &name);

    Q_INVOKABLE void alert(const QString &title, const QString &text);
    Q_INVOKABLE bool confirm(const QString &title, const QString &text);

    /* 转到行：让用户填一个行号；取消返回 -1 */
    Q_INVOKABLE int askLineNumber(int maxLine, int currentLine);

    /* 轻量设置持久化（字号 / 自动换行 / 行号 / 上次打开的目录…） */
    Q_INVOKABLE QString recall(const QString &key, const QString &fallback = QString()) const;
    Q_INVOKABLE void remember(const QString &key, const QString &value) const;

    Q_INVOKABLE QString fileNameOf(const QString &path) const;

    /* ---- 快捷键（设置面板） ---- */

    /* 把某个命令改成 key（Qt PortableText，如 "Ctrl+Shift+K"）；空串 = 恢复默认 */
    Q_INVOKABLE QString setShortcut(const QString &name, const QString &key);
    Q_INVOKABLE void resetShortcut(const QString &name);
    Q_INVOKABLE void resetAllShortcuts();

    /*
     * name -> 当前组合键；没登记的返回空串。
     * QML 的菜单要显示"用户改过的"快捷键，就查这个。
     */
    Q_INVOKABLE QString shortcutFor(const QString &name) const;

    QVariantList shortcutItems() const;

signals:
    /* 快捷键被按下；name 见 EditorController.cpp 里的注册表 */
    void commandRequested(const QString &name);

    /* 快捷键清单有变化（改键 / 恢复默认），设置面板和菜单据此重读 */
    void shortcutsChanged();

private:
    void registerShortcuts();

    /* 把 QSettings 里存过的组合键盖回 QAction */
    void restoreShortcuts();

    QAction *actionFor(const QString &name) const;

    QWidget *m_widget = nullptr;
    QHash<QString, QAction *> m_actions;
    /* name -> 出厂默认组合键（PortableText），"恢复默认"用 */
    QHash<QString, QString> m_defaults;
    /* name -> 当前生效组合键（PortableText），改键后即时更新 */
    QHash<QString, QString> m_current;
    QString m_lastDir;
    bool m_selfTestMode = false;

    static QString settingsKey(const QString &key);
    static QString shortcutSettingsKey(const QString &name);
    /* name -> 设置面板里的中文名（撞键提示要用） */
    static QString labelForName(const QString &name);
};
