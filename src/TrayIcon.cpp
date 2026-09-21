#include "TrayIcon.h"

#include "EditorController.h"
#include "LightMenu.h"
#include "Screenshot.h"
#include "StickyNotes.h"
#include "Translate.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QKeySequence>
#include <QSystemTrayIcon>
#include <QWidget>

TrayIcon::TrayIcon(QWidget *host, Screenshot *shot, EditorController *cmd, StickyNotes *notes,
                   TranslateCards *cards, QObject *parent)
    : QObject(parent), m_host(host) {
    /*
     * 图标用随包的 SVG，不走 QIcon::fromTheme()：Windows 上没有图标主题，
     * fromTheme() 返回的是空图标，托盘上就是一块空白（原来就是这样）。
     * 万一资源没进来（前缀被改过之类），退回主题图标，至少有东西显示。
     *
     * 用主图标 smartclip.svg（折带 S）：托盘那一格在 100% 缩放下就是 16px，
     * 而这个形把 S 的"口"留到了 16/128，16px 上还剩下约 1px 缝、读得出四段横竖，
     * 所以不再需要单独的紧凑档（分档的理由见 CMakeLists.txt 里「应用图标」那段）。
     */
    QIcon icon(QStringLiteral(":/brand/smartclip.svg"));
    if (icon.isNull())
        icon = QIcon::fromTheme(QStringLiteral("edit-paste"));

    m_tray = new QSystemTrayIcon(icon, this);
    m_tray->setToolTip(QStringLiteral("SmartClip — 剪贴板 / 截图 / 便签"));

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

    /*
     * 便签（见 src/StickyNotes.h）。
     *
     * 和截图同一个理由放在托盘里：主窗口收进托盘之后，便签还得能新建、
     * 能一键排列 —— 用户要的就是"收进托盘也能用"。
     *
     * "显示全部便签"是一条**开关**（有摆着的就全收起来，一条都没摆就全叫
     * 出来），所以它不需要单独两条；菜单文案跟着条数走（每次弹出前刷新，
     * 见下面的 aboutToShow）。
     */
    if (notes) {
        QAction *noteAct = m_menu.addAction(QStringLiteral("新建便签"));
        if (cmd) {
            const QString key = cmd->shortcutFor(QStringLiteral("note"));
            if (!key.isEmpty()) {
                noteAct->setShortcut(QKeySequence(key));
                noteAct->setShortcutContext(Qt::WidgetShortcut);
            }
        }
        connect(noteAct, &QAction::triggered, this, [notes]() { notes->createNote(); });

        m_notesToggleAct = m_menu.addAction(QStringLiteral("显示全部便签"));
        connect(m_notesToggleAct, &QAction::triggered, this,
                [notes]() { notes->toggleShowAll(); });

        m_notesArrangeAct = m_menu.addAction(QStringLiteral("排列便签"));
        connect(m_notesArrangeAct, &QAction::triggered, this, [notes]() { notes->arrangeAll(); });
    }

    /*
     * 翻译卡片（见 src/Translate.h）。
     *
     * 只有一张：点它就是"叫出来"——没有就建、有就置前。和图标条那一格、
     * Ctrl+Alt+T 落到的是同一个入口（Trans.showCard）。
     */
    if (cards) {
        QAction *translateAct = m_menu.addAction(QStringLiteral("翻译卡片"));
        if (cmd) {
            const QString key = cmd->shortcutFor(QStringLiteral("translate"));
            if (!key.isEmpty()) {
                translateAct->setShortcut(QKeySequence(key));
                translateAct->setShortcutContext(Qt::WidgetShortcut);
            }
        }
        connect(translateAct, &QAction::triggered, this, [cards]() { cards->showCard(); });
    }

    m_menu.addSeparator();
    QAction *showAct = m_menu.addAction(QStringLiteral("显示主窗口"));
    connect(showAct, &QAction::triggered, this, &TrayIcon::showHost);

    /*
     * 退出相关两条，都是明确入口，不弹模态框。
     *
     * 原来这里是"退出…" + 一个"完全退出 / 收进托盘"的选择框，实测那个框会在
     * 用截图快捷键的场景里挡住程序（模态，看着像卡死），用户要求去掉。
     * 改成菜单里两个条目，选哪个就是哪个。
     */
    QAction *trayHideAct = m_menu.addAction(QStringLiteral("收进托盘"));
    connect(trayHideAct, &QAction::triggered, this, [this]() {
        /* 藏起来不等于退出：托盘图标还在，托盘右键能截图，全局截图热键也还响 */
        if (m_host)
            m_host->hide();
    });

    QAction *quitAct = m_menu.addAction(QStringLiteral("退出 SmartClip"));
    /*
     * 用 `exit(0)` 而不是 `qApp->quit()`：后者只是给 app 发一个 QEvent::Quit，
     * 而 QApplication::event() 收到它会先 closeAllWindows()，**只要还剩一个露着
     * 的顶层窗口就把事件吃掉** —— 桌面上摆着便签时，"退出 SmartClip"每点一次
     * 只收起一块便签、程序不退（详见 WindowHelper::quitApp 那段）。
     * exit(0) 直接结束事件循环，不碰窗口；便签清单在 main.cpp 收尾时落盘。
     */
    connect(quitAct, &QAction::triggered, qApp, []() { QCoreApplication::exit(0); });

    /* 白底（用户指定）；和贴图窗口那个右键菜单共用一份样式，见 LightMenu.h */
    applyLightMenuStyle(&m_menu);

    /*
     * 每次弹出之前刷新便签那两条的状态。
     *
     * "显示全部便签"到底该显示"显示"还是"收起"，以及一条便签都没有时
     * "排列便签"该不该灰掉 —— 这两件事只有弹出那一刻才知道（用户可能在别处
     * 新建 / 关掉了便签）。菜单文案变了才 setText，免得每次弹出都重排菜单。
     */
    connect(&m_menu, &QMenu::aboutToShow, this, [this, notes]() {
        if (!notes)
            return;
        const int visible = notes->visibleCount();
        const int total = notes->count();

        if (m_notesToggleAct) {
            const QString label = visible > 0
                                  ? QStringLiteral("收起全部便签")
                                  : QStringLiteral("显示全部便签");
            if (m_notesToggleAct->text() != label)
                m_notesToggleAct->setText(label);
            m_notesToggleAct->setEnabled(total > 0);
        }
        if (m_notesArrangeAct) {
            /* 一条都没摆着就没什么可排列的（灰掉比点了没反应好） */
            m_notesArrangeAct->setEnabled(visible > 0);
        }
    });

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
