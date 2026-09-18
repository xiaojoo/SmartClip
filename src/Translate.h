#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QWidget>

class QNetworkAccessManager;
class QProcess;
class QQmlEngine;
class QQuickWidget;
class QTimer;
class TranslateCards;

/*
 * LLM 客户端（QML 单例 Llm，见 src/main.cpp 的 qmlRegisterSingletonInstance）。
 *
 * 三件事：
 *   1) **配置**：接口地址 / 密钥 / 模型名（api 模式），或者本地服务程序 +
 *      模型文件 + 端口（local 模式）。都落在 QSettings 的 translate/ 下，
 *      改一下立刻落盘，设置面板直接绑属性。
 *   2) **翻译**：translate() 发一次 OpenAI 兼容的 chat/completions 请求，
 *      结果通过 finished(token, 文字) / failed(token, 原因) 回来。
 *   3) **识别**：recognize() 把**一张图**（截图框选出来的那一块，PNG 的
 *      data URL）发给视觉模型，让它把图上的字读出来 —— 要译文就再加上
 *      "翻译成 X"这条指令，一次请求回来既是识别也是翻译。识别用的模型名
 *      单独一项（ocrModel），因为能看图的模型和纯文本模型通常不是同一个。
 *
 * 为什么走 HTTP 而不是内嵌一个推理库：
 *   现在主流的模型服务（OpenAI / DeepSeek / 通义 / Ollama / LM Studio /
 *   llama.cpp 的 llama-server）都提供同一套 OpenAI 兼容接口，一套 HTTP 客户端
 *   全都能用；内嵌推理（llama.cpp 的库）要跟一堆编译选项和显卡后端绑死，
 *   这个项目里不值得。
 *
 * 为什么要 token：一块翻译卡片发出去一个请求，用户可能在等的时候又点一次 ——
 * 回来的两份结果必须能分清是谁的。translate() 返回的串就是这份请求的身份证，
 * 界面把它记下来，回调里对上了才认。
 *
 * 密钥就存在 QSettings 里（明文，和这个程序里别的设置一样）。它只是本机
 * 桌面程序的一份配置，别在共享账号的机器上填自己的 key。
 */
class LlmClient final : public QObject {
    Q_OBJECT

