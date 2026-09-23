/* NOMINMAX 必须在任何头之前（Qt 的头自己会带 windows.h，晚了就防不住 min/max 宏） */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "SelfTest.h"

#include "EditorController.h"

#include "TerminalEngine.h"
#include "TerminalView.h"

#include <QCoreApplication>
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

    /* Ctrl+C：跑一个 30 秒的命令，按打断，提示符要在几秒内回来 */
    run(QStringLiteral("Start-Sleep 30"));
    QThread::msleep(400);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    e->sendBytes("\x03");
    const bool interrupted = waitUntil(
        [&e] {
            const QString last = e->lineText(e->rows() - 1);
            return last.contains(QLatin1String("PS ")) && last.contains(QLatin1Char('>'));
        },
        8000);
    tcheck(interrupted, QStringLiteral("Ctrl+C 打得断 Start-Sleep（提示符回来了）"));

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
        tcheck(canvasRunPeak < 100 && worstScreen < 100,
               QStringLiteral("面板吃下整行之后的头几帧：没有黑带（画布和屏幕两层都量）"),
               QStringLiteral("视图 %1x%2 格子 %3 列，画布最长黑段=%4，屏幕黑段峰值=%5；"
                              "顺带：单帧最多铺黑底=%6 块、画布黑采样峰值=%7（宽字符尾巴，见上）")
                   .arg(view->width()).arg(view->height()).arg(view->columns())
                   .arg(canvasRunPeak).arg(worstScreen).arg(blackRectPeak)
                   .arg(canvasBlackPeak).arg(series.join(" || ")));
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
        tcheck(fed && totalAfter == totalBefore,
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
         * 位置条的圆角（用户报的第 1 条）。量的是**这一条自己渲染出来的像素**，
         * 不是 Rectangle.radius 那个属性值 —— 属性写了不代表画出来圆了。
         *
         * 为什么不用桌面抓屏对号：第一次那么量，坐标算到 (2633,809) 打出来是 #ffffff，
         * 抓下来一看才发现那块区域是剪贴板缩略图网格（一张背景本来就黑的图），尺子读的不是我们。
         * grabToImage 只渲染这一个 item：半径 4 = 半个短边，角尖落在圆外 = 清出来的黑底，
         * 往里 4px 才是轨道色；半径要是 0，角尖就是实打实的轨道色 —— 分得出来。
         * （屏幕上那个角尖露出来的是卡片底色 #1e1f22，这里量的是"item 自己圆没圆"。）
         */
        if (auto *rb = qobject_cast<QQuickItem *>(track);
            rb && view->scrollUp() == 0 && view->historyRows() > 0) {
            auto res = rb->grabToImage();
            bool ready = false;
            QObject::connect(res.data(), &QQuickItemGrabResult::ready, [&] { ready = true; });
            waitUntil([&] { return ready; }, 3000);
            const QImage t = res->image();
            const QColor trackColor(QStringLiteral("#2a2d2e"));
            auto closeTo = [&](QRgb p) {
                return qAlpha(p) > 200 && qAbs(qRed(p) - trackColor.red()) <= 6
                       && qAbs(qGreen(p) - trackColor.green()) <= 6
                       && qAbs(qBlue(p) - trackColor.blue()) <= 6;
            };
            const int dpr = qRound(view->window()->devicePixelRatio());
            const QPoint corner(0, 0);
            const QPoint inner(4 * dpr, 4 * dpr);
            const bool has = t.width() > inner.x() && t.height() > inner.y();
            tcheck(has && !closeTo(t.pixel(corner)) && closeTo(t.pixel(inner)),
                   QStringLiteral("位置条那个角真的是圆的（角尖落在圆外、往里 4px 才是轨道色）"),
                   QStringLiteral("轨道图 %1x%2 角尖=%3 往里 4px=%4 轨道色=%5")
                       .arg(t.width()).arg(t.height())
                       .arg(has ? QString::fromLatin1(QColor(t.pixel(corner)).name(QColor::HexArgb).toLatin1())
                                : QStringLiteral("图太小"))
                       .arg(has ? QString::fromLatin1(QColor(t.pixel(inner)).name(QColor::HexArgb).toLatin1())
                                : QStringLiteral("-"))
                       .arg(trackColor.name()));
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
