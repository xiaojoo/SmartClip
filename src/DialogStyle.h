#pragma once

#include <QWidget>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <dwmapi.h>
#endif

/*
 * QtWidgets 那些消息框 / 输入框的灰黑皮肤。
 *
 * 为什么在 C++ 里给 QMessageBox 挂样式表，而不是等系统的深色主题：
 * 这些框是 QtWidgets 画的，取色走的是应用调色板；主界面之所以是深色，是因为
 * QML 自己刷的色，跟调色板没关系。所以裸的 QMessageBox 永远是浅灰底，在深色
 * 窗口里像个贴错的补丁（"快捷键"框和"关于"框都是这种）。
 *
 * 颜色取主界面同一套（见 Main.qml 的卡片/边框色），前景色显式写出来 ——
 * 只改背景的话，Fusion 仍会拿浅色的 WindowText 去画正文，深底黑字看不见。
 *
 * 注意只挂在具体的框上（setStyleSheet），别挂 qApp：QFileDialog 是原生对话框，
 * 全局样式表会影响它的布局。
 *
 * 原来是 EditorController.cpp 里的文件局部常量；"退出"那个框是 WindowHelper
 * 弹的，也要同一套皮肤，所以挪出来共用一份（别抄第二份，见 js/EditorMenus.js
 * 开头那段讲的"两份迟早不一致"）。
 */
inline const char *dialogStyle() {
    return R"qss(
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
}

/*
 * 原生标题栏也刷成深色。
 *
 * 对话框的"身子"由 dialogStyle() 刷深色，可**标题栏是系统画的** —— 系统在
 * 浅色模式下它就是白的，跟深色身子拼在一起很突兀（用户就是这么报的）。
 * DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE) 是系统给的正经口子，
 * 比自己画无边框标题栏省事，还能保留系统的 ✕ 和拖动。
 *
 * 调用时机：窗口句柄得先存在 —— 这里用 winId() 主动建一下，所以在 show() /
 * exec() 之前调也没问题。
 */
inline void applyDarkTitleBar(QWidget *w) {
#if defined(Q_OS_WIN)
    if (!w)
        return;
#  ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#    define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#  endif
#  ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#    define DWMWA_TRANSITIONS_FORCEDISABLED 3
#  endif
    const HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd)
        return;
    const BOOL dark = TRUE;
    /* 20 是 Win10 20H1+ 的值，19 是更早版本的；哪个认就用哪个 */
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

    /*
     * 顺带把这个窗口的转场动画关掉。
     *
     * 系统给新窗口默认带一段淡入 —— 实测：框的像素要在约 150ms 里才爬满
     * （蓝色图标计数 146 -> 288），用户看到的就是"弹框闪动、像重新出现了一次"。
     * 这是个一问一答的小框，半透明地慢慢浮出来只有坏处（底下界面透过来），
     * 没有好处。只关这一个窗口，不动系统全局的动画设置。
     */
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &dark, sizeof(dark));
#else
    Q_UNUSED(w);
#endif
}