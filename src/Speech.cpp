#include "Speech.h"

#include <QThread>
#include <QWaitCondition>
#include <QMutex>
#include <QMutexLocker>
#include <QDebug>

#include <atomic>

/*
 * 朗读（Windows SAPI5）。头文件里写了"为什么是它、不是 QtTextToSpeech"，
 * 这里写"为什么长成这个形状"。
 *
 * ------------------------------------------------------------------
 * 1) 为什么引擎待在自己的线程里
 *
 * ISpVoice 是从 ISpEventSource 派下来的（带连接点，见 SDK 的 sapi.h），
 * 这类接口按 STA 处理最稳：**创建它的那个线程才是能安全调它的线程**。
 * 所以引擎在自己的 QThread 里建；那个线程同时是 STA（CoInitializeEx 带
 * COINIT_APARTMENTTHREADED）—— 界面线程在 Qt 里也常是 STA，两边不抢。
 *
 * 界面线程只往那个线程投命令（见 post()），命令由那个线程的消息泵取出来执行。
 * 消息泵在"念"的时候会被拆成一段一段轮询，所以它必须能**边念边收命令** ——
 * 这就是 stop / pause 能立刻生效的原因。
 *
 * 消息泵：
 *   * 消息泵不是必须的（不说也能出声），但它让这件事成立：**念到一半掐掉**
 *     和**念完通知界面**。前者靠"一段念完就回头看一眼消息泵"，后者靠
 *     ISpVoice::WaitUntilDone(0) 轮询 —— 都不需要额外的回调窗口。
 *   * 只认自己那个窗口的消息（PostMessage 投过来的），不用
 *     PostThreadMessage 那种要靠线程 id 的写法：窗口在 worker 线程里建，
 *     句柄由 worker 自己拿着，界面线程只负责 Post。
 *
 * ------------------------------------------------------------------
 * 2) 为什么分块念
 *
 * 整段扔给 SAPI 也能念，但那样"停"要等 SAPI 自己回头 —— 段落一长就是几秒
 * 的延迟，用户会觉得按钮失灵。这里按句号 / 换行切成小段（见 splitChunks），
 * 一段念完就回一次消息泵：停止键最多差一句，手感就是"立刻"。
 * 每段还会按句读切一次（PURGEBEFORE + ASYNC），SAPI 自己的队列里只留一小段。
 *
 * ------------------------------------------------------------------
 * 3) 失败时的态度
 *
 * 系统里一个音色都没有（精简版 Windows / 没装语音包）：available = false，
 * 界面把按钮画灰。出声出错：error 里放一句人话，界面照旧能用 —— 和
 * PinOcr 找不到 OCR 语言包时是同一个态度（见 src/PinOcr.h 的文件头）。
 *
 * 这个文件是**唯一** include <sapi.h> 的地方：它会牵进来 rpc.h / ole2.h /
 * mmsystem.h 和一堆 Windows 宏（min/max 之类），扩散出去迟早撞上别的源文件
 * —— 和 PinOcr.cpp 只在自己家里 include WinRT 头是同一个理由。
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <sapi.h>

/*
 * 注意：这里**不** include SDK 的 sphelper.h。
 *
 * 那个头里有 SpEnumTokens / SpGetDescription 这些顺手的函数，但它开头就
 * `#include <atlbase.h>` —— 为了两个工具函数把 ATL 拉进这个工程不划算
 * （而且 ATL 的 CComPtr 之类会和这里的东西抢名字）。所以：
 *   * 数音色 -> ISpObjectTokenCategory::EnumTokens（纯 COM，见 setupEngine）
 *   * 取名字 -> token 的 "Name" 属性（见 tokenName）
 * 两件事都是几行的事。
 */

namespace {

/* worker 线程用的两个"叫醒"消息（参数都走 Work 里那个队列，不用消息参数） */
constexpr UINT kMsgCommand   = WM_APP + 0x51;
constexpr UINT kMsgQuit      = WM_APP + 0x52;

/* 一次 GetStatus 轮询等多久（也是"停止键"最长的反应时间） */
constexpr DWORD kPollMs = 100;

/*
 * 给 COM 要单元的活（RaiseException 之类）包一层返回值 ——
 * 这些是系统调用，不是我们的对象，析构不会抛。
 */
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    /* 给 CoCreateInstance 之类当 out 参数用：ComPtr<ISpVoice> v; &v */
    T **put() { return &m_p; }
    T *operator->() const { return m_p; }
    T *get() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }

    void reset() {
        if (m_p) {
            m_p->Release();
            m_p = nullptr;
        }
    }

