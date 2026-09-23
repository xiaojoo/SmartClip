/*
 * 必须在**任何**头文件之前定义 NOMINMAX：不然 windef.h 会带进 max / min 两个宏，
 * 而 Qt 的某些头自己就 include 了 windows.h —— 定义晚了照样被污染，
 * 本文件里 std::max(20, cols) 那种调用会在一个和现场八竿子打不着的行号上
 * 报":: 右边的非法标记"。
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN

#include "TerminalPty.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTimer>

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <thread>

/*
 * ConPTY 那三个函数 + 那个属性号，这里**自己声明、运行时 GetProcAddress 取**。
 *
 * 为什么不直接 include 了用：它们的声明被 SDK 圈在 `NTDDI_VERSION >= NTDDI_WIN10_RS5`
 * 里，而 Qt 给各翻译单元摆的是 `_WIN32_WINNT=0x0A00`（对应最老的 Win10 1507），
 * 于是 wincon.h 里根本没有 CreatePseudoConsole 这一行，硬用就得去改全工程的
 * NTDDI 定义 —— 那会影响所有别的 SDK 头可见的接口，为一个功能动它不值。
 * 动态取还有一个好处：拿不到就能明确告诉用户"系统太旧"，而不是打不开一个黑框。
 *
 * 三个函数的最低系统：Windows 10 1809（RS5）/ Windows Server 1709。
 */
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

typedef void *SmartClipPcon;   // HPCON
typedef HRESULT(WINAPI *CreatePseudoConsoleFn)(COORD, HANDLE, HANDLE, DWORD, SmartClipPcon *);
typedef void(WINAPI *ClosePseudoConsoleFn)(SmartClipPcon);
typedef HRESULT(WINAPI *ResizePseudoConsoleFn)(SmartClipPcon, COORD);