    /*
     * "api" = 用现成的 API 服务；"local" = 启动本机的推理服务再连它。
     * 两种模式共用同一套请求代码，只有"接口地址从哪来"不一样。
     */
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY settingsChanged)
    /* API 模式：接口地址，可填到 /v1 或直接填到 /chat/completions */
    Q_PROPERTY(QString apiBase READ apiBase WRITE setApiBase NOTIFY settingsChanged)
    Q_PROPERTY(QString apiKey READ apiKey WRITE setApiKey NOTIFY settingsChanged)
    Q_PROPERTY(QString model READ model WRITE setModel NOTIFY settingsChanged)
    /*
     * 识别（看图）用的模型名。
     *
     * 为什么单独一项而不是共用上面那个 model：截图识别要**能看图的模型**
     * （gpt-4o / qwen-vl-max / glm-4v / gemini-flash 这类），而用户平时配的
     * 翻译模型多半是纯文本的（deepseek-chat）。共用一个的话，要么翻译用不上便宜的
     * 文本模型，要么识别永远报"这个模型不支持图片"。
     *
     * 留空就用 model —— 只配了一个视觉模型的用户（本地 Qwen2-VL 那种）不用填两遍。
     */
    Q_PROPERTY(QString ocrModel READ ocrModel WRITE setOcrModel NOTIFY settingsChanged)

    /* 本地模式：OpenAI 兼容的推理服务程序（如 llama-server.exe）+ 模型文件 */
    Q_PROPERTY(QString localExe READ localExe WRITE setLocalExe NOTIFY settingsChanged)
    Q_PROPERTY(QString localModel READ localModel WRITE setLocalModel NOTIFY settingsChanged)
    /*
     * 多模态投影文件（llama.cpp 的 --mmproj，形如 mmproj-model-f16.gguf）。
     *
     * 视觉模型（Qwen2-VL / Gemma-3 这类）要它才能把图片那半认起来；纯文本模型
     * 留空即可 —— 留空就**不加**这个参数。翻译这边现在只发文字，但启动参数
     * 先给上：模型是 VL 的、不带 mmproj 有时连加载都过不去。
     */
    Q_PROPERTY(QString localMmproj READ localMmproj WRITE setLocalMmproj NOTIFY settingsChanged)
    Q_PROPERTY(int localPort READ localPort WRITE setLocalPort NOTIFY settingsChanged)

    /* 启动本地服务用的那条完整命令行（设置面板显示出来给人核对） */
    Q_PROPERTY(QString localCommand READ localCommand NOTIFY settingsChanged)

    /* 新建卡片时的默认目标语言（语言名见 languages()） */
    Q_PROPERTY(QString defaultTarget READ defaultTarget WRITE setDefaultTarget
                   NOTIFY settingsChanged)

    /*
     * ---- 贴图"图上选字"用哪个引擎（两个选项，见 PinOverlay 的「认字」菜单） ----
     *
     *   "windows" 本机 Windows 自带 OCR（离线、不用配）
     *   "ppocr"   跑本机那个 PP-OCR 程序（RapidOCR / PaddleOCR，见 pinOcrRunner）
     *
     * 为什么这两个设置挂在这儿：它们和"识别"是一家人（上面就有 ocrModel），
     * 设置面板那一节也在一起；而且界面（PinOverlay / 设置面板）手里只有这几个
     * 单例，挂在别处还得再开一个单例。
     */
    Q_PROPERTY(QString pinOcrEngine READ pinOcrEngine WRITE setPinOcrEngine
                   NOTIFY settingsChanged)
    /* PP-OCR 那条路要跑的命令行（默认 python + 随包脚本，见 PinOcr::defaultRunnerCommand） */
    Q_PROPERTY(QString pinOcrRunner READ pinOcrRunner WRITE setPinOcrRunner
                   NOTIFY settingsChanged)

    /* 本地服务进程在不在（设置面板那个按钮的文案跟着它变） */
    Q_PROPERTY(bool localRunning READ localRunning NOTIFY localRunningChanged)
    /* 本地服务最近几行输出（启动失败时唯一能看的地方） */
    Q_PROPERTY(QString localLog READ localLog NOTIFY localLogChanged)

    /* 有请求在飞（界面据此转圈 / 禁用按钮） */
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    /* 一句话状态：正在翻译 / 就绪 / 出错原因（设置面板和卡片都显示它） */
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

    /*
     * 语言清单（中文名，直接进提示词）。
     * languages 第一项是"自动检测"（只当源语言用），targetLanguages 去掉它。
     */
    Q_PROPERTY(QStringList languages READ languages CONSTANT)
    Q_PROPERTY(QStringList targetLanguages READ targetLanguages CONSTANT)