private:
    T *m_p = nullptr;
};

/* 找一个音色的名字（"Microsoft Huihui Desktop"）。
 *
 * 优先读 token 的 "Name" 属性（SAPI 自己写进去的显示名）；没有就退回 token id
 * 的最后一段（形如 "HKEY_LOCAL_MACHINE\\...\\Tokens\\TTS_MS_ZH-CN_HUIHUI_11.0"
 * -> "TTS_MS_ZH-CN_HUIHUI_11.0"）。**这种退路是有的**：部分第三方语音包不写
 * Name，只写 id，那时候清单上不能是一条空白。
 */
QString tokenName(ISpObjectToken *token) {
    if (!token)
        return {};
    LPWSTR name = nullptr;
    if (SUCCEEDED(token->GetStringValue(L"Name", &name)) && name) {
        const QString out = QString::fromWCharArray(name).trimmed();
        CoTaskMemFree(name);
        if (!out.isEmpty())
            return out;
    } else if (name) {
        CoTaskMemFree(name);
    }

    LPWSTR id = nullptr;
    if (FAILED(token->GetId(&id)) || !id)
        return {};
    const QString full = QString::fromWCharArray(id);
    CoTaskMemFree(id);
    const int cut = full.lastIndexOf(QLatin1Char('\\'));
    const QString tail = cut >= 0 ? full.mid(cut + 1) : full;
    return tail.trimmed();
}

QString hresultText(HRESULT hr) {
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

/*
 * 这台机器上一个音色都没有时的那句人话。
 *
 * "没装"和"挑不出这个语言的"都用它 —— 后者是"中文音色没装，先用系统默认的
 * 那个念"，对用户来说要做的事是一样的（去系统里装一个中文语音包）。
 */
QString noVoiceText() {
    return QStringLiteral("系统里没有这个语言的语音包（Windows 设置 → 时间和语言 → "
                          "语音 → 管理语音），先用默认音色念");
}

/* 一段一段地切：先按句末标点 / 换行切，太长的再按逗号切，还长的硬切 */
QStringList splitChunks(const QString &text, int maxLen = 90) {
    QStringList rough;
    QString current;
    for (const QChar ch : text) {
        current.append(ch);
        if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r')
            || ch == QChar(0x3002)   /* 。 */
            || ch == QChar(0xFF01)   /* ！ */
            || ch == QChar(0xFF1F)   /* ？ */
            || ch == QLatin1Char('.') || ch == QLatin1Char('!')
            || ch == QLatin1Char('?') || ch == QLatin1Char(';')
            || ch == QChar(0xFF1B))  /* ； */
        {
            rough.append(current);
            current.clear();
        }
    }
    if (!current.isEmpty())
        rough.append(current);

    QStringList out;
    for (const QString &piece : rough) {
        QString rest = piece;
        while (rest.size() > maxLen) {
            int cut = -1;
            for (int i = maxLen - 1; i > maxLen / 2; --i) {
                const QChar ch = rest.at(i);
                if (ch == QChar(0xFF0C) || ch == QLatin1Char(',') || ch == QLatin1Char(' ')) {
                    cut = i + 1;
                    break;
                }
            }
            if (cut <= 0)
                cut = maxLen;
            out.append(rest.left(cut));
            rest = rest.mid(cut);
        }
        if (!rest.isEmpty())
            out.append(rest);
    }
    return out;
}