namespace {

struct ConPtys
{
    CreatePseudoConsoleFn create = nullptr;
    ClosePseudoConsoleFn close = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
};

ConPtys conpty()
{
    static const ConPtys api = [] {
        ConPtys a;
        const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (!k32)
            return a;
        a.create = reinterpret_cast<CreatePseudoConsoleFn>(
            GetProcAddress(k32, "CreatePseudoConsole"));
        a.close = reinterpret_cast<ClosePseudoConsoleFn>(
            GetProcAddress(k32, "ClosePseudoConsole"));
        a.resize = reinterpret_cast<ResizePseudoConsoleFn>(
            GetProcAddress(k32, "ResizePseudoConsole"));
        return a;
    }();
    return api;
}

void closeHandle(void *&h)
{
    if (h) {
        ::CloseHandle(static_cast<HANDLE>(h));
        h = nullptr;
    }
}

/*
 * Windows 命令行的引法（和 MSVC 的解析规则配对）：
 * 只有出现空格 / tab / 引号时才加引号；串里每个反斜杠**紧邻**引号或结尾时要翻倍
 * —— 因为命令行到 argv 的那一步会再把 \" 解一遍。路径里带空格是常态
 * （比如装在 `C:\Program Files\`），不引就断成两个参数。
 */
QString quoteArg(const QString &raw)
{
    if (!raw.contains(QLatin1Char(' ')) && !raw.contains(QLatin1Char('\t'))
        && !raw.contains(QLatin1Char('"')) && !raw.isEmpty())
        return raw;

    QString out(1, QLatin1Char('"'));
    int backslashes = 0;
    for (const QChar ch : raw) {
        if (ch == QLatin1Char('\\')) {
            ++backslashes;
            out.append(ch);
            continue;
        }
        if (ch == QLatin1Char('"'))
            out.append(QString(backslashes + 1, QLatin1Char('\\'))).append(ch);
        else
            out.append(ch);
        backslashes = 0;
    }
    if (backslashes > 0)
        out.append(QString(backslashes, QLatin1Char('\\')));
    return out.append(QLatin1Char('"'));
}

/*
 * 给子进程带上 TERM / COLORTERM。
 *
 * 为什么要带：ConPTY 只负责把控制台渲染成字节流，**不**告诉对面"我是什么终端"。
 * 不设 TERM 的话，vim 按最老的 vt52 画（没颜色、不认鼠标）。
 *
 * 为什么是"临时塞进本进程环境、传 NULL"而不是自己拼一份环境块传给
 * lpEnvironment（这才是直觉上更干净的做法）：**实测不行**。
 * 哪怕把 GetEnvironmentStringsW() 拿到的那块**原样抄一遍**传回去，一个字都不加，
 * PowerShell 5.1 也起不来，屏幕上只剩一句
 *     Windows PowerShell 内部错误。加载托管的 Windows PowerShell 失败，返回错误 8009001d
 * （NTE_PROVIDER_DLL_FAIL）。同一份代码只把 lpEnvironment 换回 NULL 就正常。
 * 探针在 build/conpty-probe/，A/B/C/D/E/H 六拍量出来的结论。
 *
 * 所以这里设完立刻起、起完立刻撤 —— 本进程自己的环境不留痕，
 * 免得资源脚本（tqdm / rich 那些会读 TERM 的）跟着变行为。
 */
void setTermEnv()
{
    _wputenv(L"TERM=xterm-256color");
    _wputenv(L"COLORTERM=truecolor");
}

void unsetTermEnv()
{
    _wputenv(L"TERM=");
    _wputenv(L"COLORTERM=");
}

QString defaultShell()
{
    const QString root = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"));
    const QString ps = root + QStringLiteral(
                                  "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    return QFileInfo::exists(ps) ? ps : QStringLiteral("powershell.exe");
}

}  // namespace

TerminalPty::TerminalPty(QObject *parent) : QObject(parent) {}

TerminalPty::~TerminalPty()
{
    shutdown();
}

bool TerminalPty::start(const QString &program, const QStringList &args,
                        const QString &workingDir, int cols, int rows, QString *error)
{
    if (m_process) {
        if (error)
            *error = tr("这个终端已经在跑了");
        return false;
    }

    const ConPtys api = conpty();
    if (!api.create) {
        if (error)
            *error = tr("这台机器的 Windows 没有伪控制台接口（ConPTY 要 Windows 10 "
                        "1809 及以上），终端面板用不了");
        return false;
    }

    m_program = program.trimmed().isEmpty() ? defaultShell() : program.trimmed();
    QStringList fullArgs = args;
    /* -NoLogo：去掉那两行版权横幅。PowerShell 和 cmd 都认（cmd 认 /nologo? 不认，
       所以只在 powershell 上加）—— 用户自己填了参数就尊重他的，不硬塞。 */
    if (fullArgs.isEmpty() && m_program.endsWith(QStringLiteral("powershell.exe"),
                                                 Qt::CaseInsensitive))
        fullArgs << QStringLiteral("-NoLogo");

    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE inputRead = nullptr, inputWrite = nullptr;
    HANDLE outputRead = nullptr, outputWrite = nullptr;
    if (!CreatePipe(&inputRead, &inputWrite, &sa, 64 * 1024)
        || !CreatePipe(&outputRead, &outputWrite, &sa, 64 * 1024)) {
        if (error)
            *error = tr("建管道失败（错误 %1）").arg(GetLastError());
        if (inputRead) ::CloseHandle(inputRead);
        if (inputWrite) ::CloseHandle(inputWrite);
        if (outputRead) ::CloseHandle(outputRead);
        if (outputWrite) ::CloseHandle(outputWrite);
        return false;
    }

    COORD size;
    size.X = static_cast<SHORT>(std::max(20, cols));
    size.Y = static_cast<SHORT>(std::max(4, rows));

    SmartClipPcon pty = nullptr;
    /* 伪控制台拿的是"它自己那侧"的两个头：读入、写出。 */
    if (const HRESULT hr = api.create(size, inputRead, outputWrite, 0, &pty); FAILED(hr)) {
        if (error)
            *error = tr("伪控制台起不来（0x%1）").arg(quint32(hr), 8, 16, QLatin1Char('0'));
        ::CloseHandle(inputRead);
        ::CloseHandle(inputWrite);
        ::CloseHandle(outputRead);
        ::CloseHandle(outputWrite);
        return false;
    }

    QString cmdline = quoteArg(m_program);
    for (const QString &a : fullArgs)
        cmdline += QLatin1Char(' ') + quoteArg(a);

    LPPROC_THREAD_ATTRIBUTE_LIST attrList = nullptr;
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    attrList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(malloc(attrSize));
    bool attrOk = attrList
                  && InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)
                  && UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                               pty, sizeof(pty), nullptr, nullptr);
    if (!attrOk) {
        if (error)
            *error = tr("启动信息里挂不上伪控制台（错误 %1）").arg(GetLastError());
        if (attrList) {
            DeleteProcThreadAttributeList(attrList);
            free(attrList);
        }
        api.close(pty);
        ::CloseHandle(inputRead);
        ::CloseHandle(inputWrite);
        ::CloseHandle(outputRead);
        ::CloseHandle(outputWrite);
        return false;
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof(STARTUPINFOW);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inputRead;
    startup.hStdOutput = outputWrite;
    startup.hStdError = outputWrite;
    /*
     * 这三个头**不能省**。微软那份 ConPTY 示例只挂伪控制台属性、不填 std 头，
     * 照着抄的结果是：PowerShell 起来之后只吐两个 DECSET 就再也不出声（探针 A / C 拍）。
     * 填上才正常。伪控制台接管的是"控制台会话"，std 句柄仍然要在这里给对。
     */

    STARTUPINFOEXW startupEx {};
    startupEx.StartupInfo = startup;
    startupEx.lpAttributeList = attrList;
    /*
     * 挂了 EXTENDED_STARTUPINFO_PRESENT 之后，CreateProcess 认的是这个 cb，
     * 而且**必须**是 STARTUPINFOEXW 的大小 —— 留着上面那份 STARTUPINFOW 的 68
     * 直接就是 ERROR_INVALID_PARAMETER（错误 87），报都报不到 shell 头上。
     */
    startupEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);

    PROCESS_INFORMATION pi {};
    const std::wstring programW = m_program.toStdWString();
    std::wstring cmdW = cmdline.toStdWString();
    const std::wstring dirW = QDir::toNativeSeparators(
        workingDir.isEmpty() ? QDir::homePath() : workingDir).toStdWString();

    setTermEnv();
    const BOOL ok = CreateProcessW(
        programW.c_str(), cmdW.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr, dirW.empty() ? nullptr : dirW.c_str(),
        &startupEx.StartupInfo, &pi);
    unsetTermEnv();

    DeleteProcThreadAttributeList(attrList);
    free(attrList);

    if (!ok) {
        const DWORD code = GetLastError();
        api.close(pty);
        ::CloseHandle(inputRead);
        ::CloseHandle(inputWrite);
        ::CloseHandle(outputRead);
        ::CloseHandle(outputWrite);
        if (error)
            *error = tr("启动 %1 失败（错误 %2）").arg(QFileInfo(m_program).fileName())
                         .arg(code);
        return false;
    }

    CloseHandle(pi.hThread);
    m_process = pi.hProcess;
    m_pty = pty;
    m_inputWrite = inputWrite;
    m_outputRead = outputRead;

    /*
     * 这两个头必须在我们这边关掉。
     *   inputRead：留着的话，我们退出前 shell 永远读不到 EOF；
     *   outputWrite：留着的话，shell 退出了管道也不报断，读线程会一直空转等不到结束。
     */
    ::CloseHandle(inputRead);
    ::CloseHandle(outputWrite);

    m_stopping.store(false);
    m_readerDone.store(false);
    m_exitedEmitted = false;
    m_exitGraceTicks = 0;
    m_thread = new std::thread([this] { readerLoop(); });

    if (!m_pumpTimer) {
        m_pumpTimer = new QTimer(this);
        m_pumpTimer->setInterval(16);   // 约一帧；见头文件里"为什么要攒"那段
        connect(m_pumpTimer, &QTimer::timeout, this, &TerminalPty::pump);
    }
    m_pumpTimer->start();
    return true;
}