public:
    explicit LlmClient(QObject *parent = nullptr);
    ~LlmClient() override;

    QString mode() const;
    void setMode(const QString &value);

    QString apiBase() const;
    void setApiBase(const QString &value);

    QString apiKey() const;
    void setApiKey(const QString &value);

    QString model() const;
    void setModel(const QString &value);

    QString ocrModel() const;
    void setOcrModel(const QString &value);
    /* 真去发识别请求时用的模型名：ocrModel 留空就退回 model */
    QString visionModel() const;

    QString localExe() const;
    void setLocalExe(const QString &value);

    QString localModel() const;
    void setLocalModel(const QString &value);

    QString localMmproj() const;
    void setLocalMmproj(const QString &value);

    int localPort() const;
    void setLocalPort(int value);

    QString localCommand() const;
    /*
     * 启动本地服务要用的参数（-m <模型> [--mmproj <投影>] --port <端口>）。
     * 单独一个函数是为了让"显示出来的命令行"和"真正启动时传的"永远是同一份。
     */
    QStringList localServerArgs() const;

    QString defaultTarget() const;
    void setDefaultTarget(const QString &value);

    bool localRunning() const;
    QString localLog() const { return m_localLog; }
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }

    QStringList languages() const;
    QStringList targetLanguages() const;

    /*
     * "识别 + 翻译"那条路的回复里，取回原文那一段（见 .cpp 里 kOcrSeparator 的说明）。
     *
     * 为什么要专门有它：那个"原文 ---- 译文"的格式是给**模型**下的规矩，它不一定
     * 老老实实照做（有时回一句"图上写着：…"，或者干脆只给译文）。界面上宁可
     * 显示一点东西也不要空着，所以：能按分隔行拆开就取前半段，拆不开就把整段
     * 当原文（译文那栏空着，用户还能自己点"翻译"再来一遍）。
     */
    Q_INVOKABLE QString ocrOriginal(const QString &result) const;

    /*
     * 翻译一条。返回这次请求的 token（界面记下来，和回调里的 token 对上才认）。
     * text 空 / 没配置模型 / 已经在忙，都会异步地回一个 failed(token, 原因)，
     * 不会阻塞调用方。
     */
    Q_INVOKABLE QString translate(const QString &text, const QString &target,
                                  const QString &source = QString());

    /*
     * 识别图上的字（截图框选出来的那一块）。
     *
     * imageDataUrl 是 png 的 data URL（"data:image/png;base64,…"，见
     * Screenshot::selectionImage）。target 为空 = 只要原文；填了（"中文（简体）"
     * 这种名字）= 一边认一边翻，回来的就是译文 —— 两步并一步，比"先识别再
     * 拿文字翻一遍"少一次往返，模型看着图翻也比看着 OCR 结果翻准。
     *
     * 和 translate 一样返回这次请求的 token，结果从 finished / failed 回来。
     * 图上没字（或者模型什么都没认出来）也会回一个 failed，界面上才有人话可说。
     */
    Q_INVOKABLE QString recognize(const QString &imageDataUrl, const QString &target = QString(),
                                  const QString &source = QString());

    /* 选字引擎 / PP-OCR 命令（见上面那两个 Q_PROPERTY 的说明） */
    QString pinOcrEngine() const;
    void setPinOcrEngine(const QString &value);
    QString pinOcrRunner() const;
    void setPinOcrRunner(const QString &value);

    /*
     * 试一下配置对不对：发一句最短的翻译，结果只更新 status。
     * 走的是和 translate 完全同一条路 ——"连得上"这件事没有第二种判据。
     */
    Q_INVOKABLE void probe();

    /*
     * 发一条**自定义提示词**的请求（通用的那一档，不写死"你是翻译"）。
     *
     * 为什么单开一个入口而不是让调用方拼 translate：translate 的 system 提示词
     * 是"只输出译文"那套规矩，硬塞一个别的任务进去会让模型两套指令打架。
     * 现在用它的是编辑区校验（src/Checker.cpp：中文用词 / 代码语法），
     * 它要的是"按固定格式输出问题清单"。
     *
     * 回调契约和 translate 一模一样：返回 token，结果从 finished/failed 回来。
     * busyStatus 是这期间状态栏上那句话（"正在校验…"）。
     */
    Q_INVOKABLE QString ask(const QString &systemPrompt, const QString &userText,
                            const QString &busyStatus = QString());

    /*
     * 自检用：假装本地推理服务已经就绪（自检里没法真拉一个 llama-server 起来）。
     *
     * 只影响 localRunning()，并且**和真就绪时走同一句 flushPending()** ——
     * 要钉的就是"模型冷启动时排队那条请求，发出去时 token 还是原来那个"
     * （见 askWithToken 的说明）。
     */
    Q_INVOKABLE void setLocalReadyForTest(bool on);

    /* 当前有几个请求在飞（校验和翻译可能同时在跑，busy 不能谁先回来谁关掉） */
    int inFlight() const { return m_inFlight; }

    /* 启动 / 停掉本地推理服务进程（local 模式） */
    Q_INVOKABLE bool startLocal();
    Q_INVOKABLE void stopLocal();

    /*
     * 退出前收尾：把本地服务进程掐掉。
     *
     * main.cpp 在事件循环还活着的时候调一次（和 notes.shutdown() 同一个位置）
     * —— 留到析构那会儿，子进程可能已经跟着进程树一起没了，Qt 会在收尾阶段
     * 报一堆"process destroyed while running"。
     */
    void shutdown();

signals:
    void settingsChanged();
    void localRunningChanged();
    void localLogChanged();
    void busyChanged();
    void statusChanged();
    /* 翻译回来了：token 是 translate() 返回的那个 */
    void finished(const QString &token, const QString &text);
    /* 出错了（网络 / 配置 / 模型返回的报错），error 是人能看懂的一句话 */
    void failed(const QString &token, const QString &error);

