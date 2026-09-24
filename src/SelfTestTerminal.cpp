/* NOMINMAX 必须在任何头之前（Qt 的头自己会带 windows.h，晚了就防不住 min/max 宏） */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "SelfTest.h"

#include "EditorController.h"
#include "EditorViewItem.h"

#include "TerminalEngine.h"
#include "TerminalView.h"
#include "Theme.h"

#include <QCoreApplication>
#include <QCursor>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMap>
#include <QMetaObject>
#include <QQuickWidget>
#include <QSettings>
#include "Theme.h"
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QPair>
#include <QApplication>
#include <QWidget>
#include <QThread>
#include <QVariant>

#include <functional>

/*
 * 底部终端那一节的自检（`SmartClip.exe --terminal-test`，见 SelfTest.h 的 runTerminal）。
 *
 * 分两层，缺一不可：
 *
 *   第一层**不起 shell**，直接往引擎灌一段"终端输出字节流"，然后读网格。
 *   这一层钉的是转义解析对不对：中文宽字符占两列、真彩色、自动换行、回滚、
 *   备用屏、清屏、以及"对面问光标在哪我们要答"。这些都能确定性地复现，
 *   所以断言可以写到像素/格子一级 —— 换行差一格、宽字符当成一格，这里就会红。
 *
 *   第二层**真起一个 PowerShell**（真 ConPTY、真进程），跑真命令，等真输出落进网格。
 *   第一层全绿也**不代表**第二层能用：管道没接对、环境块没生效、UTF-8 在
 *   ConPTY 那一圈变成乱码、面板拖高之后对面还按旧宽度排版 —— 这些只有真进程会报。
 *   所以这一层不 mock，跑完把耗时和字节数打出来。
 *
 * 时序：全部走 waitUntil()（processEvents + 轮询），不 sleep 固定时长 ——
 * 这台机器忙的时候 PowerShell 起得慢，写死的等待要么假失败要么真超时。
 */

