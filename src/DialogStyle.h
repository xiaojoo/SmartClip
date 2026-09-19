#pragma once

#include <QWidget>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <dwmapi.h>
#endif

/*
 * 只剩一件事：关掉某个窗口自己的系统转场动画。
 *
 * 这里以前还放着 QtWidgets 输入框（QInputDialog）的灰黑皮肤和深色标题栏 ——
 * 那三个"要用户敲字"的框（重命名 / 转到行 / 参考线列）已经换成 QML 那侧的
 * 自绘卡片（qml/components/AskCard.qml 的 askInput，颜色由组件自己写死），
 * C++ 侧不再有输入框，皮肤和 applyDarkTitleBar 一起删了。
 * 还留着的只有下面这个：主窗口最大化 / 还原那段要用（见 src/WindowHelper.cpp）。
 */

/*
 * 关掉这个窗口的**系统转场动画**（最大化 / 还原那一段）。
 *
 * 实测（QtWidgets 那些输入框）：系统给新窗口默认带一段淡入 —— 框的像素要在约
 * 150ms 里才爬满（蓝色图标计数 146 -> 288），用户看到的就是"弹框闪动、像重新
 * 出现了一次"。主窗口那边更明显：最大化时系统会把**上一次那张画面**按新矩形
 * 缩放一遍再交出去，看着就是"窗口先跑到右边、还在放大"。这是纯观感损失
 * （这段动画不提供任何信息），所以整窗关掉它，只关这一个窗口，
 * 不动系统全局的动画设置。
 *
 * 调用时机：窗口句柄得先存在 —— 这里用 winId() 主动建一下，所以在 show() /
 * exec() 之前调也没问题。
 *
 * 返回 HRESULT：**设没设上是要看的**。这个属性只在文档里标了"配合
 * DwmSetWindowAttribute 用"（反向读不了，DwmGetWindowAttribute 会回
 * E_INVALIDARG），所以除了这里记下写的结果，没有别的办法知道它到底生效没有。
 * 排查"最大化那一下还在缩放"时，这一笔是"到底是没设上，还是设上了没用"的判据。
 */
inline HRESULT disableDwmTransitions(QWidget *w) {
#if defined(Q_OS_WIN)
    if (!w)
        return E_INVALIDARG;
#  ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#    define DWMWA_TRANSITIONS_FORCEDISABLED 3
#  endif
    const HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd)
        return E_HANDLE;
    const BOOL off = TRUE;
    return DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
#else
    Q_UNUSED(w);
    return E_NOTIMPL;
#endif
}