private:
    /* 拼出 chat/completions 的完整地址（api 模式用用户填的，本地模式用端口） */
    QString chatUrl() const;

    /*
     * ask() 的主体，token 由调用方给（ask 自己发，flushPending 用排队时那个）。
     *
     * 为什么要分开：本地模型冷启动时那条请求先排队，模型就绪后由 flushPending
     * **重新发一次**。那一下要是又走 ask()，它会再发一个新 token，而调用方
     * （校验那边）等的还是最早那个 —— 结果回来 token 对不上被当过期结果丢掉，
     * 界面上就一直停在"正在问模型…"（用户报的"一直在卡着"）。
     * 翻译那条（post）本来就是从上层把 token 传下来的，所以只有这条路有这问题。
     */
    void askWithToken(const QString &token, const QString &systemPrompt, const QString &userText,
                      const QString &busyStatus);
    QString baseUrl() const;
    /* 真正发请求；probe 为真时不发 finished，只更新 status */
    void post(const QString &token, const QString &text, const QString &target,
              const QString &source, bool probe);
    /*
     * 发一条**带图**的请求（识别那条路）。
     *
     * persona 挑的是这次要模型干哪一件事："ocr" = 只把图上的字读出来，
     * "ocr-translate" = 读出来再翻一遍（要翻译时回来的是"原文 / 分隔行 / 译文"
     * 两段，见 .cpp 里的 kOcrSeparator）。两套提示词和 post() 里那套翻译提示词
     * 分开写 —— 识别要的是"只输出读到的文字"，和翻译的规矩不是一套。
     */
    void postVision(const QString &token, const QString &imageDataUrl, const QString &target,
                    const QString &source, const QString &persona);
    /*
     * 最后那一步：把拼好的 body 发出去、认回复。翻译和识别共用（见 .cpp 里的说明）。
     * busyStatus 是这期间状态栏上那句话（"翻译中…" / "正在识别…"）。
     */
    void send(const QString &token, const QString &busyStatus, const QJsonObject &body, bool probe);

    /*
     * 等本地模型启动的那些请求（一般只有一条）。
     *
     * 本地模式下"还没启动"不再是错误：post() 会先把请求排在这儿、顺手把服务
     * 拉起来，等服务报"已就绪"（见 pollLocalReady）再原样发出去。模型加载要几秒
     * 到几十秒，用户点一下翻译就该等着，而不是先被要求去设置里点一下启动。
     */
    struct PendingRequest {
        QString token;
        QString text;
        QString target;
        QString source;
        bool probe = false;
        /*
         * 识别那几条请求也要能在本地模型加载期间排队（见上面那段），
         * 所以这里带上图：image 非空 = 这是一条识别请求，persona 是它的角色词。
         *
         * persona == "ask" 是第三种（见 ask()）：那时 source 里寄存的是
         * **system 提示词**（不是语言名），重新发出去时要原样还给 ask。
         */
        QString image;
        QString persona;
    };
    /* 模型就绪了：把排在最前面的那条发出去 */
    void flushPending();
    /* 启动失败 / 超时 / 被停掉：把排队的都按这个原因失败掉 */
    void failPending(const QString &reason);

    void setStatus(const QString &text);
    void setBusy(bool on);
    /*
     * 在飞请求的计数守卫（定义在 .cpp 里）：构造 +1、析构 -1，归零时收 busy。
     * 三条请求路径（post / postVision / ask）在发出去之前各建一个。
     */
    struct InFlightGuard;
    void persist(const QString &key, const QVariant &value);
    void appendLocalLog(const QString &chunk);
    /* 本地服务起来之后轮询 /models，能通了就报"已就绪" */
    void pollLocalReady(int attempt);

    QNetworkAccessManager *m_net = nullptr;
    QProcess *m_proc = nullptr;
    QTimer *m_readyTimer = nullptr;
    QString m_probeToken;
    /* 自检用：假装本地服务就绪（见 setLocalReadyForTest） */
    bool m_localReadyForTest = false;

    /* 配置（默认值见 .cpp 的构造函数） */
    QString m_mode;
    QString m_apiBase;
    QString m_apiKey;
    QString m_model;
    QString m_ocrModel;
    QString m_localExe;
    QString m_localModel;
    QString m_localMmproj;
    int m_localPort = 8080;
    QString m_defaultTarget;
    /* 贴图"图上选字"：用哪个引擎 + PP-OCR 那条命令（见上面那两个 Q_PROPERTY） */
    QString m_pinOcrEngine;
    QString m_pinOcrRunner;

    /* 等本地模型启动的那几条请求（见 PendingRequest） */
    QList<PendingRequest> m_pending;

    bool m_busy = false;
    /*
     * 在飞的请求数（ask / translate / recognize 都算）。
     *
     * 为什么不是一条 bool：校验发出去了、用户又点了一下翻译，两条请求同时在跑 ——
     * 先回来的那条把 busy 置假，界面上转圈就没了，可另一条还在等。
     * 所以 busy 改成"计数归零才算不忙"（见 send() 的回调里那一段）。
     */
    int m_inFlight = 0;
    QString m_status;
    QString m_localLog;
    int m_nextToken = 1;
};

