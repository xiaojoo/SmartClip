#pragma once

#include <QColor>
#include <QMenu>
#include <QPalette>
#include <QString>

/*
 * 把一个 QMenu 弄成**白底**。
 *
 * 全局调色板是整个界面那套深色（见 src/main.cpp 里的 tipPalette），QMenu
 * 默认跟着走 —— 托盘那个右键菜单、贴图窗口的右键菜单都要白底，所以放这儿
 * 一份共用，别在两处各抄一遍样式表（改了一处忘另一处，这种"两份迟早不一致"
 * 的坑见 js/EditorMenus.js 开头那段）。
 *
 * 文字要**一起**压成近黑：只翻背景的话，深色那套浅色文字落到白底上就看不见了
 * —— QMessageBox 那处踩过同样的坑，见 src/EditorController.cpp 的 kDialogStyle。
 *
 * 调色板和样式表都设：样式表保证底色一定是白的（不受当前 widget 风格影响），
 * 调色板补样式表管不到的那几笔（典型的是右边那列快捷键文字）。
 */
inline void applyLightMenuStyle(QMenu *menu) {
    if (!menu)
        return;

    QPalette light = menu->palette();
    light.setColor(QPalette::Window, Qt::white);
    light.setColor(QPalette::WindowText, QColor(0x1f, 0x1f, 0x1f));
    light.setColor(QPalette::Base, Qt::white);
    light.setColor(QPalette::Text, QColor(0x1f, 0x1f, 0x1f));
    light.setColor(QPalette::Button, Qt::white);
    light.setColor(QPalette::ButtonText, QColor(0x1f, 0x1f, 0x1f));
    menu->setPalette(light);

    menu->setStyleSheet(QStringLiteral(R"qss(
QMenu {
    background-color: #ffffff;
    border: 1px solid #d0d0d0;
    padding: 4px 0px;
}
QMenu::item {
    padding: 6px 28px 6px 16px;
    color: #1f1f1f;
}
QMenu::item:selected {
    background-color: #e5f1fb;
    color: #1f1f1f;
}
QMenu::item:disabled {
    color: #9a9a9a;
}
QMenu::separator {
    height: 1px;
    background: #e5e5e5;
    margin: 4px 8px;
}
)qss"));
}
