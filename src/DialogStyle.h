#pragma once

#include <QWidget>
#include <QOperatingSystemVersion>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <dwmapi.h>
#endif

/*
 * 只剩两件事：关掉某个窗口自己的系统转场动画，和让主窗口走系统那套抗锯齿圆角。
 *
 * 这里以前还放着 QtWidgets 输入框（QInputDialog）的灰黑皮肤和深色标题栏 ——
 * 那三个"要用户敲字"的框（重命名 / 转到行 / 参考线列）已经换成 QML 那侧的
 * 自绘卡片（qml/components/AskCard.qml 的 askInput，颜色由组件自己写死），
 * C++ 侧不再有输入框，皮肤和 applyDarkTitleBar 一起删了。
 * 还留着的只有下面这两个：主窗口最大化 / 还原那段要用（见 src/WindowHelper.cpp）。
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

/*
 * 让这个窗口走系统那套**带抗锯齿的圆角**（Windows 11 22621 起）。
 *
 * 为什么要换掉自己裁：setMask() 落到 Windows 上是 SetWindowRgn，那是**1-bit 的区域** ——
 * 每个像素要么全在、要么全不在，物理上不存在抗锯齿。半径 10 的弧在 4K@100%（dpr=1.0）
 * 上就是 4~5 级硬台阶。实测（真桌面截图逐行量左上角）：
 *   自己裁   边界 255 -> 73 一步到位，中间 0 级灰，最大一跨 4px；
 *   系统裁   边界 255 -> 247 -> 244 -> 210 -> 147 -> 108 -> 71 -> 51，最大一跨 2px。
 *
 * 两条属性必须**一起**设，都是量出来的，少一条就不成立：
 *   * DWMWA_BORDER_COLOR 不许是 DWMWA_COLOR_NONE —— 设成 NONE 会把圆角一起关掉
 *     （实测：先设 ROUND 再设 COLOR_NONE，量出来和根本没设过一模一样）；
 *   * 必须在**第一次最大化之前**设好。先最大化、再设 ROUND 完全不生效
 *     （两种顺序都量过：设后最大化 = 平滑；最大化后设 = 还是那 4~5 级硬台阶）。
 *     所以调用点只能是一次性的（WindowHelper::attachWidget，show 之前），
 *     不能挂在 Resize / 最大化分支里补。
 *
 * 代价：半径是系统锁死的，100% 缩放下实测约 8px，**改不了**（原来是 10）。
 *
 * 返回 HRESULT：S_OK 才代表圆角那条**写上了并且读回来了**，调用方据此决定还要不要自己裁。
 * Windows 10 / 22621 之前会在这里直接返回 E_NOTIMPL，走遮罩那条老路。
 *
 * 校验只能校一半：圆角那条（33）能 DwmGetWindowAttribute 读回来核对；
 * **边框色那条（34）是只写不可读的** —— 实测读回直接回 E_FAIL（0x80004005），
 * 和 DWMWA_TRANSITIONS_FORCEDISABLED 一个脾气。所以 34 只能信写入的返回值，
 * 它到底画没画出来得看屏幕（自检里靠"描边已经退场、边上仍然有那条线"来判）。
 */
inline HRESULT applyDwmRoundedCorners(QWidget *w, COLORREF borderRgb) {
#if defined(Q_OS_WIN)
#  ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#    define DWMWA_WINDOW_CORNER_PREFERENCE 33
#  endif
#  ifndef DWMWA_BORDER_COLOR
#    define DWMWA_BORDER_COLOR 34
#  endif
#  ifndef DWMWCP_ROUND
#    define DWMWCP_ROUND 2
#  endif
    static const QOperatingSystemVersion kFirstWithBorderAndRound(
        QOperatingSystemVersion::Windows, 10, 0, 22621);
    if (!w || QOperatingSystemVersion::current() < kFirstWithBorderAndRound)
        return E_NOTIMPL;
    const HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd)
        return E_HANDLE;
    const DWORD round = DWMWCP_ROUND;
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                       &round, sizeof(round));
    if (FAILED(hr))
        return hr;
    hr = DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderRgb, sizeof(borderRgb));
    if (FAILED(hr))
        return hr;
    DWORD back = 0;
    hr = DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &back, sizeof(back));
    return (SUCCEEDED(hr) && back == round) ? S_OK : E_FAIL;
#else
    Q_UNUSED(w);
    Q_UNUSED(borderRgb);
    return E_NOTIMPL;
#endif
}