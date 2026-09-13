#include "TrayIcon.h"

#include "EditorController.h"
#include "LightMenu.h"
#include "Screenshot.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QKeySequence>
#include <QSystemTrayIcon>
#include <QWidget>

TrayIcon::TrayIcon(QWidget *host, Screenshot *shot, EditorController *cmd, QObject *parent)
    : QObject(parent), m_host(host) {
    /*
     * 图标用随包的 SVG，不走 QIcon::fromTheme()：Windows 上没有图标主题，
     * fromTheme() 返回的是空图标，托盘上就是一块空白（原来就是这样）。
     * 万一资源没进来（前缀被改过之类），退回主题图标，至少有东西显示。
     */
    QIcon icon(QStringLiteral(":/icons/image.svg"));
    if (icon.isNull())
        icon = QIcon::fromTheme(QStringLiteral("edit-paste"));

    m_tray = new QSystemTrayIcon(icon, this);
    m_tray->setToolTip(QStringLiteral("SmartClip — 剪贴板 / 截图"));

    QAction *shotAct = m_menu.addAction(QStringLiteral("截图…"));
    /*
     * 快捷键照抄设置面板里当前生效的那个（用户改过键就以改过的为准），
     * 但上下文设成 WidgetShortcut：菜单里只是把它**显示**出来，别再注册成一个
     * 窗口级快捷键 —— 那会和 Cmd 里那个 Ctrl+Alt+A 撞成 "ambiguous shortcut"，
     * 两边都时灵时不灵。
     */
    if (cmd) {
        const QString key = cmd->shortcutFor(QStringLiteral("shot"));
        if (!key.isEmpty()) {
            shotAct->setShortcut(QKeySequence(key));
            shotAct->setShortcutContext(Qt::WidgetShortcut);
        }
    }
    if (shot)
        connect(shotAct, &QAction::triggered, shot, &Screenshot::beginCapture);

    m_menu.addSeparator();
    QAction *showAct = m_menu.addAction(QStringLiteral("显示主窗口"));
    connect(showAct, &QAction::triggered, this, &TrayIcon::showHost);

    QAction *quitAct = m_menu.addAction(QStringLiteral("退出 SmartClip"));
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);

    /* 白底（用户指定）；和贴图窗口那个右键菜单共用一份样式，见 LightMenu.h */
    applyLightMenuStyle(&m_menu);

    m_tray->setContextMenu(&m_menu);

    /* 左键点托盘图标 = 把主窗口叫到前面（右键才是菜单） */
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger)
                    showHost();
            });

    m_tray->show();
}

void TrayIcon::showHost() {
    if (!m_host)
        return;
    m_host->show();
    m_host->raise();
    m_host->activateWindow();
}
