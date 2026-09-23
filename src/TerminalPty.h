#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <mutex>

class QTimer;

/*
 * 一个 Windows 伪控制台（ConPTY）+ 跑在里面的 shell 进程。
 *
 * 这是底部终端面板最底下那一层：上面（src/TerminalEngine.cpp）只管"字符网格"，
 * 这一层只管"字节进出"。
 *
 * 为什么用 ConPTY 而不是 QProcess 重定向三个管道：
 *   QProcess 给子进程的是**管道**，不是终端。管道下 shell 的行为和真终端完全不
 *   一样 —— PowerShell 不发提示符、不响应 Tab 补全和方向键（那要靠 ReadLine 自己
 *   读控制台输入）、程序检测不到 tty 就把彩色输出关掉、vim / less 这类全屏程序
 *   直接拒绝运行。ConPTY 是微软给的"把控制台缓冲区变成字节流"的正解：子进程以
 *   为自己在真控制台里跑，于是它把**渲染后的屏幕**（含光标移动、清屏、颜色）
 *   写进管道，交给我们上面那层去仿真。VS Code（node-pty）、Windows Terminal、
 *   Qt Creator 的内置终端走的都是这一条。
 *
 * 数据流：
 *   shell 写它的"屏幕" -> hOutputWrite ─ 管道 ─> hOutputRead（本类读线程 Peek 出来）
 *        -> m_pending 缓冲 -> 16ms 的泵定时器 -> output() 信号 -> 上层喂 libvterm
 *   键盘/粘贴/查询应答 <- hInputWrite（本类 write()）<─ 伪控制台 <─ hInputRead
 *
 * 为什么中间要放一个缓冲 + 定时器，而不是读线程直接 emit：
 *   读线程每读到一块就跨线程发一个带 QByteArray 的队列信号，一次 `git log` 刷屏
 *   能甩出几千个事件，把主线程事件队列灌爆（界面卡住、内存尖峰）。改成"读线程只
 *   往锁里的缓冲区追加、主线程每 16ms 抽干一次"之后，一帧最多一次仿真 + 一次重绘，
 *   代价是输入回显最多多等 16ms —— 一帧而已，看不出来。
 */
class TerminalPty : public QObject
{
    Q_OBJECT

public:
    explicit TerminalPty(QObject *parent = nullptr);
    ~TerminalPty() override;

    /*
     * 起一个 shell。program / args 不填时用系统 PowerShell。
     * workingDir 空则用当前用户主目录（不是程序目录 —— 谁也不想 cd 一下把安装
     * 目录写脏）。成功返回 true；失败返回 false，error 带上原因（界面直接显示）。
     */
    bool start(const QString &program, const QStringList &args,
               const QString &workingDir, int cols, int rows, QString *error);

    /* 把字节写给 shell（键盘、粘贴、以及对 shell 查询的应答） */
    void write(const QByteArray &bytes);

    /*
     * 改行列数。伪控制台收到新尺寸会重排它的缓冲区，并把重排后的屏幕重新打一遍
     * —— 所以调用方**不需要**自己补发内容。
     */
    void resize(int cols, int rows);

    /* 关掉：结束进程、拆掉伪控制台、收掉读线程。可重复调用。 */
    void shutdown();

    bool running() const { return m_process != nullptr; }

    /* 实际使用的 shell 可执行文件全路径（start 之后有效），界面拿它做标签名 */
    QString program() const { return m_program; }

signals:
    /* 一批从 shell 那边读到的字节（已按帧攒过，一次调用一块） */
    void output(const QByteArray &bytes);

    /* shell 自己退出了（用户打了 exit）；退出码原样带上 */
    void exited(int exitCode);

private:
    void pump();          // 主线程：抽干缓冲区、看进程还在不在
    void readerLoop();    // 读线程：Peek + ReadFile 到 m_pending

    QString m_program;
    /*
     * 这些全是 Win32 句柄（HANDLE / HPCON）。头文件里**不** include windows.h：
     * 那玩意会带进一堆宏（min/max/TRANSPARENT…），污染所有 include 它的 .cpp，
     * 而 Qt 的翻译单元对这些宏很敏感。用 void* 存，用的时候在 .cpp 里转回去。
     */
    void *m_pty = nullptr;        // HPCON
    void *m_inputWrite = nullptr; // 我们往这儿写 -> 伪控制台
    void *m_outputRead = nullptr; // 我们从这儿读 <- 伪控制台
    void *m_process = nullptr;    // shell 进程 HANDLE
    void *m_thread = nullptr;     // 读线程的线程 id（std::thread 的 native_handle）

    std::mutex m_mutex;
    QByteArray m_pending;
    std::atomic<bool> m_stopping { false };
    std::atomic<bool> m_readerDone { false };
    QTimer *m_pumpTimer = nullptr;
    bool m_exitedEmitted = false;
    /* 进程已退、但还留着等尾巴字节的那几拍（见 pump() 里那段说明） */
    int m_exitGraceTicks = 0;
};