/* 语言名 / 标签 -> 几个可能的 LCID（十六进制串）—— 认不出就返回空 */
QStringList lcidHints(const QString &lang) {
    const QString l = lang.trimmed().toLower();
    if (l.isEmpty())
        return {};
    if (l.startsWith(QStringLiteral("zh")) || l.contains(QStringLiteral("中文"))
        || l.contains(QStringLiteral("汉语")) || l.contains(QStringLiteral("chinese")))
    {
        /* 804 = zh-CN，404 = zh-TW，c04 = zh-HK */
        return {QStringLiteral("804"), QStringLiteral("404"), QStringLiteral("c04")};
    }
    if (l.startsWith(QStringLiteral("en")) || l.contains(QStringLiteral("英文"))
        || l.contains(QStringLiteral("英语")) || l.contains(QStringLiteral("english")))
        return {QStringLiteral("409")};
    if (l.startsWith(QStringLiteral("ja")) || l.contains(QStringLiteral("日")))
        return {QStringLiteral("411")};
    if (l.startsWith(QStringLiteral("ko")) || l.contains(QStringLiteral("韩")))
        return {QStringLiteral("412")};
    if (l.startsWith(QStringLiteral("fr")) || l.contains(QStringLiteral("法")))
        return {QStringLiteral("40c")};
    if (l.startsWith(QStringLiteral("de")) || l.contains(QStringLiteral("德")))
        return {QStringLiteral("407")};
    if (l.startsWith(QStringLiteral("es")) || l.contains(QStringLiteral("西班牙")))
        return {QStringLiteral("40a")};
    if (l.startsWith(QStringLiteral("ru")) || l.contains(QStringLiteral("俄")))
        return {QStringLiteral("419")};
    if (l.startsWith(QStringLiteral("it")) || l.contains(QStringLiteral("意大利")))
        return {QStringLiteral("410")};
    if (l.startsWith(QStringLiteral("pt")) || l.contains(QStringLiteral("葡萄牙")))
        return {QStringLiteral("816")};
    if (l.startsWith(QStringLiteral("th")) || l.contains(QStringLiteral("泰")))
        return {QStringLiteral("41e")};
    if (l.startsWith(QStringLiteral("vi")) || l.contains(QStringLiteral("越南")))
        return {QStringLiteral("42a")};
    return {};
}

}  // namespace

/* ===========================================================================
 * Worker：引擎住的线程 + 那个消息泵
 * ======================================================================== */

class Speech::Worker final : public QThread {
public:
    enum class Kind { Speak, Stop, Pause, Resume };

    struct Work {
        Kind kind = Kind::Stop;
        QString text;
        QString lang;
    };

    Worker() = default;
    ~Worker() override { shutdown(); }

    /* 界面线程投一条命令（醒一下那个消息泵） */
    void post(const Work &work) {
        {
            QMutexLocker locker(&m_mutex);
            m_queue.append(work);
        }
        if (m_window)
            PostMessageW(m_window, kMsgCommand, 0, 0);
    }

    /* 关掉：先掐掉在念的，再让 run() 收摊（析构里调，幂等） */
    void shutdown() {
        if (!isRunning())
            return;
        {
            QMutexLocker locker(&m_mutex);
            m_queue.clear();
            m_quitting = true;
        }
        if (m_window)
            PostMessageW(m_window, kMsgQuit, 0, 0);
        if (!wait(3000)) {
            /* 真等不到（引擎卡住）才来硬的：退出前保证线程没了，不然 CoUninitialize 落在别人身上 */
            terminate();
            wait(1000);
        }
    }

    /* 引擎那边报回来的东西都从这里出去（见 Speech 的槽） */
    std::function<void(bool, const QStringList &)> onReady;
    std::function<void(int, const QString &)> onState;
    std::function<void()> onFinished;

protected:
    void run() override {
        /*
         * 单元要在**用 COM 之前**建：这个线程是我们自己 new 的，Qt 不管它。
         * 已经是别的单元（理论上不会）就不动它。
         */
        const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool comOwned = SUCCEEDED(hrInit);

        makeWindow();

        setupEngine();

        if (m_voice.get()) {
            qInfo().nospace() << "[Speech] 引擎就绪，音色 " << m_voiceInfo.size()
                              << " 个，默认 " << (m_defaultVoice.isEmpty() ? QStringLiteral("(无)")
                                                                          : m_defaultVoice);
        } else {
            qWarning().nospace() << "[Speech] 没建出朗读引擎"
                                 << (m_engineError.isEmpty() ? QString() : QStringLiteral("：") + m_engineError);
        }

        if (onReady)
            onReady(m_voice.get() != nullptr && !m_voiceInfo.isEmpty(), m_voiceInfo);

        for (;;) {
            MSG msg;
            const BOOL got = PeekMessageW(&msg, m_window, 0, 0, PM_REMOVE);
            if (got) {
                if (msg.message == kMsgQuit)
                    break;
                if (msg.message == kMsgCommand) {
                    if (!pumpCommands())
                        break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!processPending())
                break;
            ::Sleep(5);
        }

        /* 收尾：别让最后一句留在队列里念完（那会拖住退出） */
        if (m_voice.get()) {
            m_voice->Pause();          /* 先停住当前在念的那段 */
            m_voice->Speak(nullptr, SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
            qInfo() << "[Speech] worker 线程收摊";
            m_voice.reset();
        }
        if (m_window) {
            DestroyWindow(m_window);
            m_window = nullptr;
        }
        if (comOwned)
            CoUninitialize();
    }

private:
    /* 消息泵只用来"被叫醒"，消息本身不需要处理 */
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void makeWindow() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Worker::wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SmartClipSpeechWake";
        RegisterClassExW(&wc);   /* 重复注册无所谓：同进程里这个类名只有一份 */
        m_window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                   HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!m_window)
            qWarning() << "[Speech] 叫醒窗口没建出来，停止键可能要等一句念完";
    }