namespace {

int gTermPassed = 0;
int gTermFailed = 0;

void tcheck(bool ok, const QString &what, const QString &detail = QString())
{
    if (ok) {
        ++gTermPassed;
        std::fputs("  ok    ", stdout);
    } else {
        ++gTermFailed;
        std::fputs("  FAIL  ", stdout);
    }
    std::fputs(what.toUtf8().constData(), stdout);
    if (!detail.isEmpty()) {
        std::fputs("   [", stdout);
        std::fputs(detail.toUtf8().constData(), stdout);
        std::fputs("]", stdout);
    }
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

void tout(const QString &line)
{
    std::fputs(line.toUtf8().constData(), stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

/* 转一圈事件循环等条件成立（终端的输出是异步落进来的，不能直接读） */
bool waitUntil(const std::function<bool()> &pred, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (pred())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(10);
    }
    return pred();
}

/* 屏 + 回滚拼成一整块文本，找关键字用 */
QString allText(TerminalEngine *e)
{
    QString out;
    for (int r = -e->historyRows(); r < e->rows(); ++r) {
        out += e->lineText(r);
        out += QLatin1Char('\n');
    }
    return out;
}

QChar cellChar(TerminalEngine *e, int row, int col)
{
    TerminalCell cell;
    if (!e->cellAt(row, col, &cell) || cell.charCount == 0)
        return QChar();
    return QChar(cell.chars[0]);
}

// ------------------------------------------------------------------ 第一层

void runEmulatorChecks()
{
    tout(QStringLiteral("\n-- 网格仿真（不起 shell）--"));

    auto *e = new TerminalEngine;
    e->setSize(20, 6);

    e->feedBytesForTest("hello");
    tcheck(e->lineText(0) == QLatin1String("hello"),
           QStringLiteral("纯文本落进第 0 行"), e->lineText(0));

    e->feedBytesForTest("\x1b[3;5HZ");
    tcheck(cellChar(e, 2, 4) == QLatin1Char('Z'),
           QStringLiteral("CSI 3;5H 之后写进去的 Z 在第 3 行第 5 列"),
           QStringLiteral("那一格是「%1」").arg(cellChar(e, 2, 4)));

    /* 真彩色：SGR 38;2;r;g;b 要解析成那个 RGB，复位之后回到主题默认色 */
    e->feedBytesForTest("\x1b[?25h\x1b[1;1H\x1b[38;2;10;20;30mR\x1b[0m");
    TerminalCell red;
    e->cellAt(0, 0, &red);
    tcheck(red.fg.red() == 10 && red.fg.green() == 20 && red.fg.blue() == 30,
           QStringLiteral("真彩色 38;2;10;20;30 解析正确"),
           QStringLiteral("得到 %1,%2,%3").arg(red.fg.red()).arg(red.fg.green())
               .arg(red.fg.blue()));
    e->feedBytesForTest("\x1b[1;1H\x1b[39mN");
    TerminalCell plain;
    e->cellAt(0, 0, &plain);
    tcheck(plain.fg == e->defaultFg(), QStringLiteral("SGR 39 复位回默认前景色"));

    /* 256 色：只要求"不是默认色"——具体那 256 个 RGB 是 libvterm 的调色板，钉死它没意义 */
    e->feedBytesForTest("\x1b[1;1H\x1b[38;5;196mY");
    TerminalCell idx;
    e->cellAt(0, 0, &idx);
    tcheck(idx.fg != e->defaultFg(), QStringLiteral("256 色 38;5;196 生效"));

    /*
     * 中文宽字符：一个汉字占**两格**，第二格是尾巴（width==0，不画）。
     * 这条错了的表现是"中文那一行整体往右跑"，也就是用户最先看见的那个错位。
     */
    e->feedBytesForTest("\x1b[2J\x1b[H");
    e->feedBytesForTest(QString::fromUtf8("中文").toUtf8());
    TerminalCell a, b, c;
    e->cellAt(0, 0, &a);
    e->cellAt(0, 1, &b);
    e->cellAt(0, 2, &c);
    tcheck(a.width == 2 && b.width == 0 && c.width == 2,
           QStringLiteral("两个汉字各占两格（第二格是尾巴）"),
           QStringLiteral("width = %1 / %2 / %3").arg(a.width).arg(b.width).arg(c.width));
    int crow = 0, ccol = 0;
    e->cursorPos(&crow, &ccol);
    tcheck(ccol == 4, QStringLiteral("写完两个汉字之后光标停在第 5 列"),
           QStringLiteral("实际列 %1").arg(ccol));

    /* 自动换行：20 列的屏灌 21 个字符，最后那个要掉到下一行 */
    e->feedBytesForTest("\x1b[2J\x1b[H");
    e->feedBytesForTest("abcdefghij1234567890X");
    tcheck(e->lineText(0) == QLatin1String("abcdefghij1234567890")
               && e->lineText(1) == QLatin1String("X"),
           QStringLiteral("满 20 列自动换到第二行"),
           QStringLiteral("行1=%1 行2=%2").arg(e->lineText(0), e->lineText(1)));

    /*
     * 回滚：6 行的屏灌 10 行（每行后面都带 \r\n）。
     *
     * 期望值是**数出来的**，不是"顶出去 4 行"那种想当然：最后那条 L10 的 \r\n
     * 也会把光标推到屏外，于是 L10 那一行自己也被顶了出去 —— 进回滚的是
     * L1..L5（5 行），屏上从 L6 开始。
     */
    e->feedBytesForTest("\x1b[2J\x1b[H");
    QByteArray flood;
    for (int i = 1; i <= 10; ++i)
        flood += QByteArray("L") + QByteArray::number(i) + "\r\n";
    e->feedBytesForTest(flood);
    tcheck(e->historyRows() == 5, QStringLiteral("顶出去的行进了回滚区"),
           QStringLiteral("historyRows = %1").arg(e->historyRows()));
    tcheck(e->lineText(-1).contains(QLatin1String("L5")),
           QStringLiteral("回滚区最后一行（-1）是最新被顶出去的那条"),
           e->lineText(-1));
    tcheck(e->lineText(0).contains(QLatin1String("L6")),
           QStringLiteral("屏上第一行是 L6"), e->lineText(0));

    /* 备用屏：进去画东西、出来应该回到原来那一屏 */
    e->feedBytesForTest("\x1b[?1049h");
    tcheck(e->altScreen(), QStringLiteral("1049h 进了备用屏"));
    e->feedBytesForTest("ALTSCREEN-STUFF");
    e->feedBytesForTest("\x1b[?1049l");
    tcheck(!e->altScreen() && !e->lineText(0).contains(QLatin1String("ALTSCREEN")),
           QStringLiteral("1049l 退出备用屏之后主屏内容回来了（不留残影）"), e->lineText(0));

    /* 清屏 */
    e->feedBytesForTest("\x1b[2J");
    tcheck(e->lineText(0).isEmpty(), QStringLiteral("2J 清屏"));

    /*
     * 改尺寸补出来的格子必须带**主题底色**，不许带画笔上挂着的颜色。
     *
     * 这就是"主程序窗口最大化之后，终端右边那一片硬边黑"那条 bug 的根：
     * libvterm 的 clearcell()/erase_internal() 用 screen->pen 的颜色补空格子，
     * 而 ConPTY 重排整屏时画笔上挂着的就是"背景 = 调色板第 0 号 = 纯黑"，
     * 于是变宽变高之后那些没人再写的格子底色是 #000000，视图照着铺矩形铺出一条黑带。
     * 实测数字：一帧里数到 91 块 #ff000000 的单格矩形，起点正好停在改尺寸**之前**的宽度上。
     * 修法是 setSize() 注一个 ESC[m 把画笔交回"用默认色"那两个标志位（见该函数注释）。
     */
    {
        auto blackBgCells = [&](TerminalEngine *en) -> int {
            const QColor black(0, 0, 0);
            int n = 0;
            for (int r = 0; r < en->rows(); ++r)
                for (int c = 0; c < en->cols(); ++c) {
                    TerminalCell cell;
                    if (en->cellAt(r, c, &cell) && cell.bg == black)
                        ++n;
                }
            return n;
        };
        TerminalEngine *rz = new TerminalEngine;
        rz->setSize(20, 6);
        rz->feedBytesForTest("AAA");
        rz->feedBytesForTest("\x1b[40m");   // 把画笔染成调色板第 0 号（= ConPTY 那一套）
        rz->setSize(60, 12);                // 变宽变高：补出来的格子用画笔色 = 黑
        const int afterWiden = blackBgCells(rz);
        tcheck(afterWiden == 0,
               QStringLiteral("改尺寸补出来的空格带主题底色，不带画笔的黑色"),
               QStringLiteral("纯黑底格子 %1 个（画笔此时是 40 号黑，默认底 %2，格子 20x6 → 60x12）")
                   .arg(afterWiden).arg(rz->defaultBg().name()));
        /* 平时输出里的"黑画笔状态下擦除"也不许留黑底（这一条不经过 setSize，量的是 fillCell） */
        rz->feedBytesForTest("\x1b[H\x1b[J");
        rz->feedBytesForTest("\x1b[40m\x1b[K");   // 用调色板 0 号当背景，擦到行尾
        int erasedBlack = 0;
        for (int c = 0; c < rz->cols(); ++c) {
            TerminalCell ec;
            if (rz->cellAt(0, c, &ec) && ec.bg == QColor(0, 0, 0))
                ++erasedBlack;
        }
        tcheck(erasedBlack == 0,
               QStringLiteral("黑画笔状态下的擦除换成主题底色（平时输出里那 8 块的那种）"),
               QStringLiteral("第 0 行还有 %1 个纯黑底空格，默认底 %2")
                   .arg(erasedBlack).arg(rz->defaultBg().name()));
        /* 同一套复位不能把程序**自己**涂的行尾也一起抹掉（bce 还得是 bce） */
        rz->feedBytesForTest("\x1b[m\x1b[H\x1b[J");
        rz->feedBytesForTest("T");                        // (0,0) 这个格要保持默认底
        rz->feedBytesForTest("\x1b[41m\x1b[K");           // 不写字，只把 1..末尾 涂到行尾
        TerminalCell tail, touched;
        /* 只判"还不是默认底"：41 号红经 libvterm 调色板出来是 #e00000，不是 #ff0000 */
        const bool tailPainted = rz->cellAt(0, 19, &tail) && tail.bg != rz->defaultBg();
        const bool headClean = rz->cellAt(0, 0, &touched) && touched.bg == rz->defaultBg();
        tcheck(tailPainted && headClean,
               QStringLiteral("ESC[41m 之后的 ESC[K 还是涂到行尾（复位画笔只发生在改尺寸那一拍）"),
               QStringLiteral("第 19 列底色=%1 刚写过字的第 0 列底色=%2 默认底=%3")
                   .arg(tail.bg.name()).arg(touched.bg.name()).arg(rz->defaultBg().name()));
        /* 历史行比当前屏窄时补的那一格也不能是黑（blankCell 零初始化的老坑） */
        rz->setSize(20, 6);
        for (int i = 0; i < 10; ++i)
            rz->feedBytesForTest(QStringLiteral("row%1\n").arg(i).toUtf8());
        rz->setSize(80, 6);   // 变宽：回滚行还是 20 列宽，19 之后要补格子
        TerminalCell padCell;
        const bool padOk = rz->cellAt(-1, 79, &padCell) && padCell.bg == rz->defaultBg();
        tcheck(padOk, QStringLiteral("窄历史行补出来的宽格子带主题底色"),
               QStringLiteral("(-1,79) 底色=%1").arg(padCell.bg.name()));
        delete rz;
    }

    /* 查询应答：对面问"光标在哪"，不答它就一直等（ConPTY 真的会问） */
    QByteArray reply;
    QObject::connect(e, &TerminalEngine::wroteToShell, e,
                     [&reply](const QByteArray &bytes) { reply += bytes; });
    e->feedBytesForTest("\x1b[6n");
    tcheck(reply.contains("\x1b[") && reply.endsWith('R'),
           QStringLiteral("CSI 6 n（DSR 光标位置）有答"), QString::fromLatin1(reply.toHex()));

    /* OSC 标题 */
    e->feedBytesForTest("\x1b]0;我的标题\x07");
    tcheck(waitUntil([e] { return e->title() == QString::fromUtf8("我的标题"); }, 500),
           QStringLiteral("OSC 0 标题能设进来"), e->title());

    delete e;
}

// ------------------------------------------------------------------ 第二层

void runRealShellChecks()
{
    tout(QStringLiteral("\n-- 真 PowerShell（真 ConPTY）--"));

    auto *e = new TerminalEngine;
    e->setSize(90, 24);

    QString err;
    const bool started = e->start(QString(), QStringList(), QString(), &err);
    tcheck(started, QStringLiteral("PowerShell 起得来"), err);
    if (!started) {
        delete e;
        return;
    }
    tcheck(e->running(), QStringLiteral("起完之后状态是 running"));

    /* 提示符要自己画出来：这条通了才说明"字节 -> 网格"整条路是通的 */
    const bool prompted = waitUntil([&e] { return !e->lineText(0).trimmed().isEmpty(); }, 8000);
    tcheck(prompted, QStringLiteral("提示符落进了网格"), e->lineText(0));

    auto run = [&](const QString &cmd) {
        e->sendText(cmd);
        e->sendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
    };

    QElapsedTimer clock;
    clock.start();
    run(QStringLiteral("Write-Output SMARTCLIP-73"));
    const bool echoed = waitUntil([&e] { return allText(e).contains(QLatin1String("SMARTCLIP-73")); },
                                  10000);
    tcheck(echoed, QStringLiteral("Write-Output 的标记出现在网格里"),
           QStringLiteral("耗时 %1 ms").arg(clock.elapsed()));

    /*
     * 早退版对照：刚起来、只跑过两条命令的 shell 上试 Ctrl+C。
     *
     * 最小复现程序（build/probe-ctrlc.cpp）证明裸 0x03 在这台机器上**能**打断
     * ConPTY 里的 PowerShell，而后面那一版（刷屏 1200 行 + ESC[3J + 改成 50x12
     * 之后）打断不掉，差别只可能在"shell 被折腾过"。这一条把这个变量单独拎出来量：
     * 它绿、后面那条红，就是前面某一步弄坏了输入，不是 Ctrl+C 没接对。
     */
    run(QStringLiteral("Start-Sleep 20"));
    QThread::msleep(1200);
    e->sendBytes("\x03");
    QThread::msleep(1500);
    run(QStringLiteral("Write-Output (\"EA\"+\"RLY\")"));
    const bool earlyInterrupt = waitUntil(
        [&e] { return allText(e).contains(QLatin1String("EARLY")); }, 8000);
    tout(QStringLiteral("     （分段量）刚起来的 shell 上 Ctrl+C：EARLY 出现=%1，格子 %2x%3，"
                       "shell 还在跑=%4 —— 和最小复现程序（同一条裸 0x03 能打断）对照")
             .arg(earlyInterrupt).arg(e->cols()).arg(e->rows()).arg(e->running()));

    /* 环境块：我们拼的 TERM 要真的落到子进程环境里 */
    run(QStringLiteral("Write-Output $env:TERM"));
    tcheck(waitUntil([&e] { return allText(e).contains(QLatin1String("xterm-256color")); }, 10000),
           QStringLiteral("子进程里的 TERM = xterm-256color（环境块生效）"));

    /*
     * 中文往返：PowerShell 自己的输出编码是 GBK，ConPTY 在中间转成 UTF-8。
     * 这一条红的表现就是满屏"锟斤拷"，是用户一眼能看见的事故。
     */
    run(QStringLiteral("Write-Output 中文测试"));
    tcheck(waitUntil([&e] { return allText(e).contains(QString::fromUtf8("中文测试")); }, 10000),
           QStringLiteral("中文输出没有乱码"));

    /* 颜色：Write-Host -ForegroundColor Red 要真的让格子带上非默认前景 */
    run(QStringLiteral("Write-Host -ForegroundColor Red REDMARK"));
    const bool colored = waitUntil(
        [&e] {
            for (int r = -e->historyRows(); r < e->rows(); ++r) {
                const QString line = e->lineText(r);
                const int at = line.indexOf(QLatin1String("REDMARK"));
                if (at < 0)
                    continue;
                TerminalCell cell;
                e->cellAt(r, at, &cell);
                if (cell.fg != e->defaultFg())
                    return true;
            }
            return false;
        },
        10000);
    tcheck(colored, QStringLiteral("Write-Host 的红色前景真的带进了格子"));

    /*
     * 刷屏之后"最后一行"不能按行号断言：命令跑完 PowerShell 会自己再打一个提示符，
     * 屏上最后一行永远是提示符。这里要钉的其实是**尾巴没丢** ——
     * line 1200 得在（屏上或回滚里），少几行才是真事故。
     */
    e->sendBytes("\x1b[3J");   // 让 libvterm 把回滚清掉（对面不认就当没这回事）
    clock.restart();
    run(QStringLiteral("1..1200 | ForEach-Object { \"line $_\" }"));
    const bool flooded = waitUntil(
        [&e] { return allText(e).contains(QLatin1String("line 1200")); }, 30000);
    tout(QStringLiteral("     刷屏 1200 行：耗时 %1 ms，回滚 %2 行")
             .arg(clock.elapsed())
             .arg(e->historyRows()));
    tcheck(flooded, QStringLiteral("刷屏的最后一行没丢（在屏上或回滚里找得着）"));
    tcheck(e->historyRows() > 1000, QStringLiteral("刷屏的内容进了回滚区（往上翻找得着）"),
           QStringLiteral("historyRows = %1").arg(e->historyRows()));

    /*
     * 改尺寸：把面板拖高拖宽之后，对面要按**新**宽度排版。
     * 量 RawUI.BufferSize.Width —— 它报的就是 ConPTY 现在的缓冲区宽度。
     */
    e->setSize(50, 12);
    /*
     * 问对面"窗口多宽"。用 [Console]::WindowWidth 而不是 $host.UI.RawUI.BufferSize ——
     * 前者就是 ConPTY 拦下的那个 Win32 控制台调用，报的正是我们给的伪控制台尺寸。
     *
     * 判法要短：把宽度塞进一个 4 个字符的标记里找，而不是拿"命令原文 + 换行 + 数字"
     * 去正则匹配 —— resize 会让对面按新宽度**重排**整屏，长命令会被折行折断，
     * 那种正则会被重排本身弄挂（第一版就是这么红的）。
     */
    run(QStringLiteral("Write-Output \"W=$([Console]::WindowWidth)\""));
    const bool resized = waitUntil(
        [&e] { return allText(e).contains(QLatin1String("W=50")); }, 10000);
    tcheck(resized, QStringLiteral("resize 到 50 列之后对面按 50 列报宽度"),
           QStringLiteral("cols=%1 rows=%2").arg(e->cols()).arg(e->rows()));

    /*
     * Ctrl+C：跑一个 30 秒的命令，按打断。
     *
     * 判法换了。原来盯的是"最后一行有没有 PS ...>"—— 提示符落在第几行受重排、
     * 滚动、面板高度影响，那条红得说不清是打断失败还是判据挑错了地方。
     * 现在问一件用户真正关心的事：**打断之后下一条命令还跑不跑**。
     *
     * 标记要拼出来（"AFT" + "ERINT"）：直接写 AFTERINT 的话，敲进去的那行命令
     * 自己就含这个串，shell 卡着不动也能被判成"通过"。
     */
    /*
     * 这条判据红着是有原因的，四条路都试过、都量过（2026-09-23）：
     *   1) 裸 0x03 写进伪控制台输入端 —— 屏上连 ^C 都没出现；
     *   2) win32-input-mode 编码成真 KEY_EVENT（CSI ? 9001 h + CSI 67;46;3;1;4;0_）
     *      —— 和 1 一模一样，没区别；
     *   3) FreeConsole + AttachConsole(shell pid) + CTRL_C_EVENT —— **两个 API 都返回成功**，
     *      shell 既不停也不死；
     *   4) 同上，但子进程用 CREATE_NEW_PROCESS_GROUP 起、发 CTRL_BREAK_EVENT(带 pid)
     *      —— 还是返回成功、还是没反应。
     *   另外把 Start-Sleep 换成原生命令 ping -n 30 也断不掉 —— 不是托管 sleep 的锅。
     *
     * 结论：不是"我们发的东西不对"，是这台机器上 ConPTY 子进程压根不理会控制台
     * 控制事件。下一步该先用一个不带 Qt 的最小 ConPTY 复现程序把这件事钉死，
     * 再决定改哪儿 —— 所以这条先红着，不拿豁免糊。
     */
    run(QStringLiteral("Start-Sleep 30"));
    QThread::msleep(600);
    e->sendBytes("");
    /*
     * 这里必须**真的等一会儿**再敲第二条：屏上那句旧提示符本来就在（"PS " 一直在
     * 第 10 行），拿它当"打断完成了"是假判据；而 \x03 之后立刻灌下一条，
     * PowerShell 会把这段输入连同中断一起吞掉（实测那样永远等不到 AFTERINT）。
     */
    QThread::msleep(1500);
    for (int k = 0; k < 30; ++k)
        QCoreApplication::processEvents();
    const bool sawCaretC = allText(e).contains(QLatin1String("^C"));
    run(QStringLiteral("Write-Output (\"AFT\" + \"ERINT\")"));
    const bool interrupted = waitUntil(
        [&e] { return allText(e).contains(QLatin1String("AFTERINT")); }, 8000);
    tout(QStringLiteral("     打断之后的整屏（%1 行）：").arg(e->rows()));
    for (int r = 0; r < e->rows(); ++r) {
        QString row = e->lineText(r);
        while (!row.isEmpty() && row.endsWith(QLatin1Char(' ')))
            row.chop(1);
        tout(QStringLiteral("       [%1] %2").arg(r).arg(row));
    }

    /* 整屏导出来看：判据只说"没断"，说不出它卡在提示符、Y/N 问句还是回显里 */
    tout(QStringLiteral("     打断之后的整屏（%1 行）：").arg(e->rows()));
    for (int r = 0; r < e->rows(); ++r) {
        QString row = e->lineText(r);
        while (!row.isEmpty() && row.endsWith(QLatin1Char(' ')))
            row.chop(1);
        tout(QStringLiteral("       [%1] %2").arg(r).arg(row));
    }

    tcheck(interrupted, QStringLiteral("Ctrl+C 打得断 Start-Sleep（打断后下一条命令真能跑）"),
           QStringLiteral("屏上出现过 ^C=%1；AFTERINT 出现=%2；shell 还在跑=%3；格子 %4x%5")
               .arg(sawCaretC).arg(interrupted).arg(e->running())
               .arg(e->cols()).arg(e->rows()));

    /* exit：进程自己退了，状态要跟着变 */
    bool gotExit = false;
    int exitCode = -1;
    QObject::connect(e, &TerminalEngine::exited, e,
                     [&](int code) { gotExit = true; exitCode = code; });
    run(QStringLiteral("exit"));
    waitUntil([&e] { return !e->running(); }, 8000);
    tcheck(gotExit, QStringLiteral("shell 自己退出时报了 exited"),
           QStringLiteral("退出码 %1").arg(exitCode));

    delete e;

    /* 起停三回：验收尸（伪控制台 / 线程 / 管道句柄没漏的话不会卡也不会崩） */
    QElapsedTimer loop;
    loop.start();
    bool cycled = true;
    for (int i = 0; i < 3 && cycled; ++i) {
        auto *one = new TerminalEngine;
        one->setSize(80, 20);
        QString oneErr;
        cycled = one->start(QString(), QStringList(), QString(), &oneErr);
        if (cycled)
            cycled = waitUntil([&one] { return !one->lineText(0).trimmed().isEmpty(); }, 8000);
        delete one;   // 析构里要能把进程和线程收干净
    }
    tcheck(cycled, QStringLiteral("连起连收 3 次都干净（句柄 / 线程没漏）"),
           QStringLiteral("耗时 %1 ms").arg(loop.elapsed()));
}

// ------------------------------------------------------------------ 第三层

/* 找 marker 在网格里的位置（屏 + 回滚，按行号给） */
bool locate(TerminalEngine *e, const QString &marker, int *row, int *col)
{
    for (int r = -e->historyRows(); r < e->rows(); ++r) {
        const int at = e->lineText(r).indexOf(marker);
        if (at >= 0) {
            *row = r;
            *col = at;
            return true;
        }
    }
    return false;
}

/* 一块矩形里有多少像素"有墨"（和底色差得够远） */
int countInk(const QImage &img, const QRect &box, const QColor &bg)
{
    int n = 0;
    for (int y = box.top(); y < box.bottom(); ++y) {
        for (int x = box.left(); x < box.right(); ++x) {
            const QRgb p = img.pixel(x, y);
            const int d = qAbs(qRed(p) - bg.red()) + qAbs(qGreen(p) - bg.green())
                          + qAbs(qBlue(p) - bg.blue());
            if (d > 60)
                ++n;
        }
    }
    return n;
}

/* 一块矩形里有多少像素"就是这个颜色"（容差 60，抗锯齿边缘算进去） */
int countNear(const QImage &img, const QRect &box, const QColor &want)
{
    int n = 0;
    for (int y = box.top(); y < box.bottom(); ++y) {
        for (int x = box.left(); x < box.right(); ++x) {
            const QRgb p = img.pixel(x, y);
            const int d = qAbs(qRed(p) - want.red()) + qAbs(qGreen(p) - want.green())
                          + qAbs(qBlue(p) - want.blue());
            if (d < 90)
                ++n;
        }
    }
    return n;
}

void runRenderChecks(QObject *qmlRoot, EditorController *cmd)
{
    /*
     * 主题钉成深色再跑这一套。
     *
     * 下面有一批断言量的是"窗口底色 / 卡片边 / 圆角露出来的那一线"，它们隐含
     * 假设界面是深色。用户把界面切成白色之后这些会红成一片（实测 8 红里 6 条
     * 是这个原因），而程序其实没坏。所以进来先存一份、强制深色，末尾原样还回去。
     */
    auto *theme = AppTheme::instance();
    const bool themeWas = theme && theme->light();
    if (theme)
        theme->setLight(false);

    tout(QStringLiteral("\n-- 界面与真实渲染（量抓下来的图，不量代码）--"));

    /* 快捷键本身认不认：PortableText 里那个反引号不是所有版本都解得出来 */
    const QKeySequence backtick(QStringLiteral("Ctrl+`"), QKeySequence::PortableText);
    tcheck(!backtick.isEmpty(), QStringLiteral("Ctrl+` 这个组合键 Qt 认得（能当快捷键用）"),
           backtick.toString());

    /*
     * 快捷键这一跳要在应用内量：给宿主 QWidget 发一次真的 Ctrl+` 按键事件，
     * 走的是 QApplication::notify 里那套快捷键匹配（和物理按键同一条路），
     * 匹配上才会发 commandRequested("toggleTerminal") -> dispatch -> 翻开关。
     * 从外面 SendKeys 打不进来的（这台机器上前台切换时好时坏，量不到就是量不到）。
     */
    QWidget *host = nullptr;
    /*
     * 宿主窗口按"里面挂着 QQuickWidget"来认，不按遍历顺序：这台机器上顶层同时有
     * 1460x900（真宿主）、3840x2160（最大化预热那层）和一个 QMenu，
     * 旧的"第一个宽 > 600"写法挑中谁全凭顺序 —— 实测红过一次（按键发给了别人）。
     * host 后面还要拿来裁图、showMaximized，选错会牵连一串，所以优先取活动的那个。
     */
    QWidget *fallback = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (!w->isVisible() || w->width() <= 600 || !w->findChild<QQuickWidget *>())
            continue;
        if (QApplication::activeWindow() == w) {
            host = w;
            break;
        }
        if (!fallback)
            fallback = w;
    }
    if (!host)
        host = fallback;
    const bool hiddenBefore = qmlRoot->property("terminalHidden").toBool();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_QuoteLeft, Qt::ControlModifier,
                    QStringLiteral("`"));
    QCoreApplication::sendEvent(host, &press);
    /* 按下就该翻开；松开不该再触发一次（否则翻两下回到原样，看着就是"按了没反应"） */
    const bool flipped = waitUntil(
        [&] { return qmlRoot->property("terminalHidden").toBool() != hiddenBefore; }, 1500);
    const bool afterPress = qmlRoot->property("terminalHidden").toBool();
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_QuoteLeft, Qt::ControlModifier,
                      QStringLiteral("`"));
    QCoreApplication::sendEvent(host, &release);
    QCoreApplication::processEvents();
    QString topLevelDump;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        topLevelDump += QStringLiteral(" [%1 %2 %3x%4 活动=%5]")
                            .arg(QString::fromLatin1(w->metaObject()->className()))
                            .arg(w->objectName().isEmpty() ? QStringLiteral("-") : w->objectName())
                            .arg(w->width()).arg(w->height())
                            .arg(QApplication::activeWindow() == w);
    }
    tcheck(flipped
               && qmlRoot->property("terminalHidden").toBool() == afterPress,
           QStringLiteral("Ctrl+` 按下翻开、松开不重复触发（走的是快捷键匹配那条路）"),
           QStringLiteral("按前=%1 按下后=%2 松开后=%3 按键发给=%4 %5x%6 顶层:%7")
               .arg(hiddenBefore).arg(afterPress)
               .arg(qmlRoot->property("terminalHidden").toBool())
               .arg(host ? QString::fromLatin1(host->metaObject()->className())
                         : QStringLiteral("(空)")
                  ).arg(host ? host->width() : 0).arg(host ? host->height() : 0)
               .arg(topLevelDump));

    /* 后面还要量渲染：不管上面那条红不红，先把面板摆成打开的样子 */
    if (qmlRoot->property("terminalHidden").toBool()) {
        QMetaObject::invokeMethod(qmlRoot, "dispatch",
                                  Q_ARG(QVariant, QStringLiteral("toggleTerminal")));
        waitUntil([&] { return !qmlRoot->property("terminalHidden").toBool(); }, 1500);
        QCoreApplication::processEvents();
    }

    QObject *panel = nullptr;
    waitUntil([&] {
        for (QObject *o : qmlRoot->findChildren<QObject *>()) {
            if (o->property("sessionCount").isValid()) {
                panel = o;
                return true;
            }
        }
        return false;
    }, 2000);
    tcheck(panel != nullptr, QStringLiteral("面板组件在对象树里"));
    if (!panel)
        return;

    /*
     * 视图是 Repeater 长出来的，**findChild<TerminalView*> 够不到它**（实测：面板自己
     * itemAt(0) 拿得到、列数都算出来了，而按类名遍历整棵树是 0 个）。所以让面板自己
     * 交出来，再问它挂没挂进场景 —— 挂没挂进场景才决定它画不画得出来。
     */
    TerminalView *view = nullptr;
    if (!waitUntil([&] {
             QVariant v;
             QMetaObject::invokeMethod(panel, "firstView", Q_RETURN_ARG(QVariant, v));
             view = qobject_cast<TerminalView *>(v.value<QObject *>());
             return view != nullptr && view->window() != nullptr;
         }, 3000)) {
        tcheck(false, QStringLiteral("面板里长出了视图，且已挂进场景"),
               QStringLiteral("opened=%1 高=%2 会话=%3")
                   .arg(panel->property("opened").toBool())
                   .arg(panel->property("height").toReal())
                   .arg(panel->property("sessionCount").toInt()));
        return;
    }
    tcheck(true, QStringLiteral("面板里长出了视图，且已挂进场景"));

    TerminalEngine *e = view->engine();
    tcheck(waitUntil([&e] { return e->running() && !e->lineText(0).trimmed().isEmpty(); }, 10000),
           QStringLiteral("面板里的 shell 起来了"), e->lineText(0));
    tcheck(view->columns() >= 40 && view->rows() >= 5 && view->cellWidth() > 4
               && view->cellHeight() > 8,
           QStringLiteral("按面板尺寸算出了行列数和格子大小"),
           QStringLiteral("%1 列 x %2 行，格子 %3x%4")
               .arg(view->columns()).arg(view->rows())
               .arg(view->cellWidth()).arg(view->cellHeight()));

    const QString cnMarker = QStringLiteral("中文渲染%1").arg(7);
    e->sendText(QStringLiteral("Write-Output ") + cnMarker);
    e->sendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
    waitUntil([&] {
        int r, c;
        return locate(e, cnMarker, &r, &c);
    }, 10000);
    int cnRow = -1, cnCol = -1;
    const bool found = locate(e, cnMarker, &cnRow, &cnCol);
    tcheck(found, QStringLiteral("中文标记进了网格"), QStringLiteral("row=%1").arg(cnRow));

    /*
     * 标记只能在**输出**里出现，不能出现在命令里：locate() 取的是第一个匹配，
     * 而回显的那行命令永远在上面。所以把标记拆开写（"RED"+"PIXEL"），
     * 命令行里就没有 REDPIXEL 这个连续串了。
     */
    e->sendText(QStringLiteral("Write-Host -ForegroundColor Red (\"RED\"+\"PIXEL\")"
                               "; Write-Host -ForegroundColor Green (\"GREEN\"+\"PIXEL\")"));
    e->sendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
    waitUntil([&] {
        int r, c;
        return locate(e, QStringLiteral("REDPIXEL"), &r, &c)
               && locate(e, QStringLiteral("GREENPIXEL"), &r, &c);
    }, 10000);

    /*
     * 抓的是**宿主 QWidget**。QQuickWindow::grabWindow() 在 QQuickWidget 里给的是
     * 空图（实测 0x0）—— 那种窗口从头到尾没有自己的表面，像素都在宿主控件里。
     */
    const QImage img = host ? host->grab().toImage() : QImage();
    tcheck(!img.isNull() && img.width() > 200 && img.height() > 200,
           QStringLiteral("抓到了真实渲染"),
           QStringLiteral("%1x%2 宿主=%3").arg(img.width()).arg(img.height())
               .arg(host ? host->metaObject()->className() : QStringLiteral("无")));
    if (img.isNull() || img.width() < 200)
        return;
    img.save(QStringLiteral("H:/steward/build/term-render.png"));

    const qreal sx = qreal(img.width()) / host->width();
    const qreal sy = qreal(img.height()) / host->height();
    /* 视图里的格子 -> 图上的矩形 */
    auto cellBox = [&](int row, int col, int spanCols) {
        const QPointF topLeft = view->mapToScene(
            QPointF(view->padding() + col * view->cellWidth(), row * view->cellHeight()));
        return QRect(QPoint(int(topLeft.x() * sx), int(topLeft.y() * sy)),
                     QSize(qRound(view->cellWidth() * spanCols * sx),
                           qRound(view->cellHeight() * sy)));
    };

    if (found) {
        /* 逐格量"有没有墨"：宽字符的尾巴格（width==0）本来就不画，不占分母 */
        int inkedCells = 0, drawnCells = 0;
        for (int i = 0; i < 16; ++i) {
            TerminalCell cell;
            if (!e->cellAt(cnRow, cnCol + i, &cell) || cell.width == 0)
                continue;
            /* 空格子本来就没墨，不能算进分母（第一版就是这么差出 11/12 的） */
            if (cell.charCount == 0 || cell.chars[0] == U' ')
                continue;
            ++drawnCells;
            if (countInk(img, cellBox(cnRow, cnCol + i, 1), e->defaultBg()) > 6)
                ++inkedCells;
        }
        tcheck(drawnCells >= 4 && inkedCells == drawnCells,
               QStringLiteral("中文那几个格子里真的画出了字（逐格量墨）"),
               QStringLiteral("%1/%2 格有墨").arg(inkedCells).arg(drawnCells));
    }

    /* 颜色：网格说这一格是什么色，图里对应位置就得有这个色的像素 —— 网格和像素对得上 */
    for (const auto pair : { QPair<QString, QString>{ QStringLiteral("REDPIXEL"),
                                                     QStringLiteral("红") },
                             { QStringLiteral("GREENPIXEL"), QStringLiteral("绿") } }) {
        int r = -1, c = -1;
        if (!locate(e, pair.first, &r, &c)) {
            tcheck(false, QStringLiteral("%1 那一行没找到").arg(pair.second));
            continue;
        }
        TerminalCell cell;
        e->cellAt(r, c, &cell);
        const int n = countNear(img, cellBox(r, c, pair.first.size()), cell.fg);
        tcheck(n > 20, QStringLiteral("%1前景：网格里记的颜色在图上真的画出来了")
                           .arg(pair.second),
               QStringLiteral("%1,%2,%3 的像素 %4 个 @ 格子(%5,%6)")
                   .arg(cell.fg.red()).arg(cell.fg.green()).arg(cell.fg.blue())
                   .arg(n).arg(r).arg(c));
    }

    /*
     * 对齐：他点名的就是这一条，所以直接量边界，不靠眼看。
     *   1) 面板那一行上，卡片色的最左 / 最右像素
     *   2) 中间那一行上，左树卡片的最左、编辑区卡片的最右
     *   3) 面板卡片顶边往上数，中间那条窗口底色的缝有几像素
     * 前两条要相等（左和树对齐、右和内容区对齐），第三条要等于树与内容之间那条缝。
     */
    auto isCard = [&](const QRgb p) {
        return qAbs(qRed(p) - 30) <= 2 && qAbs(qGreen(p) - 31) <= 2 && qAbs(qBlue(p) - 34) <= 2;
    };
    auto isGap = [&](const QRgb p) {
        return qAbs(qRed(p) - 49) <= 2 && qAbs(qGreen(p) - 51) <= 2 && qAbs(qBlue(p) - 53) <= 2;
    };
    auto rowEdges = [&](int y, int *l, int *r) {
        *l = -1;
        *r = -1;
        for (int x = 0; x < img.width(); ++x) {
            if (isCard(img.pixel(x, y))) { *l = x; break; }
        }
        for (int x = img.width() - 1; x >= 0; --x) {
            if (isCard(img.pixel(x, y))) { *r = x; break; }
        }
    };
    const int panelTop = qRound(view->mapToScene(QPointF(0, 0)).y() * sy);
    int panelL = 0, panelR = 0, treeL = 0, editR = 0;
    rowEdges(panelTop + 20, &panelL, &panelR);
    rowEdges(qMax(4, panelTop - 60), &treeL, &editR);
    tcheck(treeL > 0 && qAbs(panelL - treeL) <= 1,
           QStringLiteral("面板左边缘和左树卡片对齐"),
           QStringLiteral("树 %1 / 面板 %2").arg(treeL).arg(panelL));
    tcheck(editR > 0 && qAbs(panelR - editR) <= 1,
           QStringLiteral("面板右边缘和编辑区卡片对齐"),
           QStringLiteral("编辑区 %1 / 面板 %2").arg(editR).arg(panelR));

    /* 面板卡片顶边 = 视图顶边往上一个标签条高 */
    const int cardTop = qRound((view->mapToScene(QPointF(0, 0)).y()
                                - panel->property("headerHeight").toReal()) * sy);
    int gap = 0;
    for (int y = cardTop - 1; y > 0 && isGap(img.pixel(panelL + 40, y)); --y)
        ++gap;
    tcheck(gap == 3, QStringLiteral("面板上面那条缝和树/内容那条缝一样宽（3px 窗口底色）"),
           QStringLiteral("量到 %1 px").arg(gap));
    /*
     * 四个角都得是圆的：上角压在标签条上（条子色），下角压在卡片/正文上（卡片色）。
     * 每个角探三个点 —— 角尖里 2px 必须是窗口底色（被圆掉了），沿两条边各让开一个
     * 半径必须是面板自己的颜色，卡片外 2px 又得回到窗口底色。
     */
    auto isPanelInk = [&](const QRgb p) {
        const int dr = qRed(p), dg = qGreen(p), db = qBlue(p);
        const bool card = qAbs(dr - 30) <= 3 && qAbs(dg - 31) <= 3 && qAbs(db - 34) <= 3;
        const bool hdr = qAbs(dr - 37) <= 3 && qAbs(dg - 37) <= 3 && qAbs(db - 38) <= 3;
        return card || hdr;
    };
    const int r = 10;
    const int cardBottom = qRound(view->mapToScene(QPointF(0, view->height())).y() * sy) + 2;
    QStringList bad;
    struct Corner { const char *name; int x, y, dx, dy; };
    const Corner corners[4] = {
        { "TL", panelL, cardTop, 1, 1 }, { "TR", panelR, cardTop, -1, 1 },
        { "BL", panelL, cardBottom, 1, -1 }, { "BR", panelR, cardBottom, -1, -1 }
    };
    for (const Corner &c : corners) {
        /* 角尖往里 1px：应该被圆掉 = 窗口底色（取 1 不取 2，2 会落在抗锯齿边上） */
        if (!isGap(img.pixel(c.x + c.dx, c.y + c.dy)))
            bad << QStringLiteral("%1尖").arg(c.name);
        /* 沿两条边各让开一个半径：应该是面板自己的颜色 */
        if (!isPanelInk(img.pixel(c.x + c.dx * 2, c.y + c.dy * r)))
            bad << QStringLiteral("%1纵边").arg(c.name);
        if (!isPanelInk(img.pixel(c.x + c.dx * r, c.y + c.dy * 2)))
            bad << QStringLiteral("%1横边").arg(c.name);
        /* 卡片外 3px：必须是窗口底色（那三条 Layout 边距露出来的部分） */
        if (!isGap(img.pixel(c.x - c.dx * 3, c.y + c.dy * r)))
            bad << QStringLiteral("%1外沿").arg(c.name);
    }
    tcheck(bad.isEmpty(), QStringLiteral("四个角都是圆的（角尖露窗口底色、边上不戳方角）"),
           bad.isEmpty() ? QStringLiteral("16 个探针点全过") : bad.join(QLatin1Char(' ')));

    /*
     * 滚轮方向（用户报的：滚轮往前、滚动条却往下）。
     * 约定：往前滚（angleDelta.y > 0）= 看更早的内容 = scrollUp 变大 = 位置条往上走。
     */
    e->sendText(QStringLiteral("1..120 | ForEach-Object { \"hrow $_\" }"));
    e->sendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
    waitUntil([&] { return allText(e).contains(QLatin1String("hrow 120")); }, 15000);
    view->scrollToEnd();
    QCoreApplication::processEvents();
    const QString topAtBottom = view->topVisibleText();

    /* 先量一次真滚轮事件：一格 = 3 行，往前滚 scrollUp 变大 */
    const QPointF mid = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
    QWheelEvent wheelFwd(mid, host->mapToGlobal(mid.toPoint()), QPoint(0, 0), QPoint(0, 120),
                         Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QWheelEvent wheelBack(mid, host->mapToGlobal(mid.toPoint()), QPoint(0, 0), QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    view->scrollToEnd();
    QApplication::sendEvent(host->findChild<QQuickWidget *>(), &wheelFwd);
    const int afterFwd = view->scrollUp();
    QApplication::sendEvent(host->findChild<QQuickWidget *>(), &wheelBack);
    const int afterBack = view->scrollUp();
    tcheck(afterFwd == 3 && afterBack == 0,
           QStringLiteral("滚轮往前一格 = 往上翻 3 行，往后一格回到底"),
           QStringLiteral("前=%1 后=%2").arg(afterFwd).arg(afterBack));

    view->scrollLines(-9);   // 往前 9 行
    const QString topUp = view->topVisibleText();
    tcheck(view->scrollUp() == 9 && !view->atBottom(),
           QStringLiteral("往前 9 行 = scrollUp 变大（往上翻历史）"),
           QStringLiteral("scrollUp=%1").arg(view->scrollUp()));
    tcheck(topUp != topAtBottom && topUp == e->lineText(-9),
           QStringLiteral("往前翻之后视图最上面那一行确实是更早的内容"),
           QStringLiteral("贴底顶行「%1」→ 翻上去「%2」").arg(topAtBottom, topUp));
    view->scrollLines(9);
    tcheck(view->atBottom() && view->topVisibleText() == topAtBottom,
           QStringLiteral("再往回滚能贴回底部"), QStringLiteral("scrollUp=%1").arg(view->scrollUp()));

    /*
     * 面板矩形里的"最长连续黑段"：用户那块黑是一整条硬边矩形，按行找连续跑才和它同形状。
     * 只盯面板自己那块矩形 —— 整桌面数黑点会被盖在我们上面的别的窗口污染。
     */
    struct BlackRun { int len = 0, x = 0, y = 0; };
    auto worstBlackRun = [&](QQuickItem *probe) -> BlackRun {
        const QImage desktop = QGuiApplication::primaryScreen()->grabWindow(0).toImage();
        const QPoint origin = host->mapToGlobal(QPoint(0, 0));
        QImage img = desktop.copy(QRect(origin, host->size()));
        if (img.isNull())
            img = desktop;
        const qreal kx = host->width() > 0 ? qreal(img.width()) / host->width() : 1.0;
        const qreal ky = host->height() > 0 ? qreal(img.height()) / host->height() : 1.0;
        const QPointF a = probe->mapToScene(QPointF(0, 0));
        const QPointF b = probe->mapToScene(QPointF(probe->width(), probe->height()));
        const QRectF vr = QRectF(a, b).normalized();
        const int x0 = qBound(0, qRound(vr.x() * kx), img.width());
        const int x1 = qBound(0, qRound(vr.right() * kx), img.width());
        const int y0 = qBound(0, qRound(vr.y() * ky), img.height());
        const int y1 = qBound(0, qRound(vr.bottom() * ky), img.height());
        BlackRun worst;
        for (int y = y0; y < y1; y += 2) {
            int run = 0;
            for (int x = x0; x < x1; ++x) {
                const QRgb p = img.pixel(x, y);
                if (qRed(p) < 6 && qGreen(p) < 6 && qBlue(p) < 6) {
                    ++run;
                    if (run > worst.len)
                        worst = BlackRun{ run, x - run + 1, y };
                } else {
                    run = 0;
                }
            }
        }
        return worst;
    };

    /*
     * 最大化：整行让给面板。这时候卡片里**不许出现纯黑**——
     * 中间那一行藏着原生子窗口（QScintilla），让位的时候最容易留下没重画的洞。
     * 量之前先按用户的尺寸摆窗口：1460x900 下复现不出来，4K 才出（见 2026-09-22 那轮）。
     */
    const QSize oldSize = host->size();
    const QPoint oldPos = host->pos();
    const int rowsBeforeMax = view->rows();
    /*
     * 走他真按的那条路，不是 showMaximized()：最大化那颗按钮是"按下先花 165ms 把 4K
     * 那一帧渲染好、松手才当场换几何"（WindowControls.qml:134-142）。
     * 用户那张图里正文正好停在**最大化之前**的宽度上 —— 只有这条预热路径会
     * "先渲染、后换尺寸"，showMaximized() 那条不走。
     */
    QObject *win = nullptr;
    if (auto *eng = host->findChild<QQuickWidget *>()->engine())
        win = eng->singletonInstance<QObject *>("SmartClip.Globals", "Win");
    bool realPath = false;
    if (win) {
        QMetaObject::invokeMethod(win, "prewarmMaximize");
        for (int i = 0; i < 40; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        bool handled = false;
        QMetaObject::invokeMethod(win, "prewarmRelease", Q_RETURN_ARG(bool, handled));
        realPath = handled;
    }
    if (!host->isMaximized())
        host->showMaximized();
    waitUntil([&] { return host->isMaximized(); }, 3000);
    /*
     * 立刻抓屏幕，**只泵够上屏的事件就抓**：多泵一轮就等于给它一次自愈的机会，
     * 量到的永远是"修好之后"。用户那张图里正文停在 1412 = 1460 宽窗口时面板的宽，
     * 是"换完几何之后没人重画右边"这一种。
     */
    QStringList rightAfter;
    for (int i = 0; i < 6; ++i) {
        for (int k = 0; k < 2; ++k)
            QCoreApplication::processEvents();
        const BlackRun b = worstBlackRun(view);
        rightAfter << QStringLiteral("%1@%2,%3").arg(b.len).arg(b.x).arg(b.y);
        QThread::msleep(20);
    }
    tcheck(true, QStringLiteral("【探针】真路最大化之后头几帧，视图里的最长黑段"),
           QStringLiteral("预热真路=%1，逐次=%2（视图 %3x%4 格子 %5x%6）")
               .arg(realPath).arg(rightAfter.join(" "))
               .arg(view->width()).arg(view->height()).arg(view->rows())
               .arg(view->property("cols").toInt()));
    for (int i = 0; i < 30; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    for (int i = 0; i < 40; ++i) {
        QCoreApplication::processEvents();
        if (host->width() >= 3000)
            break;
        QThread::msleep(25);
    }
    /*
     * 先把编辑区真填上一份文档：它是**原生子窗口**（QScintilla），
     * 用户报的黑块只在"开着文档最大化"时出现 —— 空着欢迎页复现不出来（试过）。
     */
    const QString tmpDoc = QDir::tempPath() + QStringLiteral("/smartclip-term-probe.md");
    QFile f(tmpDoc);
    if (f.open(QIODevice::WriteOnly)) {
        f.write("# term probe\n");
        f.close();
    }
    QMetaObject::invokeMethod(qmlRoot, "openTreeFile", Q_ARG(QVariant, tmpDoc));
    bool docOpen = false;
    waitUntil([&] {
        QObject *v = qmlRoot->property("view").value<QObject *>();
        docOpen = v && v->property("hasDocument").toBool();
        return docOpen;
    }, 3000);
    for (int i = 0; i < 20; ++i)
        QCoreApplication::processEvents();
    qmlRoot->setProperty("terminalMaximized", true);
    if (!waitUntil([&] { return view->rows() > 30; }, 3000)) {
        tcheck(false, QStringLiteral("最大化之后网格跟着变高"),
               QStringLiteral("rows=%1").arg(view->rows()));
    }
    /*
     * 用户那张图是"窗口最大化 + 面板没最大化"（图只有 591 高），正文正好停在 1412
     * = 1460 宽窗口时面板的宽。所以两个时机都要探：换窗口几何那一跳（上面那条探针）、
     * 和面板吃下整行这一跳（这一条）。都只泵够上屏就抓，多泵一轮就是给它自愈的机会。
     */
    {
        QStringList series;
        int worstScreen = 0, blackRectPeak = 0, canvasRunPeak = 0, canvasBlackPeak = 0;
        for (int i = 0; i < 6; ++i) {
            for (int k = 0; k < 2; ++k)
                QCoreApplication::processEvents();
            const BlackRun b = worstBlackRun(view);
            const QString canvasRun = view->dbgCanvasBlackRun();   // 先扫，两个数字才新鲜
            worstScreen = qMax(worstScreen, b.len);
            blackRectPeak = qMax(blackRectPeak, view->dbgBlackRects());
            canvasRunPeak = qMax(canvasRunPeak, view->dbgCanvasRun());
            canvasBlackPeak = qMax(canvasBlackPeak, view->dbgCanvasBlackSamples());
            series << QStringLiteral("屏%1@%2 / 画布%3 / 铺底%4")
                          .arg(b.len).arg(b.x).arg(canvasRun)
                          .arg(view->dbgLastFrameBgStats());
            QThread::msleep(20);
        }
        /*
         * 门禁量的是**那条带**：画布和屏幕上都不许出现 100 px 以上的连续黑。
         * 修之前这两个数是 2384 / 2382（左边界钉在改尺寸之前的宽度上）。
         *
         * 剩下的"铺了几块黑底格子"只报数、不当门禁，因为量出来它是另一件事：
         * 那 8 块是**宽字符的尾巴格**（字=0x63 是上一帧留下的脏值，宽=0，charCount=0），
         * 也就是 ConPTY 用"背景=调色板 0 号"写上去的中文 —— 每个汉字后面跟着一个
         * 8x15 的黑格，被字本身的笔画打断，所以连续黑段量到 0、稀疏采样量到 11 个点。
         * 那是"黑这个颜色在这套主题里该映射成什么"的决定（改调色板 0 号会同时改掉
         * 所有黑前景），不是"哪一层没画"的 bug，所以摆在这儿给人看，不塞进门禁里。
         */
        /*
         * 门禁量的是**画布 + 我们主动铺的矩形**这两层（都在渲染线程自己那一侧，
         * 读回来不会骗人）：修之前是"画布最长黑段 2384、一帧铺 91 块 #ff000000"。
         *
         * 屏幕那一列只报数不当门禁：换几何的头一帧屏幕上可以整条全黑（这一轮量到
         * 3797 px = 视图整个宽），而**同一帧画布是干净的**（连续黑段 0、铺黑底 0）——
         * 那是离屏表面回读那一拍，和终端内容无关，结案记录见 maximize-flash-tradeoffs
         * （4K 面积税，只有 GPU 顶层能消）。拿它当门禁就是把那件已拍板的事重新吵一遍。
         */
        tcheck(canvasRunPeak < 100 && blackRectPeak == 0,
               QStringLiteral("面板吃下整行之后的头几帧：画布干净，我们也没主动铺过黑底格子"),
               QStringLiteral("视图 %1x%2 格子 %3 列，画布最长黑段=%4，单帧最多铺黑底=%5 块；"
                              "屏幕黑段峰值=%6（只报数：头一帧画布干净而屏幕全黑 = 离屏回读那一拍）")
                   .arg(view->width()).arg(view->height()).arg(view->columns())
                   .arg(canvasRunPeak).arg(blackRectPeak).arg(worstScreen));
    }

    /*
     * 格子层的证据：改完尺寸之后，**空白格**都带了什么底色。
     *
     * 为什么量这一层：用户机器上开了 SMARTCLIP_TERM_LOG 之后，"画布取样"那一行直接
     * 给出 #000000（画布右边就是黑的，不是呈现层丢的）。画布上那一笔黑只有一个来源 ——
     * drawRow() 里 `if (runColor != m_bg) fillRect(...)`，也就是**格子的 bg 本身就是黑**。
     * 所以病在引擎交出来的颜色，不在纹理。这一条把"哪些格子带了非默认底色、
     * 是什么色"打成数字，不用再猜是 pen 泄漏（libvterm 的 erase/clearcell 用
     * screen->pen）还是越界补的 blankCell（零初始化 = flags 0 = 显式 RGB 黑）。
     */
    auto blankCellColors = [&](TerminalEngine *en, int row, const char *tag) -> QString {
        int blankTotal = 0, blankBad = 0, writtenBad = 0;
        QMap<QString, int> badColors;   // QColor 在 Qt6 没有 operator<，按 #rrggbb 归并
        int firstBadCol = -1;
        for (int col = 0; col < en->cols(); ++col) {
            TerminalCell c;
            if (!en->cellAt(row, col, &c)) {
                if (firstBadCol < 0)
                    firstBadCol = col;
                ++blankBad;   // 取不到 = 视图那边退化成默认构造的 TerminalCell（底色无效）
                continue;
            }
            const bool blank = (c.charCount == 0);
            const bool bad = (c.bg != en->defaultBg());
            if (blank) {
                ++blankTotal;
                if (bad) {
                    ++blankBad;
                    badColors[c.bg.name()]++;
                    if (firstBadCol < 0)
                        firstBadCol = col;
                }
            } else if (bad) {
                ++writtenBad;
            }
        }
        QString top;
        for (auto it = badColors.constBegin(); it != badColors.constEnd() && top.size() < 60; ++it)
            top += QStringLiteral(" %1x%2").arg(it.value()).arg(it.key());
        return QStringLiteral("%1 行%2：空格 %3 个 / 空格底色非默认 %4 个%5 / 有字且非默认 %6 个"
                              " 第一个异常列=%7（默认底=%8 格子 %9x%10）")
            .arg(QLatin1String(tag)).arg(row).arg(blankTotal).arg(blankBad).arg(top)
            .arg(writtenBad).arg(firstBadCol).arg(en->defaultBg().name())
            .arg(en->cols()).arg(en->rows());
    };
    {
        TerminalEngine *en = view->engine();
        QStringList cellReports;
        cellReports << blankCellColors(en, 0, "屏顶");
        cellReports << blankCellColors(en, en->rows() - 1, "屏底");
        cellReports << blankCellColors(en, -1, "回滚");
        tout(QStringLiteral("     %1\n").arg(cellReports.join("\n     ")));
    }

    /*
     * 用户 2026-09-23 第 3 条：终端最大化之后"滚动条消失了，但里面的内容被截断、
     * 也不能滚动"。按他那个顺序复现：**先在常规高度灌够历史，再把面板吃下整行**。
     *
     * 三个数一起看才有结论：
     *   rows      改完之后屏上能容几行
     *   history   改完之后还剩多少行在屏顶以上（=0 就是条子藏起来的直接原因）
     *   最早那行  还在不在（不在 = 真丢了，不是"没得滚"）
     * 屏从 44 行涨到 134 行，libvterm 会 sb_popline 把历史倒回屏上，倒一部分是正常
     * 的；600 行内容不可能被 134 行吃干净 —— 要是 history 归 0 又找不着 krow 1，
     * 那就是丢了。
     */
    {
        qmlRoot->setProperty("terminalMaximized", false);
        waitUntil([&] { return view->rows() < 60; }, 4000);
        view->clearBuffer();
        QCoreApplication::processEvents();
        e->sendText(QStringLiteral("1..600 | ForEach-Object { \"krow $_\" }"));
        e->sendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
        const bool fed = waitUntil([&] { return allText(e).contains(QLatin1String("krow 600")); },
                                   25000);
        const int rowsBefore = view->rows(), histBefore = view->historyRows();
        /* 改之前屏上有几行有字、最下面那行有字的在第几行 */
        auto screenFill = [&](TerminalEngine *en) -> QVariantList {
            int nonEmpty = 0, firstRow = -1, lastRow = -1;
            for (int r = 0; r < en->rows(); ++r) {
                if (en->lineText(r).trimmed().isEmpty())
                    continue;
                ++nonEmpty;
                if (firstRow < 0)
                    firstRow = r;
                lastRow = r;
            }
            return { nonEmpty, firstRow, lastRow };
        };
        const QVariantList before = screenFill(e);

        qmlRoot->setProperty("terminalMaximized", true);
        waitUntil([&] { return view->rows() > 100; }, 4000);
        for (int k = 0; k < 20; ++k)
            QCoreApplication::processEvents();
        const int rowsAfter = view->rows(), histAfter = view->historyRows();
        const QVariantList after = screenFill(e);

        int firstAt = -9999, lastAt = -9999, nonEmpty = 0;
        for (int r = -histAfter; r < rowsAfter; ++r) {
            const QString line = e->lineText(r).trimmed();
            if (line.isEmpty())
                continue;
            ++nonEmpty;
            if (line == QStringLiteral("krow 1")) firstAt = r;
            if (line == QStringLiteral("krow 600")) lastAt = r;
        }
        /* 条子的可见性必须等于"有没有得滚"，两边读的是同一个数 */
        QObject *trk = nullptr;
        {
            QVariant tv;
            QMetaObject::invokeMethod(panel, "currentTrack", Q_RETURN_ARG(QVariant, tv));
            trk = tv.value<QObject *>();
        }
        const bool barVisible = trk && trk->property("visible").toBool();
        /*
         * 宽度没变（473 → 473），所以**行数是可以直接相加减的**：没有折行合并、
         * 也没有拆开。这一趟变高的净行数必须守恒 —— 少了就是真丢了行。
         * （窗口同时变宽的那一种不能这么算，这里刻意只让它变高。）
         */
        const int totalBefore = histBefore + before[0].toInt();
        const int totalAfter = histAfter + after[0].toInt();
        tout(QStringLiteral("     最大化前后：格子 %1x%2 回滚 %3 屏上有字 %4 行(第 %5..%6) "
                            "→ 格子 %7x%8 回滚 %9 屏上有字 %10 行(第 %11..%12)，"
                            "合计 %13 → %14；krow 1 在第 %15 行，krow 600 在第 %16 行，条子可见=%17")
                 .arg(rowsBefore).arg(view->columns()).arg(histBefore)
                 .arg(before[0].toInt()).arg(before[1].toInt()).arg(before[2].toInt())
                 .arg(rowsAfter).arg(view->columns()).arg(histAfter)
                 .arg(after[0].toInt()).arg(after[1].toInt()).arg(after[2].toInt())
                 .arg(totalBefore).arg(totalAfter).arg(firstAt).arg(lastAt).arg(barVisible));
        /*
         * 判"变少"，不判"不等"：总数可以**多**出一行（命令行回显、折行解开），
         * 丢行才是要抓的那件事（修之前是 645 → 555，正好少 90 = 新长出来的行数）。
         * 写成相等会红在"多了一行"上，那条红没有意义。
         */
        tcheck(fed && totalAfter >= totalBefore,
               QStringLiteral("终端变高（宽度不变）之后一行都不许丢"),
               QStringLiteral("回滚+屏上有字：%1 → %2（rows %3→%4，回滚 %5→%6，屏上 %7→%8）")
                   .arg(totalBefore).arg(totalAfter).arg(rowsBefore).arg(rowsAfter)
                   .arg(histBefore).arg(histAfter).arg(before[0].toInt()).arg(after[0].toInt()));
        tcheck(firstAt > -9999 && lastAt > -9999,
               QStringLiteral("终端最大化之后最早/最新那两行都还翻得着"),
               QStringLiteral("krow 1 在第 %1 行、krow 600 在第 %2 行（rows=%3 history=%4）")
                   .arg(firstAt).arg(lastAt).arg(rowsAfter).arg(histAfter));
        tcheck(barVisible == (histAfter > 0),
               QStringLiteral("条子的显与藏，和「有没有得滚」是同一个数"),
               QStringLiteral("history=%1 条子可见=%2").arg(histAfter).arg(barVisible));
        qmlRoot->setProperty("terminalMaximized", false);
        waitUntil([&] { return view->rows() < 60; }, 4000);
    }

    /*
     * 关键差别：**空闲**。用户那一刻终端没有新输出，节点没人再要求重画；我的探针
     * 一直泡在刷屏内容里，第二帧就被新内容冲好了 —— 那样量到的"自愈"是假的。
     * 所以先等内容彻底安静（paint 计数不再涨），再把面板收回原高、重新吃下整行，连采 20 帧。
     */
    {
        qmlRoot->setProperty("terminalMaximized", false);
        waitUntil([&] { return view->rows() < 60; }, 3000);
        for (int i = 0; i < 60; ++i)
            QCoreApplication::processEvents();
        QThread::msleep(400);
        view->dbgResetPaintStats();
        waitUntil([&] { return view->dbgPaints() > 0; }, 500);
        view->dbgResetPaintStats();
        QThread::msleep(200);
        const bool quiet = view->dbgPaints() == 0;   // 确认这一刻确实没在画
        qmlRoot->setProperty("terminalMaximized", true);
        QStringList idle;
        QVector<int> idleCanvas;
        for (int i = 0; i < 20; ++i) {
            for (int k = 0; k < 2; ++k)
                QCoreApplication::processEvents();
            const BlackRun b = worstBlackRun(view);
            const QString canvasRun = view->dbgCanvasBlackRun();
            idleCanvas << view->dbgCanvasBlackSamples();
            /* 只在屏幕也黑的那几帧再并排量一次格子：三层数字对不上才有结论 */
            QString extra;
            if (b.len >= 100)
                extra = QStringLiteral(" 画布%1｜%2")
                            .arg(canvasRun)
                            .arg(blankCellColors(view->engine(), 0, "屏顶"));
            idle << QStringLiteral("%1%2").arg(b.len).arg(extra);
        }
        int lastBlack = -1;
        for (int i = idleCanvas.size() - 1; i >= 0; --i)
            if (idleCanvas[i] > 0) { lastBlack = i; break; }
        /*
         * 门禁看的是**画布**（我们交出去的那张图）：空闲改尺寸之后一帧都不许有黑。
         * 屏幕那一列照旧打出来当参考 —— 换几何的头一帧屏幕上还留着面板底下原来那块
         * （剪贴板缩略图里有一张真黑图，量到 201 px），那是"谁盖着谁"的事，不是我们画的。
         */
        tcheck(quiet && lastBlack < 0, QStringLiteral("空闲时把面板撑满整行：20 帧里画布不许有黑"),
               QStringLiteral("确实安静=%1，首个画布黑帧=%2，屏幕黑段逐帧=%3（视图 %4x%5）")
                   .arg(quiet).arg(lastBlack).arg(idle.join(","))
                   .arg(view->width()).arg(view->height()));
        qmlRoot->setProperty("terminalMaximized", false);
        waitUntil([&] { return view->rows() < 60; }, 3000);
    }
    for (int i = 0; i < 20; ++i)
        QCoreApplication::processEvents();
    /*
     * 抓**真实桌面**，不是 host->grab()：离屏渲染里根本没有原生子窗口那一层，
     * "让位留下的黑洞"只有真屏幕上才看得见（第一版用 grab() 量，全绿，用户照样看到黑块）。
     */
    host->raise();
    host->activateWindow();
    for (int i = 0; i < 30; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    const QImage desktop = QGuiApplication::primaryScreen()->grabWindow(0).toImage();
    const QPoint origin = host->mapToGlobal(QPoint(0, 0));
    QImage maxImg = desktop.copy(QRect(origin, host->size()));
    if (maxImg.isNull())
        maxImg = desktop;
    auto *panelItem = qobject_cast<QQuickItem *>(panel);
    const BlackRun worstRun = worstBlackRun(panelItem);
    /*
     * 黑块是硬边矩形、没有圆角 —— 那它不是 QML 画的卡片，而是**盖在 QML 上面的原生窗口**。
     * 把本进程所有顶层窗口都列出来对号（ConPTY 会自己建一个 PseudoConsoleWindow）。
     */
    QString editorRect;
    QString winList;
    {
        struct Ctx { QString *out; };
        Ctx ctx{ &winList };
        EnumWindows(
            [](HWND h, LPARAM l) {
                auto *c = reinterpret_cast<Ctx *>(l);
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (pid != GetCurrentProcessId() || !IsWindowVisible(h))
                    return TRUE;
                RECT r {};
                GetWindowRect(h, &r);
                wchar_t cls[128] = L"";
                GetClassNameW(h, cls, 128);
                *c->out += QStringLiteral(" [%1 %2,%3~%4,%5]")
                               .arg(QString::fromWCharArray(cls))
                               .arg(r.left).arg(r.top).arg(r.right).arg(r.bottom);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
    }
    for (QObject *o : qmlRoot->findChildren<QObject *>()) {
        const QString cn = QString::fromLatin1(o->metaObject()->className());
        if (cn.startsWith(QLatin1String("EditorArea"))) {
            if (auto *it = qobject_cast<QQuickItem *>(o)) {
                const QPointF a = it->mapToScene(QPointF(0, 0));
                const QPointF b = it->mapToScene(QPointF(it->width(), it->height()));
                editorRect = QStringLiteral(" 编辑区 %1,%2 ~ %3,%4 可见=%5")
                                 .arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y())
                                 .arg(it->isVisible());
            }
            break;
        }
    }
    tcheck(worstRun.len < 100,
           QStringLiteral("最大化之后面板里没有黑块（原生子窗口让位不留洞）"),
           QStringLiteral("最长连续黑段 %1 px @ %2,%3（文档已开=%4%5）本进程窗口:%6")
               .arg(worstRun.len).arg(worstRun.x).arg(worstRun.y)
               .arg(docOpen).arg(editorRect).arg(winList));
    maxImg.save(QStringLiteral("H:/steward/build/term-maximized.png"));

    /*
     * 用户 2026-09-23 那段录屏里的黑块（逐帧量出来的几何，不是猜）：
     *   x 1442..3823、y 1416..2075 —— 一条**硬边矩形**，左边界正好停在最大化**之前**
     *   那块正文的宽度（1442）上；而且它比面板本身还大：上边压住标签条下面第一行、
     *   下边一直盖到状态栏，右边顶到窗口右缘。
     *
     * 所以判据不能只框**面板**那一个矩形 —— 黑块会溢到面板外面去，上面这条就漏掉了它。
     * 这里按**整个窗口**再量一遍：卡片底色 #1e1f22、窗口底色 #313335 都不是纯黑，
     * 整窗里出现一条 >=100px 的连续纯黑，就是有一片像素从来没被画过（离屏那张渲染
     * 目标没铺到那儿，混出来正好是黑）。idle 那一拍也一样：面板撑满整行时黑块可以
     * 落在面板矩形之外，所以两个时机都按整窗量。
     */
    {
        auto worstBlackInWindow = [&](const QImage &shot) -> BlackRun {
            const qreal kx = host->width() > 0 ? qreal(shot.width()) / host->width() : 1.0;
            const qreal ky = host->height() > 0 ? qreal(shot.height()) / host->height() : 1.0;
            BlackRun worst;
            for (int y = 0; y < shot.height(); y += 2) {
                int run = 0;
                for (int x = 0; x < shot.width(); ++x) {
                    const QRgb p = shot.pixel(x, y);
                    if (qRed(p) < 8 && qGreen(p) < 8 && qBlue(p) < 8) {
                        ++run;
                        if (run > worst.len)
                            worst = BlackRun{ run, qRound(x / kx), qRound(y / ky) };
                    } else {
                        run = 0;
                    }
                }
            }
            return worst;
        };
        const BlackRun fullWin = worstBlackInWindow(maxImg);
        /*
         * 门限定在 600 px，不是原来那条 <100：整窗扫黑这一把尺子会扫到**别人**——
         * 剪贴板缩略图网格里有一张背景本来就是纯黑的图（201 px @ 1540,510，
         * 逐点取样是 #16213a / #ffffff / #1e0a07 围着它，抓下来一看是摩托车那张）。
         * 我们要抓的那条历史黑带是 2382~2384 px、左边界钉在改尺寸之前的宽度上，
         * 600 这条线既漏不掉它也不会被一张图骗红。
         */
        tcheck(fullWin.len < 600,
               QStringLiteral("最大化之后**整个窗口**里没有黑块（面板之外也算）"),
               QStringLiteral("最长连续黑段 %1 px @ %2,%3（窗口 %4x%5，文档已开=%6）")
                   .arg(fullWin.len).arg(fullWin.x).arg(fullWin.y)
                   .arg(host->width()).arg(host->height()).arg(docOpen));
    }
    /*
     * 用户报的：最大化之后滚轮滚不动了。同一个滚轮事件在最大化的尺寸上再打一次。
     * historyRows 是这一条的"有没有东西可翻"：它要是 0，翻不动是真的没得翻（不是 bug），
     * 所以数字一起打出来，别让它绿得没意义。
     */
    view->scrollToEnd();
    QCoreApplication::processEvents();
    /*
     * 最大化一次能容 134 行，之前那 120 行铺不满一屏，history 直接归 0，
     * "翻不动"就没法判断是没得翻还是滚轮坏了。这里再灌 600 行，确保最大化之后仍有历史。
     */
    e->sendText(QStringLiteral("1..600 | ForEach-Object { \"mrow $_\" }"));
    e->sendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
    waitUntil([&] { return allText(e).contains(QLatin1String("mrow 600")); }, 20000);
    {
        const QPointF m2 = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
        QWheelEvent wf(m2, host->mapToGlobal(m2.toPoint()), QPoint(0, 0), QPoint(0, 120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        const bool handled = QApplication::sendEvent(host->findChild<QQuickWidget *>(), &wf);
        tcheck(view->scrollUp() > 0,
               QStringLiteral("最大化之后滚轮照样能翻历史"),
               QStringLiteral("rows=%1 history=%2 scrollUp=%3 事件被吃=%4")
                   .arg(view->rows()).arg(view->historyRows()).arg(view->scrollUp()).arg(handled));
    }
    /*
     * 用户报的第 5 条："拖动途中输出的内容被截断了"。
     * 翻到最顶上，看最早那一行还在不在 —— 改尺寸（宽和高一起变）过程中只要有一行
     * 被丢掉，这里就找不着它了。判据用"整行正好等于 mrow 1"，不能用 contains：
     * mrow 10、mrow 100 都含 "mrow 1"，那样这条尺子量不出任何东西。
     */
    {
        view->scrollLines(-view->historyRows() - view->rows());   // 一路翻到最顶
        QCoreApplication::processEvents();
        QString oldest;
        for (int r = -view->historyRows(); r < view->rows(); ++r) {
            const QString line = e->lineText(r).trimmed();
            if (line == QStringLiteral("mrow 1")) {
                oldest = line;
                break;
            }
        }
        tcheck(!oldest.isEmpty() && view->scrollUp() >= view->historyRows() - 2,
               QStringLiteral("最大化（宽和高一起变）之后最早那行输出还翻得出来"),
               QStringLiteral("rows=%1 history=%2 scrollUp=%3，找得到 mrow 1=%4，屏顶现在是「%5」")
                   .arg(view->rows()).arg(view->historyRows()).arg(view->scrollUp())
                   .arg(!oldest.isEmpty()).arg(view->topVisibleText()));
        view->scrollToEnd();
    }
    /*
     * 画布尺寸必须跟上 item 尺寸。这条量的是"改完尺寸那一帧有没有拿旧图去贴新矩形"，
     * 它**不是**那条黑带的判据（黑带是格子的底色，见上面"面板吃下整行"那两条；
     * 当年先怀疑到这一层，是因为只量了尺寸、没量颜色）。留着是因为它盯的是另一件事，
     * 而且换基类之后它必须一直绿。
     */
    {
        QImage canvas;
        auto res = view->grabToImage();
        bool ready = false;
        QObject::connect(res.data(), &QQuickItemGrabResult::ready, [&] { ready = true; });
        waitUntil([&] { return ready; }, 3000);
        canvas = res->image();
        const int wantW = qRound(view->width() * view->window()->devicePixelRatio());
        const int wantH = qRound(view->height() * view->window()->devicePixelRatio());
        tcheck(!canvas.isNull() && qAbs(canvas.width() - wantW) <= 2
                   && qAbs(canvas.height() - wantH) <= 2,
               QStringLiteral("最大化之后画布尺寸跟上了 item 尺寸（右边不许是没画过的）"),
               QStringLiteral("走的是预热那条真路=%1，画布 %2x%3，item %4x%5（DPR=%6，该是 %7x%8）")
                   .arg(realPath)
                   .arg(canvas.width()).arg(canvas.height()).arg(view->width())
                   .arg(view->height()).arg(view->window()->devicePixelRatio())
                   .arg(wantW).arg(wantH));
    }
    qmlRoot->setProperty("terminalMaximized", false);
    /* 缩回"最大化之前那个高度"才对，不该假设它是多少（存档里 termHeight 是多少都行） */
    waitUntil([&] { return qAbs(view->rows() - rowsBeforeMax) <= 2; }, 4000);
    const int rowsAfterMax = view->rows();
    host->showNormal();
    host->resize(oldSize);
    host->move(oldPos);
    QCoreApplication::processEvents();


    /*
     * 滚动条（用户报的"最大化滚动条不能滚"+"显示出来、不要隐藏"）。
     * 常驻：内容超出视口就一直在（贴底也留）；点一下轨道任意位置窗口跳过去（可测的那一步），
     * 按住拖是同一个函数（合成鼠标拖不动，见下面拖高度那条）。
     */
    {
        /* 位置条在委托里，findChild 够不到 —— 让面板自己交出来（同 firstView 那条坑） */
        QObject *track = nullptr;
        {
            QVariant tv;
            QMetaObject::invokeMethod(panel, "currentTrack", Q_RETURN_ARG(QVariant, tv));
            track = tv.value<QObject *>();
        }
        view->scrollToEnd();
        QCoreApplication::processEvents();
        tcheck(track != nullptr && view->historyRows() > 0
                   && track->property("visible").toBool(),
               QStringLiteral("有内容超出视口时位置条常驻（贴在底部也留着）"),
               QStringLiteral("找得到=%1 history=%2 rows=%3 可见=%4")
                   .arg(track != nullptr).arg(view->historyRows()).arg(view->rows())
                   .arg(track ? track->property("visible").toBool() : false));
        /*
         * 出现/消失的**时机**：一次都不改尺寸，只让输出把内容顶出视口。
         *
         * 用户报的第 2 条"滚动条时隐时现"。QML 那条 `visible: view.historyRows > 0`
         * 要求 historyRows 变化时有信号发出来，而它的 NOTIFY 挂的是 gridChanged
         * （行列数变了才发）—— 回滚行数在输出滚动时就变，行列数一点不动。
         * 少了补发，这个绑定就只在改尺寸那一拍重新求值：翻出来一半是黑的、一半是没刷的。
         */
        if (track) {
            view->clearBuffer();
            for (int k = 0; k < 20; ++k)
                QCoreApplication::processEvents();
            const int histAfterClear = view->historyRows();
            const bool hiddenAfterClear = !track->property("visible").toBool();
            const int need = view->rows() * 2;
            e->sendText(QStringLiteral("1..%1 | ForEach-Object { \"sbrow $_\" }").arg(need));
            e->sendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
            waitUntil([&] { return view->historyRows() > view->rows() / 2; }, 20000);
            for (int k = 0; k < 20; ++k)
                QCoreApplication::processEvents();
            tcheck(hiddenAfterClear && histAfterClear == 0 && view->historyRows() > 0
                       && track->property("visible").toBool(),
                   QStringLiteral("位置条跟着内容出现：清空就藏、顶出视口就露（全程没改过尺寸）"),
                   QStringLiteral("清空后 history=%1 已藏=%2 → 灌 %3 行后 history=%4 可见=%5")
                       .arg(histAfterClear).arg(hiddenAfterClear).arg(need)
                       .arg(view->historyRows()).arg(track->property("visible").toBool()));
        }
        /*
         * 位置条的像素（用户报的第 1 条 + 2026-09-24 那句"不要有背景"）。
         * 量的是**这两个 item 自己渲染出来的像素**，不是 Rectangle 的属性值 ——
         * 属性写了不代表画出来那样。
         *
         * 为什么不用桌面抓屏对号：第一次那么量，坐标算到 (2633,809) 打出来是 #ffffff，
         * 抓下来一看才发现那块区域是剪贴板缩略图网格（一张背景本来就黑的图），尺子读的不是我们。
         * grabToImage 只渲染这一个 item，遮挡与它无关。
         *
         * 两件事分开钉：
         *   1) 轨道必须**透明** —— 贴底（scrollUp=0）时滑块在最下面，轨道上沿那 4px
         *      就是纯轨道，alpha 还满着就说明又铺回底色了；
         *   2) 滑块自己是圆的（半径 3 = 半个短边）：角尖落在圆外、往里才见色。
         *      原来这条量的是轨道的圆角，轨道既然不画底，圆角就没意义了，挪到滑块上。
         */
        if (auto *rb = qobject_cast<QQuickItem *>(track);
            rb && view->scrollUp() == 0 && view->historyRows() > 0) {
            const int dpr = qRound(view->window()->devicePixelRatio());
            auto grabOne = [dpr](QQuickItem *item, QImage *out) {
                auto res = item->grabToImage();
                bool ready = false;
                QObject::connect(res.data(), &QQuickItemGrabResult::ready, [&] { ready = true; });
                waitUntil([&] { return ready; }, 3000);
                *out = res->image();
                return !out->isNull() && out->width() >= 5 * dpr && out->height() >= 5 * dpr;
            };
            /* QColor(QRgb) 会把 alpha 丢掉（0x00000000 打出来是 #ff000000，看着像不透明的黑）
               —— 这条判据量的就是 alpha，必须走 setRgba 把那一档带出来 */
            auto hex = [](QRgb p) {
                QColor c;
                c.setRgba(p);
                return c.name(QColor::HexArgb);
            };

            QImage trackImg;
            const bool gotTrack = grabOne(rb, &trackImg);
            const QPoint trackMid(4 * dpr, 4 * dpr);
            const bool trackClear = gotTrack && qAlpha(trackImg.pixel(trackMid)) < 40;
            tcheck(trackClear,
                   QStringLiteral("位置条轨道不许有底色（贴底时轨道上沿是透明的）"),
                   QStringLiteral("轨道图 %1x%2 上沿那一点=%3（该是 alpha<40）")
                       .arg(trackImg.width()).arg(trackImg.height())
                       .arg(gotTrack ? hex(trackImg.pixel(trackMid))
                                     : QStringLiteral("图没抓到")));

            QQuickItem *thumbItem = nullptr;
            for (QObject *ch : rb->children()) {
                auto *ci = qobject_cast<QQuickItem *>(ch);
                if (ci && ci->property("color").isValid() && ci->property("radius").isValid()
                    && ci->height() < rb->height()) {
                    thumbItem = ci;
                    break;
                }
            }
            const QColor thumbColor(QStringLiteral("#4b4d4f"));
            auto isThumb = [&](QRgb p) {
                return qAlpha(p) > 200 && qAbs(qRed(p) - thumbColor.red()) <= 8
                       && qAbs(qGreen(p) - thumbColor.green()) <= 8
                       && qAbs(qBlue(p) - thumbColor.blue()) <= 8;
            };
            QImage thumbImg;
            const bool gotThumb = thumbItem && grabOne(thumbItem, &thumbImg);
            const QPoint corner(0, 0), inner(3 * dpr, 6 * dpr);
            tcheck(gotThumb && !isThumb(thumbImg.pixel(corner)) && isThumb(thumbImg.pixel(inner)),
                   QStringLiteral("滑块那个角真的是圆的（角尖落在圆外、往里才是滑块色）"),
                   QStringLiteral("滑块图 %1x%2 角尖=%3 往里=%4 滑块色=%5 找到滑块=%6")
                       .arg(thumbImg.width()).arg(thumbImg.height())
                       .arg(gotThumb ? hex(thumbImg.pixel(corner)) : QStringLiteral("-"))
                       .arg(gotThumb ? hex(thumbImg.pixel(inner)) : QStringLiteral("-"))
                       .arg(thumbColor.name()).arg(thumbItem != nullptr));
        }
        view->scrollToEnd();
        QCoreApplication::processEvents();
        /* 点轨道靠上那一处（20%）：scrollUp 该一下跳到接近满 */
        auto *trackItem = qobject_cast<QQuickItem *>(track);
        if (trackItem && view->historyRows() > 0) {
            view->scrollToEnd();
            QCoreApplication::processEvents();
            const QPointF sc = trackItem->mapToScene(QPointF(4, trackItem->height() * 0.2));
            const QPoint gl = host->mapToGlobal(sc.toPoint());
            QMouseEvent pr(QEvent::MouseButtonPress, sc, gl, Qt::LeftButton, Qt::LeftButton,
                           Qt::NoModifier);
            QMouseEvent rl(QEvent::MouseButtonRelease, sc, gl, Qt::LeftButton, Qt::NoButton,
                           Qt::NoModifier);
            QApplication::sendEvent(host->findChild<QQuickWidget *>(), &pr);
            QApplication::sendEvent(host->findChild<QQuickWidget *>(), &rl);
            for (int k = 0; k < 10; ++k)
                QCoreApplication::processEvents();
            const int up = view->scrollUp();
            const int want = view->historyRows();
            tcheck(up > want / 2, QStringLiteral("点位置条上半段 = 跳到更早的内容"),
                   QStringLiteral("点 20%% 处 → scrollUp=%1（上限 %2，点之前 0）")
                       .arg(up).arg(want));
            view->scrollToEnd();
        } else {
            tcheck(false, QStringLiteral("点位置条上半段 = 跳到更早的内容"),
                   QStringLiteral("轨道没找到或没历史可滚"));
        }
        view->scrollToEnd();
    }

    /*
     * 按住拖的那一段，光标不许闪回箭头（用户 2026-09-23 又提的那条）。
     *
     * QML 的 cursorShape 只在鼠标**停在那个 item 上**时生效：拖高度的把手只有 6px 高、
     * 位置条只有 8px 宽，指针一抖就跑到正文里，按住不放的过程中光标当场变回箭头。
     * 修法是拖动期间压一枚应用级 override 光标（Win.pushResizeCursor，左树那条缝同一招）。
     *
     * 量的层次要挑对：**位移**这一层合成鼠标量不了（这工程早就记过"合成鼠标拖不动"），
     * 但 override 是按下那一刻压上去的 —— 所以判据是"按下之后 overrideCursor 的 shape
     * 是不是那一枚、松手之后是不是还掉了"，这两步合成分得出来。
     */
    {
        auto overrideShape = []() -> int {
            const QCursor *c = QApplication::overrideCursor();
            return c ? int(c->shape()) : -1;
        };
        auto *qw = host->findChild<QQuickWidget *>();
        auto pressReleaseOn = [&](QQuickItem *item, int *pressedShape, int *afterRelease) {
            if (!item || !qw) {
                *pressedShape = -2;
                *afterRelease = -2;
                return;
            }
            const QPointF sc = item->mapToScene(QPointF(item->width() / 2.0,
                                                        item->height() / 2.0));
            const QPoint gl = host->mapToGlobal(sc.toPoint());
            QMouseEvent pr(QEvent::MouseButtonPress, sc, gl, Qt::LeftButton, Qt::LeftButton,
                           Qt::NoModifier);
            QMouseEvent rl(QEvent::MouseButtonRelease, sc, gl, Qt::LeftButton, Qt::NoButton,
                           Qt::NoModifier);
            QCoreApplication::sendEvent(qw, &pr);
            QCoreApplication::processEvents();
            *pressedShape = overrideShape();
            QCoreApplication::sendEvent(qw, &rl);
            QCoreApplication::processEvents();
            *afterRelease = overrideShape();
        };
        QVariant sv;
        QMetaObject::invokeMethod(panel, "stripItem", Q_RETURN_ARG(QVariant, sv));
        auto *strip = qobject_cast<QQuickItem *>(sv.value<QObject *>());
        int stripPressed = -2, stripReleased = -2;
        pressReleaseOn(strip, &stripPressed, &stripReleased);
        tcheck(stripPressed == int(Qt::SizeVerCursor) && stripReleased == -1,
               QStringLiteral("拖高度按住的那一段，光标钉在上下拉伸、松手就还掉"),
               QStringLiteral("把手 %1x%2，按下之后 override=%3（该是 %4），松手之后=%5（该是没压）")
                   .arg(strip ? qRound(strip->width()) : -1)
                   .arg(strip ? qRound(strip->height()) : -1)
                   .arg(stripPressed).arg(int(Qt::SizeVerCursor)).arg(stripReleased));

        /* 位置条那条 8px 轨道里的 MouseArea：按 children 里带 cursorShape 的那个找 */
        QObject *trackMouse = nullptr;
        {
            QVariant tv;
            QMetaObject::invokeMethod(panel, "currentTrack", Q_RETURN_ARG(QVariant, tv));
            if (auto *trk = tv.value<QObject *>()) {
                const auto kids = trk->children();
                for (QObject *c : kids) {
                    if (c->metaObject()->className()
                            && QString::fromLatin1(c->metaObject()->className())
                                   .contains(QLatin1String("MouseArea"))) {
                        trackMouse = c;
                        break;
                    }
                }
            }
        }
        int barPressed = -2, barReleased = -2;
        pressReleaseOn(qobject_cast<QQuickItem *>(trackMouse), &barPressed, &barReleased);
        view->scrollToEnd();
        tcheck(barPressed == int(Qt::PointingHandCursor) && barReleased == -1,
               QStringLiteral("拖位置条按住的那一段，光标钉住、松手就还掉"),
               QStringLiteral("找到轨道里的 MouseArea=%1，按下之后 override=%2（该是 %3），"
                              "松手之后=%4")
                   .arg(trackMouse != nullptr).arg(barPressed)
                   .arg(int(Qt::PointingHandCursor)).arg(barReleased));
    }

    /*
     * 用户报的第一条：**拖动改尺寸途中**面板里出黑块，点一下选择才消失。
     * 合成鼠标驱动不了那条 6px 热区（见下面拖高度那条的说明），所以这里直接推拖动真正推的
     * 那个数：一格一格把高度顶上去，每一格等**几何落定**就抓一帧桌面。
     * 只等几何、不等重绘，正是用户拿眼睛看到的那一格：缓存没重画的时候布局早就是新的了。
     */
    const qreal heightToRestore = panel->property("wantedHeight").toReal();
    BlackRun worstDrag;
    int worstAt = 0;
    for (int h = 180; h <= 660; h += 120) {
        QMetaObject::invokeMethod(panel, "draggedTo", Q_ARG(double, double(h)));
        waitUntil([&] { return qAbs(panel->property("height").toReal() - h) < 40; }, 800);
        for (int i = 0; i < 4; ++i)
            QCoreApplication::processEvents();
        const BlackRun w = worstBlackRun(panelItem);
        if (w.len > worstDrag.len) {
            worstDrag = w;
            worstAt = h;
        }
    }
    tcheck(worstDrag.len < 100,
           QStringLiteral("改尺寸途中每一格面板里都没有黑块（180~660 共 5 格）"),
           QStringLiteral("最差连续黑段 %1 px @ %2,%3（目标高度 %4）")
               .arg(worstDrag.len).arg(worstDrag.x).arg(worstDrag.y).arg(worstAt));
    QMetaObject::invokeMethod(panel, "draggedTo", Q_ARG(double, heightToRestore));
    waitUntil([&] { return qAbs(panel->property("height").toReal() - heightToRestore) < 40; },
              800);

    /*
     * 拖顶边改高度。
     *
     * **这里量的是信号链，不是鼠标热区**：合成 QMouseEvent 发给 QQuickWidget 之后，
     * 按下能被 MouseArea 吃掉、跟着的移动事件却进不了拖动抓取（实测 press 被 accept、
     * move 不被 accept，高度纹丝不动），所以那条 6px 热区本身只能靠人手拖一下确认。
     * 链路上其余环节（draggedTo -> window.terminalHeight -> 布局槽位 -> 记进设置）
     * 这一条都能钉住。
     */
    /*
     * 先把面板摆到 300 高：窗口还原后只有 900 高，存档里的 termHeight 可能是 700，
     * 再 +120 会被布局夹住 —— 那是前提不成立，不是拖不动。
     */
    qmlRoot->setProperty("terminalHeight", 300.0);
    waitUntil([&] { return qAbs(panel->property("wantedHeight").toReal() - 300) < 30; }, 2000);
    const qreal before = panel->property("wantedHeight").toReal();
    /* 先确认槽位是"真的开着"：面板关着时 before=0，+120 会被下限夹到 120，那条断言就空转了 */
    tcheck(before > 100, QStringLiteral("拖动那一步开跑前面板是开着的（槽位 > 100）"),
           QStringLiteral("wanted=%1").arg(before));
    /* 这一段会写设置，先把原值记下来，测完按原样放回去（和 --tool-test 同一约定） */
    const QString savedHeight = cmd ? cmd->recall(QStringLiteral("termHeight")) : QString();
    /* 参数类型要对上：QML 的 real 就是 double，用 QVariant 传 invokeMethod 会静默失败 */
    const bool sent = QMetaObject::invokeMethod(panel, "draggedTo",
                                                Q_ARG(double, before + 120.0));
    const qreal after = panel->property("wantedHeight").toReal();
    tcheck(qAbs(after - before - 120) < 2,
           QStringLiteral("拖动的信号链：draggedTo(+120) 让槽位真的长了 120"),
           QStringLiteral("%1 -> %2 (发出=%3)").arg(before).arg(after).arg(sent));
    tcheck(qmlRoot->property("terminalHeight").toReal() > before,
           QStringLiteral("拖完把新高度记进了窗口状态（收起再展开还是这个高）"));
    QMetaObject::invokeMethod(panel, "dragFinished");
    tcheck(cmd
               && cmd->recall(QStringLiteral("termHeight"), QStringLiteral("0")).toDouble()
                      > before,
           QStringLiteral("松手那一刻新高度落进了设置"));
    /* 拖回原样，后面的检查按原来的高度走；设置也按原样放回去 */
    QMetaObject::invokeMethod(panel, "draggedTo", Q_ARG(double, before));
    QMetaObject::invokeMethod(panel, "dragFinished");
    if (cmd && !savedHeight.isEmpty())
        cmd->remember(QStringLiteral("termHeight"), savedHeight);

    /* 收起：再 dispatch 一次。判**布局槽位**（wantedHeight）和 visible，不判 height ——
     * Quick Layouts 会跳过不可见的项、不再给它设几何，于是 height 停在收起前的残值
     * （实测 233），那不是缺陷，是布局的规矩。
     */
    /*
     * 用户 2026-09-23 第 1 条：**打开文档之后**，终端标签条右边那几颗按钮
     * （新建 / 清屏）看不到了。
     *
     * 怀疑的是编辑区那块原生子窗（createWindowContainer 出来的 QScintilla）：
     * 原生窗口永远画在 QML 上面，它只要往面板这一片伸过去一点，按钮就被盖掉，
     * 而在 QML 自己的坐标里量永远是"没重叠"。所以这里把两边都换算成**全局矩形**
     * 再对号，并把那一刻的真实桌面裁一条下来给人看。
     */
    {
        /*
         * 先把"打开了文档"这个前提**当场造出来**：上一节开的那份到这里可能已经换了
         * 状态，而这一条量的就是"有文档时那几颗按钮还在不在"。
         * 判据里把 hasDocument 和原生子窗的颗数一起打出来 —— 0 颗子窗 = 前提没成立，
         * 那时候"没盖住"是假绿（第一版就是这么绿的）。
         */
        const QString tmpDoc2 = QDir::tempPath() + QStringLiteral("/smartclip-term-probe2.md");
        QFile f2(tmpDoc2);
        if (f2.open(QIODevice::WriteOnly)) {
            f2.write("# 文档已打开\n\n这一段用来让编辑区那块原生子窗真的摆到界面上。\n");
            f2.close();
        }
        QMetaObject::invokeMethod(qmlRoot, "openTreeFile", Q_ARG(QVariant, tmpDoc2));
        bool docOpen = false;
        waitUntil([&] {
            QObject *v = qmlRoot->property("view").value<QObject *>();
            docOpen = v && v->property("hasDocument").toBool();
            return docOpen;
        }, 4000);
        for (int k = 0; k < 30; ++k)
            QCoreApplication::processEvents();

        /*
         * 两个时机都要量：面板常规高度（在编辑区下面）和面板吃下整行（盖住编辑区那一片）。
         * 后者才是嫌疑最大的那一种 —— 编辑区那块控件的矩形要是不跟着收，
         * 它就正好压在标签条这一条上。
         */
        auto measure = [&](const char *tag) {
        QVariant hv;
        QMetaObject::invokeMethod(panel, "headerRect", Q_RETURN_ARG(QVariant, hv));
        const QRectF headerScene = hv.toRectF();
        QVariant bv;
        QMetaObject::invokeMethod(panel, "headerButtons", Q_RETURN_ARG(QVariant, bv));
        const QVariantList btns = bv.toList();
        auto *qw = host->findChild<QQuickWidget *>();
        /*
         * 盖住按钮的不是"原生子窗"：EnumChildWindows 在这台机器上一颗都数不到 ——
         * 编辑那块控件压根没有 HWND。它是宿主页面上和 QQuickWidget **并列的一块 QWidget**，
         * Qt 直接把它画在 QQuickWidget 上面（谁 raise 过谁在上）。
         * 所以这里量的是它的 geometry()（宿主坐标）和按钮的场景坐标，两边都换算到
         * 宿主坐标再对号 —— 和 EditorViewItem 摆它时用的是同一套换算
         * （"场景坐标 + QQuickWidget 相对宿主的偏移"）。
         */
        const QPoint sceneInHost = qw ? qw->mapTo(host, QPoint(0, 0)) : QPoint();
        QVariantMap pane;
        if (EditorViewItem *ev = EditorViewItem::instance())
            pane = ev->paneGeometryForTest();
        const QRect widgetRect(sceneInHost.x() + pane.value(QStringLiteral("widgetX")).toInt(),
                               sceneInHost.y() + pane.value(QStringLiteral("widgetY")).toInt(),
                               pane.value(QStringLiteral("widgetW")).toInt(),
                               pane.value(QStringLiteral("widgetH")).toInt());
        const bool widgetUp = pane.value(QStringLiteral("widgetVisible")).toBool();
        QString report = QStringLiteral("标签条 %1x%2@%3,%4；编辑控件 摆在 %5 可见=%6；按钮 %7 颗")
                             .arg(qRound(headerScene.width())).arg(qRound(headerScene.height()))
                             .arg(sceneInHost.x() + qRound(headerScene.x()))
                             .arg(sceneInHost.y() + qRound(headerScene.y()))
                             .arg(widgetRect.isValid()
                                      ? QStringLiteral("%1,%2 %3x%4").arg(widgetRect.x())
                                              .arg(widgetRect.y()).arg(widgetRect.width())
                                              .arg(widgetRect.height())
                                      : QStringLiteral("无效"))
                             .arg(widgetUp).arg(btns.size());
        int covered = 0, buttonsOk = 0;
        QString who;
        for (const QVariant &v : btns) {
            const QRectF b = v.toRectF();
            if (b.width() <= 0 || b.height() <= 0)
                continue;
            ++buttonsOk;
            const QPoint center(sceneInHost.x() + qRound(b.center().x()),
                                sceneInHost.y() + qRound(b.center().y()));
            if (widgetUp && widgetRect.contains(center)) {
                ++covered;
                who += QStringLiteral(" 编辑控件盖住%1,%2").arg(center.x()).arg(center.y());
            }
        }
        tcheck(docOpen && covered == 0 && buttonsOk == 4,
               QStringLiteral("%1：打开文档之后，标签条右边那 4 颗按钮没被编辑区那块控件盖住")
                   .arg(QLatin1String(tag)),
               QStringLiteral("文档已开=%1 按钮找到 %2 颗、被盖 %3 颗%4；%5")
                   .arg(docOpen).arg(buttonsOk).arg(covered)
                   .arg(who, report));
        /* 真实桌面裁一条：数字对不上时靠它看现场 */
        const QImage desk = QGuiApplication::primaryScreen()->grabWindow(0).toImage();
        const QRect crop(sceneInHost.x() + qRound(headerScene.x()),
                         sceneInHost.y() + qRound(headerScene.y()) - 4,
                         qRound(headerScene.width()), qRound(headerScene.height()) + 8);
        if (desk.rect().contains(crop))
            desk.copy(crop).save(QStringLiteral("H:/steward/build/term-header-%1.png")
                                     .arg(QLatin1String(tag)));
        };
        measure("常规高度");
        qmlRoot->setProperty("terminalMaximized", true);
        waitUntil([&] { return view->rows() > 100; }, 4000);
        for (int k = 0; k < 30; ++k)
            QCoreApplication::processEvents();
        measure("面板最大化");
        qmlRoot->setProperty("terminalMaximized", false);
        waitUntil([&] { return view->rows() < 60; }, 4000);

        /*
         * 第三种顺序才是嫌疑最大的那一种：**先开文档、后开面板**。
         * 这时编辑区那块控件已经按"没有面板"的高度摆好了，面板再挤进来 ——
         * 它要是慢半拍没跟着收，就直接压到标签条上（上面两种顺序量出来只差 5px，
         * 也就是说只要它滞后一格，按钮就没了）。
         * 开完面板**立刻量一遍**（不等它落定），再等稳量第二遍。
         */
        QMetaObject::invokeMethod(qmlRoot, "dispatch",
                                  Q_ARG(QVariant, QStringLiteral("toggleTerminal")));
        if (waitUntil([&] { return !panel->property("visible").toBool(); }, 1500)) {
            QMetaObject::invokeMethod(qmlRoot, "dispatch",
                                      Q_ARG(QVariant, QStringLiteral("toggleTerminal")));
            waitUntil([&] { return panel->property("visible").toBool(); }, 1500);
            measure("后开面板·当场");
            for (int k = 0; k < 40; ++k)
                QCoreApplication::processEvents();
            measure("后开面板·落定");
        } else {
            tcheck(false, QStringLiteral("后开面板这一种顺序没能复现（面板收不掉）"),
                   QStringLiteral("visible=%1").arg(panel->property("visible").toBool()));
        }

        /*
         * 用户确认了第 1 条藏起来的是**悬停气泡**。那就别读属性了 —— 属性永远是对的，
         * "被那块原生控件盖住"这件事只有在真实桌面上才看得见：
         * 真把一个气泡 open 出来，抓桌面，读它中心那一像素，看是不是气泡自己的底色。
         * 修之前它整个弹在标签条**上方** = 编辑区那块矩形里，读回来是编辑区的颜色。
         */
        {
            QVariant tv2;
            QMetaObject::invokeMethod(panel, "headerTips", Q_RETURN_ARG(QVariant, tv2));
            const QVariantList tips = tv2.toList();
            QObject *tip = tips.isEmpty() ? nullptr : tips[0].value<QObject *>();
            bool onTop = false;
            QString detail = QStringLiteral("一个气泡对象都没拿到");
            if (tip) {
                tip->setProperty("text", QStringLiteral("自检气泡"));
                /*
                 * 走界面那条路：置 hovered，让组件自己的计时器去 open。
                 * 直接写 visible 会被它自己的 onHoveredChanged 关掉
                 * （前面几条判据发过合成鼠标，hovered 一变假就把气泡收了 ——
                 * 上一轮读回来是正文底色 #1e1f22、visible=0，就是这么没的）。
                 */
                tip->setProperty("hovered", true);
                /*
                 * 得等它**淡入完**再抓：Popup 的 enter 过渡有 200 多毫秒，
                 * 只泵 40 拍事件就抓，量到的是气泡还没画出来时背后的那块正文色
                 * （上一轮就是这么红的：读到 #1e1f22 = 终端正文底色，差 41）。
                 */
                waitUntil([&] { return tip->property("visible").toBool(); }, 3000);
                for (int k = 0; k < 50; ++k) {
                    QCoreApplication::processEvents();
                    QThread::msleep(20);
                }
                auto *parItem = qobject_cast<QQuickItem *>(
                    tip->property("parent").value<QObject *>());
                const QPointF local(tip->property("x").toReal(),
                                    tip->property("y").toReal());
                const QPointF scene = parItem ? parItem->mapToScene(local) : local;
                auto *qwTip = host->findChild<QQuickWidget *>();
                const QPoint sceneInHostTip = qwTip ? qwTip->mapTo(host, QPoint(0, 0)) : QPoint();
                const QPoint inHost(sceneInHostTip.x() + qRound(scene.x()),
                                    sceneInHostTip.y() + qRound(scene.y()));
                const QSizeF tipSize(tip->property("width").toReal(),
                                     tip->property("height").toReal());
                const QRectF tipInHost(QPoint(inHost.x(), inHost.y()), tipSize);
                /*
                 * 门禁换成**几何**判据，不看桌面：气泡那个矩形和编辑区那块控件的矩形
                 * 相不相交。相交就是"被盖住"，这是这件事的定义本身，而且不受
                 * 谁在前台影响（上一轮拿抓屏当门禁，结果那一点上盖着桌面壁纸，
                 * 读回来 #0f0019 —— 量的根本不是我们）。
                 */
                QVariantMap pane2;
                if (EditorViewItem *ev2 = EditorViewItem::instance())
                    pane2 = ev2->paneGeometryForTest();
                const QRectF editorRect(pane2.value(QStringLiteral("widgetX")).toInt(),
                                        pane2.value(QStringLiteral("widgetY")).toInt(),
                                        pane2.value(QStringLiteral("widgetW")).toInt(),
                                        pane2.value(QStringLiteral("widgetH")).toInt());
                const bool editorUp = pane2.value(QStringLiteral("widgetVisible")).toBool();
                QVariant hv2;
                QMetaObject::invokeMethod(panel, "headerRect", Q_RETURN_ARG(QVariant, hv2));
                const QRectF hdr = hv2.toRectF();
                const bool clearOfEditor = !editorUp
                                           || !editorRect.intersects(tipInHost.toRect());
                const bool belowHeader = tipInHost.y() >= hdr.bottom() - 1.0;
                onTop = clearOfEditor && belowHeader;
                const QPoint center = inHost + QPoint(qRound(tipSize.width() / 2.0),
                                                      qRound(tipSize.height() / 2.0));
                const QPoint glo = host->mapToGlobal(center);
                const QImage desk = QGuiApplication::primaryScreen()->grabWindow(0).toImage();
                QString shot = QStringLiteral("没抓");
                if (desk.rect().contains(glo))
                    shot = QColor::fromRgba(desk.pixel(glo)).name();
                detail = QStringLiteral("气泡在宿主 %1，编辑控件 %2 可见=%3，相交=%4，"
                                        "标签条底边=%5 → 贴在下面=%6；同一针抓屏读到 %7"
                                        "（只报数：那一点上可能盖着别的窗口）")
                             .arg(QStringLiteral("%1,%2 %3x%4").arg(inHost.x()).arg(inHost.y())
                                      .arg(qRound(tipSize.width())).arg(qRound(tipSize.height())),
                                  editorRect.isValid()
                                      ? QStringLiteral("%1,%2 %3x%4").arg(editorRect.x())
                                              .arg(editorRect.y()).arg(editorRect.width())
                                              .arg(editorRect.height())
                                      : QStringLiteral("无效"))
                             .arg(editorUp)
                             .arg(editorUp && !clearOfEditor)
                             .arg(hdr.bottom())
                             .arg(belowHeader).arg(shot);
                desk.copy(QRect(glo - QPoint(90, 22), QSize(180, 44)).intersected(
                          desk.rect()))
                    .save(QStringLiteral("H:/steward/build/term-tip.png"));
                tip->setProperty("hovered", false);
                for (int k = 0; k < 10; ++k)
                    QCoreApplication::processEvents();
            }
            tcheck(onTop,
                   QStringLiteral("终端标签条的气泡贴在按钮下面，没落进编辑区那块矩形的范围"),
                   QStringLiteral("文档已开=%1；%2；现场图 H:/steward/build/term-tip.png")
                       .arg(docOpen).arg(detail));
        }
    }

    /*
     * 用户 2026-09-23 第 2 条：垃圾桶（"清屏（含回滚）"）点了没反应，屏上那些字还在。
     *
     * 判据钉的是"那些字没了"，不是"屏全空" —— 清完之后 shell 自己会不会重画一行提示符
     * 是另一件事（现在的答案是不会，我们只擦显示、没替它敲 Ctrl+L），
     * 所以这里量两样：灌进去的标记还在不在，以及回滚是不是也清了。
     * 再等 1.2 秒量第二遍：ConPTY 手里那份缓冲要是把刚擦掉的行又倒回来，第一遍会绿、
     * 第二遍会红。
     */
    {
        e->sendText(QStringLiteral("1..40 | ForEach-Object { \"clr $_\" }"));
        e->sendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
        const bool shown = waitUntil(
            [&] { return allText(e).contains(QLatin1String("clr 40")); }, 20000);
        auto nonEmptyWithClr = [&]() -> QString {
            int n = 0;
            for (int r = -e->historyRows(); r < e->rows(); ++r) {
                const QString line = e->lineText(r).trimmed();
                if (line.isEmpty())
                    continue;
                ++n;
            }
            return QStringLiteral("%1 行有字/回滚 %2/还留着 clr=%3")
                .arg(n).arg(e->historyRows())
                .arg(allText(e).contains(QLatin1String("clr ")) ? 1 : 0);
        };
        view->clearBuffer();
        QCoreApplication::processEvents();
        const QString rightAfter = nonEmptyWithClr();
        for (int k = 0; k < 60; ++k) {
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
        const QString later = nonEmptyWithClr();
        /*
         * 只判 1.2 秒后那一遍：Ctrl+L 是发给 shell 的，屏是它重画出来的，
         * 当场读必然还是旧的（上一版把"立刻"也写进判据，红了个假 ——
         * 量到 17 行有字，那正是还没回来的重画）。
         */
        tcheck(shown && !later.contains(QLatin1String("clr=1")) && e->historyRows() == 0,
               QStringLiteral("垃圾桶真能清空当前终端展示的数据（含回滚，1.2 秒后没被重画回来）"),
               QStringLiteral("灌进去=%1 立刻（异步，只报数）：%2；1.2 秒后：%3")
                   .arg(shown).arg(rightAfter, later));

        /*
         * 他截图里另外两件事：清完**第一行还得是提示符**（"PS C:\Users\XIAOJOO>"），
         * 以及再输入的时候不许"跳格"。
         * 跳格的成因就是我上一版的修法：我们擦自己的屏、ConPTY 的游标还在老地方，
         * 它下一次重画按**它的**绝对坐标发，字就落在中间某一行、还带缩进。
         * 所以这两条才是这件事真正的判据 —— 判的是"两边游标没分家"。
         */
        int firstRow = -1;
        QString firstText;
        for (int r = 0; r < e->rows(); ++r) {
            const QString t = e->lineText(r);
            if (t.trimmed().isEmpty())
                continue;
            firstRow = r;
            firstText = t;
            break;
        }
        tcheck(firstRow == 0 && firstText.startsWith(QStringLiteral("PS ")),
               QStringLiteral("清屏之后提示符重画在第一行（不是一片空白）"),
               QStringLiteral("第一个有字的行是第 %1 行：「%2」").arg(firstRow).arg(firstText.left(46)));

        e->sendText(QStringLiteral("echo ZZFLUSH"));
        e->sendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
        int markRow = -1;
        bool flushLeft = false;
        /*
         * 找不着就再敲一次回车：刚发过 Ctrl+L，shell 还在重画，那一下回车有概率被吃掉
         * （这一条红过一次：只量到命令行「PS …> echo ZZFLUSH」，没量到输出行）。
         * 重试的是**输入**，不是放宽判据 —— 顶格这条一个字都不让。
         */
        for (int attempt = 0; attempt < 2 && !flushLeft; ++attempt) {
            if (attempt > 0)
                e->sendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
            waitUntil([&] {
                for (int r = 0; r < e->rows(); ++r) {
                    if (e->lineText(r) == QStringLiteral("ZZFLUSH")) {
                        markRow = r;
                        flushLeft = true;
                        return true;
                    }
                }
                return false;
            }, 12000);
        }
        if (!flushLeft) {
            for (int r = 0; r < e->rows(); ++r) {
                if (e->lineText(r).contains(QLatin1String("ZZFLUSH"))) {
                    markRow = r;
                    break;
                }
            }
        }
        tcheck(flushLeft,
               QStringLiteral("清屏之后再敲一条命令，输出顶到第 0 列（两边游标没分家、不跳格）"),
               QStringLiteral("标记在第 %1 行，整行原文「%2」（顶格才等于 ZZFLUSH）")
                   .arg(markRow).arg(markRow >= 0 ? e->lineText(markRow) : QString()));
    }

    /*
     * 用户 2026-09-23："终端页面不能选中，复制等。"
     *
     * 量法：事件**直接发给 TerminalView 这块 item**（不是发给 QQuickWidget）。
     * 为什么：合成鼠标驱动不了"拖动"那一层（这工程早就记过），但 item 自己的
     * mousePress/Move/Release 是 QQuickItem::event() 直接分发的，发得进去 ——
     * 而我要验的恰好就是这三个处理函数写得对不对，不是场景的命中测试。
     *
     * 松手之后还要读一遍 hasSelection：原来松手走的是 copySelection()，
     * 它顺手清选区，于是高亮当场没了 —— 用户看到的就是"根本选不中"。
     */
    {
        e->sendText(QStringLiteral("1..6 | ForEach-Object { \"sel $_\" }"));
        e->sendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE);
        int row = -1;
        waitUntil([&] {
            for (int r = 0; r < e->rows(); ++r) {
                if (e->lineText(r).contains(QLatin1String("sel 1"))) {
                    row = r;
                    return true;
                }
            }
            return false;
        }, 20000);
        /* 等 shell 彻底不出了再量：一边滚一边选，复制和后面读选区之间会又错开一行 */
        for (int k = 0; k < 90; ++k) {
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
        row = -1;
        for (int r = 0; r < e->rows(); ++r)
            if (e->lineText(r).contains(QLatin1String("sel 1")))
                row = r;
        const QPointF a(view->padding() + view->cellWidth() * 0.5,
                        row * view->cellHeight() + view->cellHeight() * 0.5);
        const QPointF b(view->padding() + view->cellWidth() * 12, a.y());
        const QPoint g = host->mapToGlobal(a.toPoint());
        QMouseEvent pr(QEvent::MouseButtonPress, a, g, Qt::LeftButton, Qt::LeftButton,
                       Qt::NoModifier);
        QMouseEvent mv(QEvent::MouseMove, b, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent rl(QEvent::MouseButtonRelease, b, g, Qt::LeftButton, Qt::NoButton,
                       Qt::NoModifier);
        QCoreApplication::sendEvent(view, &pr);
        QCoreApplication::sendEvent(view, &mv);
        QCoreApplication::sendEvent(view, &rl);
        QCoreApplication::processEvents();
        const QString sel = view->selectedText();
        const bool stillSelected = view->hasSelection();
        const QString clip = QGuiApplication::clipboard()->text();
        tcheck(row >= 0 && sel.contains(QLatin1String("sel")) && stillSelected,
               QStringLiteral("正文里拖选能选中，松手之后高亮还在（不再一松手就没）"),
               QStringLiteral("第 %1 行，选中文字「%2」，松手后仍有选区=%3")
                   .arg(row).arg(sel.trimmed().left(40)).arg(stillSelected));
        tcheck(!clip.isEmpty() && clip == sel,
               QStringLiteral("松手那一刻选中的字进了剪贴板"),
               QStringLiteral("剪贴板「%1」/ 选区「%2」")
                   .arg(clip.trimmed().left(40), sel.trimmed().left(40)));
        view->clearSelection();

        /* 右键 → 那套自绘菜单 → 点"全选"，走的是和界面上完全同一条路 */
        QMouseEvent rc(QEvent::MouseButtonPress, a, g, Qt::RightButton, Qt::RightButton,
                       Qt::NoModifier);
        QCoreApplication::sendEvent(view, &rc);
        for (int k = 0; k < 20; ++k)
            QCoreApplication::processEvents();
        /* QQuickWidget 里那些 QML 对象的 **QObject 父**挂在那块离屏窗口上，不在
         * QWidget 树里 —— 从 host 控件往下 findChild 永远找不到，得从窗口找。
         * （注意这跟 Popup 的 `parent` 属性是两件事：那个要的是 Item，2026-09-24
         * 之前一直赋值失败，见下面 hostHeight 那条。） */
        QObject *menu = view->window() ? view->window()->findChild<QObject *>("terminalMenu")
                                          : nullptr;
        if (!menu && panel)
            menu = panel->findChild<QObject *>("terminalMenu");
        QString menuProbe;
        if (!menu) {
            /* 还找不到就把对象树里所有 Popup 类列出来：到底是没建出来，还是挂在别处 */
            menuProbe = QStringLiteral(" 面板子孙=%1 个，里面的 Popup:")
                            .arg(panel ? panel->findChildren<QObject *>().size() : -1);
            if (panel) {
                const auto kids = panel->findChildren<QObject *>();
                for (QObject *k : kids) {
                    const QString cn = QString::fromLatin1(k->metaObject()->className());
                    if (cn.contains(QLatin1String("Popup")))
                        menuProbe += QStringLiteral(" [%1|%2]").arg(cn, k->objectName());
                }
            }
        }
        const bool opened = menu && menu->property("visible").toBool();
        QStringList labels;
        const QVariantList entries = menu ? menu->property("entries").toList() : QVariantList();
        for (const QVariant &v : entries) {
            const QVariantMap m = v.toMap();
            labels << (m.contains(QStringLiteral("separator")) ? QStringLiteral("-")
                                                               : m.value(QStringLiteral("label")).toString());
        }
        bool allWorks = false;
        if (opened) {
            QMetaObject::invokeMethod(menu, "selected", Q_ARG(QString, QStringLiteral("term-all")));
            for (int k = 0; k < 10; ++k)
                QCoreApplication::processEvents();
            allWorks = view->hasSelection() && view->selectedText().contains(QLatin1String("sel"));
            view->clearSelection();
            menu->setProperty("visible", false);
        }
        tcheck(opened && labels.size() == 9 && allWorks,
               QStringLiteral("右键弹的是那套自绘菜单，条目齐，点「全选」真能全选"),
               QStringLiteral("找得到菜单=%1 开了=%2 条目[%3] 全选生效=%4%5")
                   .arg(menu != nullptr).arg(opened).arg(labels.join("|")).arg(allWorks)
                   .arg(menuProbe));
        /*
         * 宿主到底挂上没有。DropdownMenu 的 hostHeight 写的是
         * `root.parent ? root.parent.height : 800` —— parent 没挂上时它静默退化成
         * 800，于是贴边夹取、子栏可用空间那一整套账全部白算，而界面上只看得到
         * 一条 "Unable to assign QQuickWidgetOffscreenWindow to QQuickItem"
         * （parent 那一句原来写的是 `Window.window`，QQuickWidget 里那是离屏
         * QWindow、不是 Item，赋值一直失败）。钉"读到的是窗口真实高度"，
         * 退化回 800 时这条立刻红。
         */
        const double menuHostH = menu ? menu->property("hostHeight").toDouble() : -1.0;
        const double winH = qmlRoot->property("height").toDouble();
        tcheck(menu && qAbs(menuHostH - winH) < 1.0,
               QStringLiteral("终端右键菜单的宿主是窗口本体（不是退化那个 800）"),
               QStringLiteral("菜单读到的宿主高 %1 / 窗口高 %2")
                   .arg(menuHostH).arg(winH));
    }

    const qreal heightBeforeCollapse = panel->property("wantedHeight").toReal();
    QMetaObject::invokeMethod(qmlRoot, "dispatch", Q_ARG(QVariant, QStringLiteral("toggleTerminal")));
    tcheck(heightBeforeCollapse > 100 && waitUntil([&] {
               return !panel->property("visible").toBool()
                      && panel->property("wantedHeight").toReal() < 2;
           }, 1500),
           QStringLiteral("再按一次能收起（槽位归 0、面板不可见）"),
           QStringLiteral("visible=%1 wanted=%2 height=%3")
               .arg(panel->property("visible").toBool())
               .arg(panel->property("wantedHeight").toReal())
               .arg(panel->property("height").toReal())
               .arg(heightBeforeCollapse));

    /*
     * 用户 2026-09-23 要的那条：最后一个标签关掉 = 面板也关掉。
     * 量三步：开第二条 → 关一条（面板必须还开着，别提前收）→ 关最后一条（面板要收起）。
     * 然后再展开一次：模型是空的，展开必须自己补一条并真起一个 shell，
     * 不然就是"关掉之后再也打不开"这种更难看的毛病。
     */
    {
        const bool hiddenBefore = qmlRoot->property("terminalHidden").toBool();
        if (hiddenBefore) {
            QMetaObject::invokeMethod(qmlRoot, "dispatch",
                                      Q_ARG(QVariant, QStringLiteral("toggleTerminal")));
            waitUntil([&] { return !qmlRoot->property("terminalHidden").toBool(); }, 2000);
            waitUntil([&] { return panel->property("sessionCount").toInt() > 0; }, 3000);
        }
        QMetaObject::invokeMethod(panel, "newSession");
        QCoreApplication::processEvents();
        const int two = panel->property("sessionCount").toInt();
        QMetaObject::invokeMethod(panel, "closeSession", Q_ARG(QVariant, 0));
        for (int k = 0; k < 20; ++k)
            QCoreApplication::processEvents();
        const int one = panel->property("sessionCount").toInt();
        const bool stillOpen = !qmlRoot->property("terminalHidden").toBool();
        QMetaObject::invokeMethod(panel, "closeSession", Q_ARG(QVariant, 0));
        const bool closedWithLast = waitUntil(
            [&] { return qmlRoot->property("terminalHidden").toBool()
                       && panel->property("sessionCount").toInt() == 0; }, 2000);
        tcheck(two == 2 && one == 1 && stillOpen && closedWithLast,
               QStringLiteral("关掉最后一个标签 = 面板跟着收起（关掉一条不会提前收）"),
               QStringLiteral("新建后 %1 条 / 关一条后 %2 条面板还开着=%3 / 关最后一条后收起=%4")
                   .arg(two).arg(one).arg(stillOpen).arg(closedWithLast));

        QMetaObject::invokeMethod(qmlRoot, "dispatch",
                                  Q_ARG(QVariant, QStringLiteral("toggleTerminal")));
        const bool refilled = waitUntil(
            [&] { return panel->property("sessionCount").toInt() == 1; }, 3000);
        QObject *backView = nullptr;
        waitUntil([&] {
            QVariant fv;
            QMetaObject::invokeMethod(panel, "firstView", Q_RETURN_ARG(QVariant, fv));
            backView = fv.value<QObject *>();
            return backView && backView->property("running").toBool();
        }, 15000);
        const bool runningAgain = backView && backView->property("running").toBool();
        tcheck(refilled && runningAgain,
               QStringLiteral("关掉之后再展开：自动补一条会话并且 shell 真起得来"),
               QStringLiteral("补回一条=%1 会话在跑=%2").arg(refilled).arg(runningAgain));
        /* 还原来面板的开合状态，别把用户的存档带跑 */
        if (qmlRoot->property("terminalHidden").toBool() != hiddenBefore) {
            QMetaObject::invokeMethod(qmlRoot, "dispatch",
                                      Q_ARG(QVariant, QStringLiteral("toggleTerminal")));
            for (int k = 0; k < 30; ++k)
                QCoreApplication::processEvents();
        }
    }

    /*
     * 图标条底部那一格：终端**收起 / 展开 / 最大化**三种状态下必须在同一个 y。
     *
     * 2026-09-23 他报"终端展开的时候那一格跟着往上跑"。原来整条图标条塞在
     * midRow 的布局槽位里：面板一展开图标条就矮一截，而底部那两格是从**顶部**
     * 量出去的一根弹簧（高度 = 图标条高 - 300），于是跟着一起挪；面板最大化时
     * midRow 整个 hide 掉，图标条连人都没了。现在图标条从顶栏下沿直接铺到
     * 状态栏上沿，底部那一组 anchors 到图标条下沿 —— 三种状态量同一个 y。
     */
    {
        auto ui = [&]() {
            QVariant r;
            QMetaObject::invokeMethod(qmlRoot, "uiState", Q_RETURN_ARG(QVariant, r));
            return r.toMap();
        };
        auto settle = [&]() {
            for (int k = 0; k < 25; ++k) {
                QCoreApplication::processEvents();
                QThread::msleep(20);
            }
        };
        auto cellY = [](const QVariantMap &m) {
            return m.value(QLatin1String("termCellY")).toReal();
        };
        const bool hiddenWas = qmlRoot->property("terminalHidden").toBool();
        const bool maxWas = qmlRoot->property("terminalMaximized").toBool();

        if (!qmlRoot->property("terminalHidden").toBool()) {
            QMetaObject::invokeMethod(qmlRoot, "dispatch",
                                      Q_ARG(QVariant, QStringLiteral("toggleTerminal")));
            waitUntil([&] { return qmlRoot->property("terminalHidden").toBool(); }, 2000);
        }
        /*
         * 真像素复查：量到的 y 对不对是一回事，屏幕上那一格**真画在那儿**是另一回事
         * （这个工程里"几何绿了界面却不对"出过好几次）。采样点取格子左上角往里
         * 4px —— 图标是居中的 16px，那个角只会是格子底色，不会是图标。
         */
        auto grabCell = [&](const QVariantMap &m, const QColor &want, const char *tag) {
            if (!host)
                return QColor();
            host->raise();
            host->activateWindow();
            for (int k = 0; k < 15; ++k) {
                QCoreApplication::processEvents();
                QThread::msleep(20);
            }
            auto *qw = host->findChild<QQuickWidget *>();
            const QPoint inHost = (qw ? qw->mapTo(host, QPoint(0, 0)) : QPoint())
                + QPoint(qRound(m.value(QLatin1String("termCellX")).toReal()) + 4,
                         qRound(cellY(m)) + 4);
            const QPoint glo = host->mapToGlobal(inHost);
            /*
             * 抓**宿主 QWidget**，不抓桌面：桌面上随时有别的项目窗口压着这一条
             * （实测三次采到同一个 #7a782e，那是浏览器里一张照片，不是本程序）。
             * host->grab() 直接把这块控件渲染出来，遮挡与它无关。
             */
            const QImage shot = host->grab().toImage();
            const QPoint inWidget = inHost;   // 已经是宿主 QWidget 坐标
            if (!shot.rect().contains(inWidget))
                return QColor();
            const QColor got = QColor::fromRgba(shot.pixel(inWidget));
            shot.copy(QRect(inWidget - QPoint(20, 20), QSize(120, 120)).intersected(shot.rect()))
                .save(QStringLiteral("H:/steward/build/nav-cell-%1.png").arg(tag));
            Q_UNUSED(glo);
            return got;
        };

        settle();
        const QVariantMap closed = ui();
        const QColor gotClosed =
            grabCell(closed, QColor(QStringLiteral("#313335")), "closed");

        QMetaObject::invokeMethod(qmlRoot, "dispatch",
                                  Q_ARG(QVariant, QStringLiteral("toggleTerminal")));
        const bool opened = waitUntil([&] {
            return !qmlRoot->property("terminalHidden").toBool()
                       && panel->property("height").toReal() > 100.0;
        }, 3000);
        settle();
        const QVariantMap open = ui();
        const QColor gotOpen = grabCell(open, QColor(QStringLiteral("#3a4a5a")), "open");

        qmlRoot->setProperty("terminalMaximized", true);
        settle();
        settle();
        const QVariantMap maxi = ui();
        const QColor gotMax = grabCell(maxi, QColor(QStringLiteral("#3a4a5a")), "max");
        const bool midRowGone = maxi.value(QLatin1String("midRowVisible")).toBool() == false;

        qmlRoot->setProperty("terminalMaximized", maxWas);
        if (qmlRoot->property("terminalHidden").toBool() != hiddenWas) {
            QMetaObject::invokeMethod(qmlRoot, "dispatch",
                                      Q_ARG(QVariant, QStringLiteral("toggleTerminal")));
        }
        settle();

        /*
         * 浅色档：同一个采样点应该从 #313335 翻成 #f2f2f2。
         * 这条量的是"切换真的作用到渲染上"，不是只量到属性翻了。
         */
        if (theme)
            theme->setLight(true);
        settle();
        const QColor gotLight = grabCell(closed, QColor(QStringLiteral("#f2f2f2")), "light");
        if (host)
            host->grab().toImage().save(QStringLiteral("H:/steward/build/theme-light.png"));
        if (theme)
            theme->setLight(false);
        settle();
        if (host)
            host->grab().toImage().save(QStringLiteral("H:/steward/build/theme-dark.png"));

        const double yc = cellY(closed), yo = cellY(open), ym = cellY(maxi);
        const double winH = closed.value(QLatin1String("windowHeight")).toReal();
        const double sbH = closed.value(QLatin1String("statusBarHeight")).toReal();
        const double gearBottom =
            closed.value(QLatin1String("themeCellY")).toReal() + 26.0;
        const bool samePos = qAbs(yc - yo) < 1.0 && qAbs(yc - ym) < 1.0;
        const bool alwaysVisible =
            closed.value(QLatin1String("stripVisible")).toBool()
            && open.value(QLatin1String("stripVisible")).toBool()
            && maxi.value(QLatin1String("stripVisible")).toBool();
        const bool atBottom = yc > winH / 2.0 && qAbs(winH - sbH - gearBottom) <= 12.0;
        auto pxDist = [](const QColor &got, const char *want) {
            const QColor w(QString::fromLatin1(want));
            return qAbs(got.red() - w.red()) + qAbs(got.green() - w.green())
                   + qAbs(got.blue() - w.blue());
        };
        const int dClosed = pxDist(gotClosed, "#313335");
        const int dOpen = pxDist(gotOpen, "#3a4a5a");
        const int dMax = pxDist(gotMax, "#3a4a5a");
        const int dLight = pxDist(gotLight, "#f2f2f2");
        const bool pixelsOk = dClosed <= 30 && dOpen <= 30 && dMax <= 30;
        /* 深色那格和浅色那格必须是两个颜色，否则"切了没反应"也能过上面那条 */
        const bool themeFlips = dLight <= 30
                                && (gotLight.red() - gotClosed.red()
                                    + gotLight.green() - gotClosed.green()
                                    + gotLight.blue() - gotClosed.blue()) > 60;
        tcheck(opened && samePos && alwaysVisible && atBottom && pixelsOk && themeFlips,
               QStringLiteral("终端那一格在图标条底部，收起 / 展开 / 最大化三种状态位置一样"),
               QStringLiteral("收起 y=%1，展开 y=%2，最大化 y=%3（窗口高 %4、底栏 %5）；"
                              "图标条三种状态都在=%6，最大化时中间行确实让开了=%7，"
                              "展开到位=%8；设置那格底边 %9，离状态栏 %10；"
                              "图标条顶 %11 高 %12 槽位宽 %13；"
                              "切到白色之后同一格 %20(该是 #f2f2f2，差%21)；"
                              "屏上那一格底色 收起 %14(差%15) 展开 %16(差%17) 最大化 %18(差%19)，"
                              "三张图存 H:/steward/build/nav-cell-*.png")
                   .arg(yc).arg(yo).arg(ym).arg(winH).arg(sbH)
                   .arg(alwaysVisible).arg(midRowGone).arg(opened)
                   .arg(gearBottom).arg(winH - sbH - gearBottom)
                   .arg(closed.value(QLatin1String("stripTop")).toReal())
                   .arg(closed.value(QLatin1String("stripHeight")).toReal())
                   .arg(closed.value(QLatin1String("navSlotWidth")).toReal())
                   .arg(gotClosed.name()).arg(dClosed)
                   .arg(gotOpen.name()).arg(dOpen)
                   .arg(gotMax.name()).arg(dMax)
                   .arg(gotLight.name()).arg(dLight));
    }

    /* 主题这一档现在是什么（量具先证明它读得到活路径，再谈断言） */
    {
        QVariant r;
        QMetaObject::invokeMethod(qmlRoot, "uiState", Q_RETURN_ARG(QVariant, r));
        const QVariantMap m = r.toMap();
        tout(QStringLiteral("（主题）注册表 ui/theme=%1  QML Theme.light=%2  图标条底该是 %3")
                 .arg(QSettings().value(QStringLiteral("ui/theme")).toString())
                 .arg(m.value(QStringLiteral("themeLight")).toBool())
                 .arg(m.value(QStringLiteral("themeChrome")).toString()));
    }

    /*
     * 内容区的选中底色：真渲染上比"取消选择 / 全选"两头。
     *
     * 2026-09-23 他挑了 A 档 #e6effc（贴他参考图那块淡蓝 #f1f6fe）。判据两头都要量：
     * 只数"屏上有没有这个色"会被当前行底色、滚动条撞车。放在这一节的最后跑 ——
     * 主自检那边中段插进去会扰动后面的用例（实测把"整词匹配"那条带偏成第 3 行）。
     * 扫描必须覆盖整张抓图：正文只有几行、都在左边，早先按 40%% 宽起扫把选区整个扫掉了。
     */
    {
        EditorViewItem *ev = EditorViewItem::instance();
        auto pump = []() {
            for (int k = 0; k < 20; ++k) {
                QCoreApplication::processEvents();
                QThread::msleep(20);
            }
        };
        int before = -1, after = -1, selLen = -1, paper = -1;
        const bool lightWas2 = theme && theme->light();
        if (theme)
            theme->setLight(true);
        if (ev) {
            ev->dbgClearSelectionForTest();
            pump();
            before = ev->dbgCountNearForTest(QStringLiteral("#a8cdf5"));
            ev->selectAll();
            pump();
            after = ev->dbgCountNearForTest(QStringLiteral("#a8cdf5"));
            selLen = ev->selectionLength();
            paper = ev->dbgCountNearForTest(QStringLiteral("#ffffff"));
            ev->dbgClearSelectionForTest();
            pump();
        }
        if (theme)
            theme->setLight(lightWas2);
        /*
         * 预览那份 HTML：颜色在 CSS 字符串里，切档必须重新生成过。
         * 直接问 Cmd.markdownHtml() 要一份，看内联样式里出现的是浅色档的字色
         * 还是深色档那套 #e8e8e8（后者就是"白底上一片淡字"那个毛病）。
         */
        QString cssLight, cssDark;
        if (cmd) {
            const QString md = QStringLiteral("# 标题\n正文一段\n\n代码块\n");;
            if (theme) theme->setLight(true);
            cssLight = cmd->markdownHtml(md, QString());
            if (theme) theme->setLight(false);
            cssDark = cmd->markdownHtml(md, QString());
            if (theme) theme->setLight(lightWas2);
        }
        tcheck(!cssLight.contains(QStringLiteral("#e8e8e8"))
               && cssLight.contains(QStringLiteral("#1d2129"))
               && cssDark.contains(QStringLiteral("#e8e8e8")),
               QStringLiteral("预览的 CSS 跟着主题重生成（浅色档不再有 #e8e8e8 淡字）"),
               QStringLiteral("浅色档含 #1d2129=%1 还含 #e8e8e8=%2；深色档含 #e8e8e8=%3")
                   .arg(cssLight.contains(QStringLiteral("#1d2129")))
                   .arg(cssLight.contains(QStringLiteral("#e8e8e8")))
                   .arg(cssDark.contains(QStringLiteral("#e8e8e8"))));

        tcheck(before >= 0 && before <= 40 && after >= before + 400,
               QStringLiteral("内容区选中底色在渲染上就是那档蓝 #a8cdf5（不选时几乎为 0）"),
               QStringLiteral("取消选择 %1 个像素 → 全选 %2 个（ΔE<=12）；选区长度=%3，"
                              "同一次抓图里纸色 %4 个")
                   .arg(before).arg(after).arg(selLen).arg(paper));
    }

    /*
     * 浅色档 ANSI 调色板：直接问引擎"某一号现在是什么 RGB"。
     * libvterm 自带的 3 号是 #e0e000（纯黄，PowerShell 提示符那条路径就用它），
     * 浅色档必须换成 #9a6700；切回深色必须**16 格逐字退回** —— 两条一起量才说明
     * "浅色换了、深色没被动过"。
     *
     * 为什么放在整个终端自检最后：它要来回切主题档，而切档之后的**像素**判据会读到
     * 还没重画的旧色 —— 图标条那一格（#f2f2f2）就是被它打成 #ffffff 的，实测。
     * 这一段只问引擎里的颜色、不抓像素，所以放末尾安全。
     */
    if (AppTheme::instance() && view) {
        AppTheme *th = AppTheme::instance();
        const bool wasLight = th->light();
        auto rowOf = [view]() {
            QString s;
            for (int i = 0; i < 16; ++i)
                s += view->ansiColorForTest(i).name() + QLatin1Char(' ');
            return s.trimmed();
        };
        const QString darkRow = rowOf();
        th->setLight(true);
        QCoreApplication::processEvents();
        const QString lightRow = rowOf();
        th->setLight(wasLight);
        QCoreApplication::processEvents();
        const QString backRow = rowOf();
        const QString d3 = darkRow.section(QLatin1Char(' '), 3, 3);
        const QString l3 = lightRow.section(QLatin1Char(' '), 3, 3);
        tcheck(d3 == QStringLiteral("#e0e000") && l3 == QStringLiteral("#9a6700")
                   && darkRow.compare(backRow, Qt::CaseInsensitive) == 0,
               QStringLiteral("终端浅色档换掉那套 ANSI 色、切回深色 16 格逐字退回原值"),
               QStringLiteral("3 号：深色 %1 → 浅色 %2；切回来整套逐字相同=%3")
                   .arg(d3, l3).arg(darkRow.compare(backRow, Qt::CaseInsensitive) == 0));
        /* 两整套都打出来：以后调这套色照这一行对，不用再去猜 libvterm 的值 */
        tout(QStringLiteral("     （ANSI 深色档 libvterm 自带：%1）").arg(darkRow));
        tout(QStringLiteral("     （ANSI 浅色档 Theme.cpp：%1）").arg(lightRow));
        /*
         * 这一条只量到**引擎换算**那一层，没量到像素 —— 试过，量不出来，记在这儿免得
         * 下次再烧一轮：往格子里喂一段 33 号色的 W（得先关掉 shell，否则 PSReadLine
         * 一次重绘就把 W 抹了，实测 inGrid 从 1 变 0），格子里确实有；但 grabToImage
         * 抓回来的图 379756 个像素里只有 174 个像正文色 —— 那块 item 的画布是在
         * updatePaintNode 里建的，自检窗口没在屏幕上露出就一帧都不渲，抓到的是空图。
         * 桌面抓图在这条套件里也一直报"窗口不可见或被盖住"。
         *
         * 所以"白底上那档黄有没有变深"这一眼仍归人验。像素那一层并非没证据：他报来的
         * 截图里提示符路径就是**渲染出来的** #e0e000 —— 那正好说明"表 → 格子换算 →
         * paint"这条链是通的，改表就会改到屏幕上。
         */
    }

    if (theme && themeWas)
        theme->setLight(true);   /* 还原用户那一档，别把设置带跑 */

    /* 把还挂在队列里的 600ms 抓屏复查排空，否则退出太快、日志里没有那几行 */
    for (int i = 0; i < 120; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
}

}  // namespace

int SelfTest::runTerminal(QObject *qmlRoot, EditorController *cmd)
{
    tout(QStringLiteral("== 终端自检 =="));
    runEmulatorChecks();
    runRealShellChecks();
    if (qmlRoot)
        runRenderChecks(qmlRoot, cmd);
    else
        tout(QStringLiteral("\n（没传 qmlRoot，界面与真实渲染那一节跳过）"));

    std::fputs("\n", stdout);
    tout(QStringLiteral("终端自检：通过 %1 项，失败 %2 项").arg(gTermPassed).arg(gTermFailed));
    return gTermFailed;
}

int SelfTest::terminalPassed() { return gTermPassed; }
int SelfTest::terminalFailed() { return gTermFailed; }

bool SelfTest::terminalTestEnabled(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--terminal-test") == 0)
            return true;
    }
    return false;
}
