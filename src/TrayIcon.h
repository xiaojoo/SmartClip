#pragma once

#include <QMenu>
#include <QObject>

class QSystemTrayIcon;
class QWidget;
class EditorController;
class Screenshot;

/*
 * 托盘图标（任务栏右下角那个）。
 *
 * 右键菜单：截图 / 显示主窗口 / 退出。截图这一条的用处是 —— 主窗口被别的
 * 程序压着、或者缩在一边时，不用先把它叫出来就能开截图（Ctrl+Alt+A 只在
 * 主窗口是活动窗口时才响）。
 *
 * 为什么单独一个类、而不是继续写在 main() 里：自检要验"菜单里到底有没有
 * 截图这一条、连没连上"（见 src/SelfTest.cpp）。托盘图标本身点不出来
 * （Windows 会把它塞进"隐藏的图标"浮出区，位置随图标数量变），但菜单对象
 * 是可以直接拿的 —— menu() 就是为这个留的。main() 里那段匿名代码没这个口子。
 */
class TrayIcon final : public QObject {
    Q_OBJECT

public:
    TrayIcon(QWidget *host, Screenshot *shot, EditorController *cmd, QObject *parent = nullptr);

    /* 右键菜单（自检用；不拥有它，别删） */
    QMenu *menu() { return &m_menu; }

private:
    void showHost();

    QWidget *m_host = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu m_menu;
};