    /*
     * 建引擎 + 数音色 + 挑默认那个。
     *
     * 这里**不发声音**，只是把"这台机器有什么"摊平（音色描述 + 语言标签），
     * 界面要挑音色直接查这份清单，不用再走一趟 COM。
     *
     * 数音色走的是纯 COM（ISpObjectTokenCategory::EnumTokens），**不用**
     * SDK 里那个 SpEnumTokens()——那个在 sphelper.h 里，而那个头会
     * `#include <atlbase.h>`，为了几行枚举把 ATL 拉进这个工程不划算。
     */
    void setupEngine() {
        HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                      IID_ISpVoice, reinterpret_cast<void **>(m_voice.put()));
        if (FAILED(hr) || !m_voice) {
            m_engineError = QStringLiteral("建朗读引擎失败（CoCreateInstance %1）").arg(hresultText(hr));
            return;
        }

        ComPtr<ISpObjectTokenCategory> category;
        hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                              IID_ISpObjectTokenCategory,
                              reinterpret_cast<void **>(category.put()));
        if (FAILED(hr) || !category) {
            m_engineError = QStringLiteral("取不到语音类别（%1）").arg(hresultText(hr));
            return;
        }
        hr = category->SetId(SPCAT_VOICES, FALSE);
        if (FAILED(hr)) {
            m_engineError = QStringLiteral("打不开语音类别（SetId %1）").arg(hresultText(hr));
            return;
        }

        /* NULL / NULL = 不做属性过滤：音色全都数出来（语言的事我们自己挑） */
        ComPtr<IEnumSpObjectTokens> tokens;
        hr = category->EnumTokens(nullptr, nullptr, tokens.put());
        if (FAILED(hr) || !tokens) {
            m_engineError = QStringLiteral("数不出音色（EnumTokens %1）").arg(hresultText(hr));
            return;
        }

        for (;;) {
            ComPtr<ISpObjectToken> token;
            ULONG fetched = 0;
            if (tokens->Next(1, token.put(), &fetched) != S_OK || fetched == 0 || !token)
                break;

            /*
             * 音色名字（"Microsoft Huihui Desktop"）+ Language 属性（"804" 这种
             * LCID 串，有的语音包干脆不写）。两样都留给界面挑音色用。
             */
            LPWSTR lcid = nullptr;
            const QString name = tokenName(token.get());
            if (name.isEmpty()) {
                token.reset();
                continue;
            }
            if (FAILED(token->GetStringValue(L"Language", &lcid)))
                lcid = nullptr;

            VoiceInfo info;
            info.description = name;
            if (lcid) {
                info.lcid = QString::fromWCharArray(lcid).trimmed().toLower();
                CoTaskMemFree(lcid);
            }

            info.token = token.get();
            info.token->AddRef();          /* 清单自己拿着一份引用（token 析构后还要用） */
            m_voices.append(info);

            /*
             * 清单里第一个是 SAPI 认定的**默认音色**（SpEnumTokens 按优先级排的）：
             * 把它设进引擎，认不出语言的文本就归它念。
             */
            if (!m_defaultToken) {
                m_voice->SetVoice(info.token);
                m_defaultToken = info.token;
                m_defaultToken->AddRef();
                m_defaultVoice = info.description;
                m_defaultLcid = info.lcid;
            }
            m_voiceInfo.append(info.lcid.isEmpty()
                                    ? info.description
                                    : QStringLiteral("%1 (%2)").arg(info.description, info.lcid));
        }

        if (m_voices.isEmpty())
            m_engineError = QStringLiteral("系统里一个语音包都没有");
    }

    /* 语言标签 -> 音色（挑不出就退回默认那个） */
    ISpObjectToken *pickVoice(const QString &lang) {
        const QString l = lang.trimmed().toLower();
        if (!l.isEmpty()) {
            const QStringList hints = lcidHints(lang);
            for (const VoiceInfo &voice : m_voices) {
                if (!voice.lcid.isEmpty() && hints.contains(voice.lcid))
                    return voice.token;
            }
            /*
             * 标签这条路没认出来（有的语音包写的是别的形式）：拿音色描述里的
             * 词再试一次（"Microsoft Huihui Desktop" 这种名字里带语种）。
             */
            if (l.startsWith(QStringLiteral("zh")) || l.contains(QStringLiteral("中文"))) {
                for (const VoiceInfo &voice : m_voices) {
                    const QString d = voice.description.toLower();
                    if (d.contains(QStringLiteral("chinese")) || d.contains(QStringLiteral("huihui"))
                        || d.contains(QStringLiteral("kangkang")) || d.contains(QStringLiteral("yaoyao")))
                        return voice.token;
                }
            }
        }
        return m_defaultToken;
    }

    /* 把队列里的命令都做掉；返回 false = 该收摊了 */
    bool pumpCommands() {
        for (;;) {
            Work work;
            {
                QMutexLocker locker(&m_mutex);
                if (m_queue.isEmpty())
                    return !m_quitting;
                work = m_queue.takeFirst();
            }
            if (work.kind == Kind::Speak)
                startSpeaking(work.text, work.lang);
            else if (work.kind == Kind::Stop)
                stopSpeaking();
            else if (work.kind == Kind::Pause)
                pauseResume(true);
            else
                pauseResume(false);
        }
    }

    void startSpeaking(const QString &text, const QString &lang) {
        m_error.clear();
        m_speaking = false;
        m_paused = false;

        if (!m_voice || text.trimmed().isEmpty()) {
            if (!text.trimmed().isEmpty())
                qInfo() << "[Speech] 引擎没建起来，念不了";
            reportState();
            return;
        }
        ISpObjectToken *token = pickVoice(lang);
        const HRESULT hr = m_voice->SetVoice(token);
        if (FAILED(hr))
            qWarning().nospace() << "[Speech] SetVoice 失败 " << hresultText(hr);

        /* 语速稍微往快一点调（默认 0 对中文偏慢）；音量交给系统 */
        m_voice->SetRate(1);

        m_chunks = splitChunks(text);
        if (m_chunks.isEmpty())
            m_chunks.append(text);
        m_stopRequested = false;
        m_speaking = true;
        m_chunkIndex = 0;
        reportState();

        qInfo().nospace() << "[Speech] 开始念 " << m_chunks.size() << " 段，音色 "
                          << (token == m_defaultToken ? QStringLiteral("(默认)")
                                                      : QStringLiteral("(按语言挑的)"));

        /* 第一段现在就发出去，别等下一轮循环 */
        if (!speakNextChunk()) {
            m_speaking = false;
            reportState();
        }
    }

    void stopSpeaking() {
        m_stopRequested = true;
        m_chunks.clear();
        m_chunkIndex = 0;
        if (m_voice) {
            m_voice->Speak(nullptr, SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
            m_voice->Resume();
        }
        const bool was = m_speaking;
        m_speaking = false;
        m_paused = false;
        reportState();
        if (was)
            qInfo() << "[Speech] 停";
    }

    void pauseResume(bool wantPause) {
        if (!m_voice || !m_speaking)
            return;
        /*
         * 注意 SAPI 这个接口就长这样：Pause() / Resume() **各是一个无参方法**，
         * 不像别的接口那样 Pause(TRUE/FALSE)。所以这里要分两支调。
         */
        if (wantPause)
            m_voice->Pause();
        else
            m_voice->Resume();
        m_paused = wantPause;
        reportState();
    }

    /* 送下一段；返回 false = 没有下一段了 */
    bool speakNextChunk() {
        if (!m_voice)
            return false;
        if (m_stopRequested)
            return false;
        while (m_chunkIndex < m_chunks.size() && m_chunks.at(m_chunkIndex).trimmed().isEmpty())
            ++m_chunkIndex;
        if (m_chunkIndex >= m_chunks.size())
            return false;

        const QString chunk = m_chunks.at(m_chunkIndex);
        ++m_chunkIndex;
        const HRESULT hr = m_voice->Speak(reinterpret_cast<LPCWSTR>(chunk.utf16()),
                                          SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
        if (FAILED(hr)) {
            m_error = QStringLiteral("念不出来（Speak %1）").arg(hresultText(hr));
            m_speaking = false;
            reportState();
            return false;
        }
        return true;
    }

    /*
     * 每轮循环看一眼：还有没有在念、这一段念完了没有。
     *
     * 返回 false = 线程该收摊了。
     */
    bool processPending() {
        bool quitting = false;
        {
            QMutexLocker locker(&m_mutex);
            quitting = m_quitting;
        }
        if (quitting)
            return false;

        if (!m_voice || !m_speaking)
            return true;

        if (m_stopRequested) {
            m_speaking = false;
            reportState();
            return true;
        }

        /*
         * WaitUntilDone(0)：等 0 毫秒 = 只看一眼"念完了没"，不阻塞。
         * 阻塞版本会把消息泵卡住，"停止"就点不动了 —— 这就是这里必须轮询的原因。
         */
        const HRESULT hr = m_voice->WaitUntilDone(kPollMs);
        if (hr != S_OK)
            return true;   /* 还在念（或者暂停着） */

        /* 这一段念完了：还有下一段就接着发，没有就是整段念完了 */
        if (!speakNextChunk()) {
            m_speaking = false;
            m_paused = false;
            reportState();
            if (!m_stopRequested && m_error.isEmpty() && onFinished)
                onFinished();
        }
        return true;
    }

    void reportState() {
        if (!onState)
            return;
        /* 1 = 在念，2 = 暂停，0 = 没在念（Speech::stateFrom 认这三个） */
        const int state = m_speaking ? (m_paused ? 2 : 1) : 0;
        onState(state, m_error);
    }

    struct VoiceInfo {
        QString description;
        QString lcid;
        ISpObjectToken *token = nullptr;   /* 自己拿着引用 */
    };

    HWND m_window = nullptr;

    ComPtr<ISpVoice> m_voice;
    QList<VoiceInfo> m_voices;
    QStringList m_voiceInfo;

    ISpObjectToken *m_defaultToken = nullptr;
    QString m_defaultVoice;
    QString m_defaultLcid;
    QString m_engineError;

    QMutex m_mutex;
    QList<Work> m_queue;
    bool m_quitting = false;

    QStringList m_chunks;
    int m_chunkIndex = 0;
    std::atomic_bool m_stopRequested{false};
    bool m_speaking = false;
    bool m_paused = false;
    QString m_error;
};

/* ===========================================================================
 * Speech（QML 单例）
 * ======================================================================== */

Speech::Speech(QObject *parent) : QObject(parent) {
    qRegisterMetaType<QStringList>("QStringList");

    m_worker = new Worker;
    m_worker->onReady = [this](bool available, const QStringList &voices) {
        QMetaObject::invokeMethod(this, [this, available, voices] {
            engineReady(available, voices);
        }, Qt::QueuedConnection);
    };
    m_worker->onState = [this](int state, const QString &error) {
        QMetaObject::invokeMethod(this, [this, state, error] {
            stateFrom(state, error);
        }, Qt::QueuedConnection);
    };
    m_worker->onFinished = [this] {
        QMetaObject::invokeMethod(this, [this] { finishedFrom(); }, Qt::QueuedConnection);
    };
    m_worker->start();
}

Speech::~Speech() {
    if (m_worker) {
        m_worker->shutdown();
        delete m_worker;
        m_worker = nullptr;
    }
}

void Speech::speak(const QString &text, const QString &lang) {
    if (!m_worker)
        return;
    /*
     * 空文本在界面那边就该挡住（译文框空着时按钮是灰的），这里再兜一层：
     * 免得点一下什么反应都没有、也没句解释。
     */
    if (text.trimmed().isEmpty()) {
        if (m_speaking)
            stop();
        else
            setError(QStringLiteral("没有可念的内容"));
        return;
    }
    Worker::Work work;
    work.kind = Worker::Kind::Speak;
    work.text = text;
    work.lang = lang;
    m_worker->post(work);
}

void Speech::stop() {
    if (!m_worker)
        return;
    Worker::Work work;
    work.kind = Worker::Kind::Stop;
    m_worker->post(work);
}

void Speech::pause() {
    if (!m_worker)
        return;
    Worker::Work work;
    work.kind = Worker::Kind::Pause;
    m_worker->post(work);
}

void Speech::resume() {
    if (!m_worker)
        return;
    Worker::Work work;
    work.kind = Worker::Kind::Resume;
    m_worker->post(work);
}

void Speech::toggle(const QString &text, const QString &lang) {
    /*
     * "在念"这件事以界面这份状态为准（引擎那边是异步报回来的，这里要的是
     * "用户点这一下想干什么"）。正在念 -> 停；否则 -> 念这段（已经排队的那段
     * 由引擎那边的 PURGEBEFORESPEAK 顶掉）。
     *
     * 注意：点了"念"之后 speaking 要等引擎报回来才会变 true，这中间用户可能
     * 再点一次 —— 那次看到 speaking 还是 false，会再投一条 Speak（把前一条顶掉）。
     * 也就是"连点两下 = 重新开始念"，不会卡住。
     */
    if (m_speaking)
        stop();
    else
        speak(text, lang);
}

QString Speech::voiceFor(const QString &lang) const {
    const QString l = lang.trimmed().toLower();
    if (l.isEmpty())
        return m_defaultVoice;

    const QStringList hints = lcidHints(lang);
    for (const QString &voice : m_voices) {
        /* 清单里那一条长这样："Microsoft Huihui Desktop (804)" */
        const QString lower = voice.toLower();
        for (const QString &hint : hints) {
            if (lower.contains(hint))
                return voice;
        }
    }
    /*
     * 语言标签这条路在有些语音包上认不出来（它们不写 Language 属性）：
     * 退回按语种名猜 —— 中文那几个音色名是固定的（Huihui / Kangkang / Yaoyao）。
     */
    if (l.startsWith(QStringLiteral("zh")) || l.contains(QStringLiteral("中文"))) {
        for (const QString &voice : m_voices) {
            const QString d = voice.toLower();
            if (d.contains(QStringLiteral("huihui")) || d.contains(QStringLiteral("kangkang"))
                || d.contains(QStringLiteral("yaoyao")) || d.contains(QStringLiteral("chinese"))
                || d.contains(QStringLiteral("zh-")))
                return voice;
        }
    }
    return m_defaultVoice;
}

void Speech::engineReady(bool available, const QStringList &voices) {
    if (m_available != available) {
        m_available = available;
        emit changed();
        emit availableChanged();
    }
    if (m_voices != voices) {
        m_voices = voices;
        emit voicesChanged();
    }
    /*
     * 清单里第一条就是 SAPI 认定的默认音色（EnumTokens 按优先级排的）——
     * 界面问"某种语言用哪个音色"、引擎挑不出对应语言时，退的都是它。
     */
    m_defaultVoice = voices.isEmpty() ? QString() : voices.first();
    /*
     * 一个音色都没有时给一句人话（不是"报错"，是"这台机器上这条路走不通"）——
     * 界面靠 available 画灰按钮，靠 error 说明为什么。
     */
    if (!available && m_error.isEmpty())
        setError(QStringLiteral("这台机器上没有可用的语音包（Windows 设置 → 时间和语言 → 语音）"));
}

void Speech::stateFrom(int state, const QString &error) {
    const bool speaking = state != 0;
    const bool paused = state == 2;

    if (m_speaking != speaking) {
        m_speaking = speaking;
        emit speakingChanged();
    }
    if (m_paused != paused) {
        m_paused = paused;
        emit pausedChanged();
    }
    if (m_error != error)
        setError(error);
}

void Speech::finishedFrom() {
    /* 一段念完了：状态那边（stateFrom 0）已经把它落回"没在念" */
    emit finished();
}

void Speech::setError(const QString &text) {
    if (m_error == text)
        return;
    m_error = text;
    emit errorChanged();
}
