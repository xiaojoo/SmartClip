#pragma once

#include <QMenu>
#include <QObject>

class QSystemTrayIcon;
class QWidget;
class EditorController;
class Screenshot;
class StickyNotes;

/*
 * 托盘图标（任务栏右下角那个）。
 *
 * 右键菜单：截图 / 便签（新建 / 显示全部 / 排列）/ 显示主窗口 / 收进托盘 / 退出。
 * 前两组放在这里的用处是 —— 主窗口被别的程序压着、或者缩在一边时，不用先把它
 * 叫出来就能截图、就能开一块便签（截图那个全局热键 Ctrl+Alt+A 只在主窗口是活动
 * 窗口时才响，而"新建便签"的全局热键是系统级的）。
 *
 * 为什么单独一个类、而不是继续写在 main() 里：自检要验"菜单里到底有没有截图 /
 * 便签这几条、连没连上"（见 src/SelfTest.cpp）。托盘图标本身点不出来
 * （Windows 会把它塞进"隐藏的图标"浮出区，位置随图标数量变），但菜单对象
 * 是可以直接拿的 —— menu() 就是为这个留的。main() 里那段匿名代码没这个口子。
 */
class TrayIcon final : public QObject {
    Q_OBJECT

public:
    TrayIcon(QWidget *host, Screenshot *shot, EditorController *cmd, StickyNotes *notes = nullptr,
             QObject *parent = nullptr);

    /* 右键菜单（自检用；不拥有它，别删） */
    QMenu *menu() { return &m_menu; }

private:
    void showHost();

    QWidget *m_host = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu m_menu;
    /*
     * 便签那两条（"显示全部 / 收起全部"是一条开关，文案跟着总条数变；
     * "排列便签"在一条都没摆着时要灰掉）。见构造函数里 aboutToShow 那一段。
     */
    QAction *m_notesToggleAct = nullptr;
    QAction *m_notesArrangeAct = nullptr;
};