/*
 * 桌面上的翻译卡片：**一个置顶无边框小窗 + 一个 QQuickWidget**，
 * 界面在 qml/translate/TranslateCard.qml 里 —— 和便签窗口
 * （StickyNoteWindow）、截图选区窗口是同一个套路，理由也一样：
 * 置顶 / 无边框 / 不进任务栏都是窗口标志位，拖动和拖边改大小走
 * startSystemMove / startSystemResize 交给窗口管理器。
 *
 * 它只有一张（不是便签那种一堆）：用户要的是"桌面上有一块翻译用的小卡片"。
 * 关掉 = hide()，数据留在 QSettings 里，下次启动按上次的样子回来。
 *
 * QML 侧拿到的属性（setInitialProperties 传进去，见构造函数的说明）：
 *   cardWin     这个窗口（拖动 / 改大小 / 关闭 / 置顶开关）
 *   textIn      上面那个框（输入）
 *   textOut     下面那个框（译文）
 *   sourceLang  源语言（"自动检测" = 让模型自己认）
 *   targetLang  目标语言
 *   pinned      置顶（默认开）
 *   cardNumber  身份号（标题栏"翻译"旁边那个号，自检也用）
 *
 * 翻译本身不在这里：QML 直接调 Llm.translate(...)，结果写回 textOut。
 * 这个类只管"窗口 + 这份状态存哪儿"。
 */
class TranslateCard final : public QWidget {
    Q_OBJECT

    Q_PROPERTY(QString textIn READ textIn WRITE setTextIn NOTIFY textInChanged)
    Q_PROPERTY(QString textOut READ textOut WRITE setTextOut NOTIFY textOutChanged)
    Q_PROPERTY(QString sourceLang READ sourceLang WRITE setSourceLang NOTIFY languagesChanged)
    Q_PROPERTY(QString targetLang READ targetLang WRITE setTargetLang NOTIFY languagesChanged)
    Q_PROPERTY(bool pinned READ pinned WRITE setPinned NOTIFY pinnedChanged)
    Q_PROPERTY(int cardNumber READ cardNumber CONSTANT)

public:
    /* engine 必须传：不传 QQuickWidget 会自己 new 一个引擎，卡片里就 import
     * 不到 SmartClip.Globals（见构造函数里的说明）。 */
    explicit TranslateCard(QQmlEngine *engine);
    ~TranslateCard() override;

    QString textIn() const { return m_textIn; }
    void setTextIn(const QString &text);

    QString textOut() const { return m_textOut; }
    void setTextOut(const QString &text);

    QString sourceLang() const { return m_sourceLang; }
    void setSourceLang(const QString &lang);

    QString targetLang() const { return m_targetLang; }
    void setTargetLang(const QString &lang);

    bool pinned() const { return m_pinned; }
    void setPinned(bool on);

    int cardNumber() const { return m_number; }

    QObject *qmlRoot() const;
    /* 界面加载失败时那条错误（成功返回空串）；自检拿它钉"卡片真的建起来了" */
    QString qmlError() const;

    /* 卡片所在那块屏的工作区（摆位用，和便签那边同一个理由） */
    Q_INVOKABLE QRect screenBounds() const;