void TerminalPty::write(const QByteArray &bytes)
{
    if (!m_inputWrite || bytes.isEmpty())
        return;
    const char *p = bytes.constData();
    int left = bytes.size();
    while (left > 0) {
        DWORD got = 0;
        if (!WriteFile(static_cast<HANDLE>(m_inputWrite), p, DWORD(left), &got, nullptr) || got == 0)
            return;   // 管道断了（shell 已退）：丢掉，由 pump 那侧报退出
        p += got;
        left -= int(got);
    }
}

void TerminalPty::resize(int cols, int rows)
{
    if (!m_pty)
        return;
    COORD size;
    size.X = static_cast<SHORT>(std::max(20, cols));
    size.Y = static_cast<SHORT>(std::max(4, rows));
    /*
     * 不能在读线程上调这个：微软文档写明 ResizePseudoConsole 会阻塞等对面把缓冲区
     * 排空，和读管道撞在同一个线程上就是死锁。本类的读线程是另起的一条，
     * 这里跑在 GUI 线程，所以安全。
     */
    conpty().resize(static_cast<SmartClipPcon>(m_pty), size);
}

void TerminalPty::shutdown()
{
    if (m_pumpTimer)
        m_pumpTimer->stop();

    m_stopping.store(true);
    if (m_thread) {
        auto *t = static_cast<std::thread *>(m_thread);
        if (t->joinable())
            t->join();
        delete t;
        m_thread = nullptr;
    }

    closeHandle(m_outputRead);
    closeHandle(m_inputWrite);

    if (m_process) {
        /*
         * 先杀进程再拆伪控制台。反过来的话 ClosePseudoConsole 会等 shell 自己结束，
         * 而一个卡在 `Read-Host` 上的 PowerShell 能把它一直等到用户被烦死。
         */
        TerminateProcess(static_cast<HANDLE>(m_process), 0);
        WaitForSingleObject(static_cast<HANDLE>(m_process), 2000);
        closeHandle(m_process);
    }

    if (m_pty) {
        conpty().close(static_cast<SmartClipPcon>(m_pty));
        m_pty = nullptr;
    }
}