    /* ---- QML 调的（界面动作） ---- */
    /* 标题栏按住：交给窗口管理器拖动 */
    Q_INVOKABLE void beginDrag();
    /* 右下角按住：交给窗口管理器改大小 */
    Q_INVOKABLE void beginResize();
    /* 关掉卡片：藏起来（状态留着，图标条 / 托盘能再叫出来） */
    Q_INVOKABLE void closeCard();
    /* 把译文复制到系统剪贴板 */
    Q_INVOKABLE void copyResult();
    /* 源 / 目标语言对调（"自动检测"不参与对调，那时只把目标语言抄到源语言） */
    Q_INVOKABLE void swapLanguages();
    /* 清空两个框 */
    Q_INVOKABLE void clearAll();
    Q_INVOKABLE void togglePinned();

signals:
    void textInChanged();
    void textOutChanged();
    void languagesChanged();
    void pinnedChanged();
    /* 卡片被藏起来了（总管据此更新"桌面上还有没有"） */
    void closed();
    /* 卡片里的状态改了（总管据此排一次落盘） */
    void stateChanged();

protected:
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void applyWindowFlags();
    /*
     * 把这个属性推给 QML 根对象（restore 时用）。
     *
     * 两份状态的分工：**窗口在跑的时候 QML 是界面那一份**（输入框里用户敲的字
     * 由它写回这里），C++ 这份是落盘用的存档。恢复时方向相反 —— 存档里的值要
     * 推回界面，所以每个 setter 都顺手同步一次。值相同就不推（setProperty 会
     * 打断 QML 那边的绑定，也会绕回来）。
     */
    void pushToQml(const char *name, const QVariant &value);

    friend class TranslateCards;

    QQmlEngine *m_engine = nullptr;
    QQuickWidget *m_view = nullptr;

    QString m_textIn;
    QString m_textOut;
    QString m_sourceLang;
    QString m_targetLang;
    bool m_pinned = true;
    int m_number = 1;
    /* 正在 placeAt 里改几何：那会儿的 move/resize 不该再写一次位置 */
    bool m_placing = false;
};

/*
 * 翻译卡片总管（QML 单例 Trans，见 src/main.cpp）。
 *
 * 管三件事：卡片窗口的生死、上次的样子（QSettings 的 translate/card/*）、
 * 以及"把它叫出来"这一个入口（图标条 / 托盘 / 快捷键都落到 showCard()）。
 *
 * 只有一张卡片，所以这里没有便签那套清单和组合逻辑：showCard() 建一次，
 * 之后就是 show + raise。窗口**不设父子**（顶层窗口），显式指针管着它。
 */
class TranslateCards final : public QObject {
    Q_OBJECT

    /* 摆着的卡片数（0 / 1）—— 图标条那一格亮不亮看它 */
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY changed)

public:
    /*
     * llm 是那个 LLM 客户端（新建卡片时向它要默认目标语言；可为 nullptr）。
     * engine 是主窗口那个 QQuickWidget 的引擎（可为 nullptr，那样卡片的 QML
     * 就 import 不到 SmartClip.Globals 那几个单例）。都**不**登记成父子关系
     * —— 引擎是宿主 QWidget 的子对象，而本对象比那个 QWidget 活得久
     * （见 main.cpp 里的声明顺序）。
     */
    explicit TranslateCards(LlmClient *llm = nullptr, QQmlEngine *engine = nullptr,
                            QObject *parent = nullptr);
    ~TranslateCards() override;

    /* 引擎建好之后补挂一次（main.cpp 里 quick 建好就调，见便签那边同一个坑） */
    void attachEngine(QQmlEngine *engine);

    /* 上次退出时卡片是摆着的就恢复出来（启动时调一次） */
    void start();
    /* 退出前收尾：落盘（幂等） */
    void shutdown();

    /* 把卡片叫到桌面上：没有就建，有就 show + raise + 激活 */
    Q_INVOKABLE TranslateCard *showCard();
    /* 收起来（数据留着） */
    Q_INVOKABLE void hideCard();

    int visibleCount() const;

    /* 卡片状态改了：排一次落盘（防抖，和便签那份 store 一个做法） */
    void scheduleSave();
    /* 卡片被藏起来了 */
    void onCardClosed();

signals:
    void changed();

private:
    TranslateCard *ensureCard();
    /* 把 QSettings 里那份存档读进卡片（正文 / 语言 / 置顶 / 位置） */
    void restore(TranslateCard *card);
    void save() const;

    LlmClient *m_llm = nullptr;
    QQmlEngine *m_engine = nullptr;
    QPointer<TranslateCard> m_card;
    QTimer *m_saveTimer = nullptr;
    /* 卡片编号：只有一张，但界面上"翻译 1"那个号还是要发 */
    int m_nextNumber = 1;
};