void TerminalPty::readerLoop()
{
    auto *h = static_cast<HANDLE>(m_outputRead);
    QByteArray buf;
    buf.resize(64 * 1024);

    while (!m_stopping.load()) {
        DWORD avail = 0;
        /*
         * PeekNamedPipe 失败 = 管道对面那个写头（伪控制台）已经关了 = shell 退了。
         * 这里直接收线程，不 sleep：进程都没了，等下去没有意义。
         */
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr))
            break;
        if (avail == 0) {
            /*
             * 没数据才睡。有数据就贴着读、不睡 —— 这样刷屏时读线程是全速搬，
             * 空闲时才是每秒 250 次的轻量轮询。
             */
            Sleep(4);
            continue;
        }
        DWORD want = std::min<DWORD>(avail, DWORD(buf.size()));
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), want, &got, nullptr) || got == 0)
            break;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.append(buf.constData(), int(got));
    }
    m_readerDone.store(true);
}

void TerminalPty::pump()
{
    QByteArray chunk;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        chunk.swap(m_pending);
    }
    if (!chunk.isEmpty())
        emit output(chunk);

    if (!m_process || m_exitedEmitted)
        return;
    if (WaitForSingleObject(static_cast<HANDLE>(m_process), 0) != WAIT_OBJECT_0)
        return;

    /* 先把手上已经搬到的字节交出去（这一拍看到的都算数） */
    QByteArray tail;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        tail.swap(m_pending);
    }
    if (!tail.isEmpty())
        emit output(tail);

    /*
     * 再等最多 3 拍（约 48ms）让读线程把管道里剩下的尾巴搬完 —— 但**不能等它退出**。
     *
     * 一开始这里写的是"等读线程自己收（它会在 PeekNamedPipe 报错时退出）"，
     * 结果 `exit` 之后 exited 永远不发：伪控制台的写端是 ConPTY 内部持有的，
     * shell 退了它也不关（HPCON 还活着），于是 PeekNamedPipe 一直成功、
     * 读线程一直空转。改成"进程没了 + 尾巴抽干 + 最多三拍"，不依赖那个 EOF。
     */
    if (!m_readerDone.load() && m_exitGraceTicks < 3) {
        ++m_exitGraceTicks;
        return;
    }

    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(m_process), &code);
    m_exitedEmitted = true;
    m_stopping.store(true);      // 让读线程下一拍自己退，shutdown 时 join
    m_pumpTimer->stop();
    /*
     * 进程句柄在这里就关掉：running() 读的就是它。留着会让界面以为 shell 还在，
     * 而 shutdown() 再去 TerminateProcess 一个已经退出的句柄也没意义。
     */
    ::CloseHandle(static_cast<HANDLE>(m_process));
    m_process = nullptr;
    emit exited(int(code));
}
