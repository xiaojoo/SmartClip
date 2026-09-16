#include "Translate.h"

#include <QBuffer>
#include <QClipboard>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMoveEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWidget>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantMap>
#include <QWindow>

#include "PinOcr.h"

namespace {

/* 设置键（都挂在 translate/ 下，和别的界面设置同一个 QSettings 目录） */
constexpr const char *kKeyBase = "translate/";
/* 新建卡片的默认目标语言。和 LlmClient::defaultTarget 是同一个键 */
constexpr const char *kKeyDefaultTarget = "translate/target";
constexpr const char *kKeyCard = "translate/card/";

/* 卡片默认尺寸（自检和"找回屏幕里"都用它兜底） */
constexpr int kDefaultW = 420;
constexpr int kDefaultH = 380;
constexpr int kMinW = 340;
constexpr int kMinH = 280;

/*
 * 语言清单（中文名，直接进提示词）。
 *
 * 第一项是"自动检测"，只当源语言用 —— 目标语言不能是"自动"。
 * 这份表就是界面下拉里的全部内容，模型不需要认识语言代码。
 */
const QStringList &languageList() {
    static const QStringList kList{
        QStringLiteral("自动检测"),   QStringLiteral("中文（简体）"), QStringLiteral("繁体中文"),
        QStringLiteral("英语"),       QStringLiteral("日语"),        QStringLiteral("韩语"),
        QStringLiteral("法语"),       QStringLiteral("德语"),        QStringLiteral("西班牙语"),
        QStringLiteral("俄语"),       QStringLiteral("葡萄牙语"),    QStringLiteral("意大利语"),
        QStringLiteral("阿拉伯语"),   QStringLiteral("泰语"),        QStringLiteral("越南语"),
    };
    return kList;
}

QString defaultTargetLanguage() {
    return QSettings().value(QString::fromLatin1(kKeyDefaultTarget),
                             QStringLiteral("中文（简体）")).toString();
}

/*
 * 识别 + 翻译那条路回来时的分隔行（提示词里的格式约定，见 postVision）。
 *
 * "第一段原文 / 分隔行 / 第二段译文" —— 抠原文（ocrOriginal）就是按这一行切。
 * 提示词里写的就是这一串，两处必须一致，所以只留 C++ 这一份，界面不自己抄。
 * 行首行尾刻意不留标记词（不写"【原文】"）：那样切出来的原文还得再洗一遍
 * 才干净，而用户多半是要直接复制走的。
 */
constexpr const char *kOcrSeparator = "----";

/*
 * 识别那两种任务的代号（recognize -> postVision 之间传的就是它）。
 *
 * 用常量而不是在两处各写一遍字面量：哪天改了字面量而漏改一处，请求会**悄悄地**
 * 退回"只识别"那条提示词（多模态的 body 照样发得出去，不报错），
 * 只有拿真模型才看得出来翻译没了。
 */
constexpr const char *kPersonaOcr = "ocr";
constexpr const char *kPersonaOcrTranslate = "ocr-translate";

}  // namespace

/* ===========================================================================
 * LlmClient
 * ======================================================================== */

LlmClient::LlmClient(QObject *parent) : QObject(parent) {
    m_net = new QNetworkAccessManager(this);

    QSettings settings;
    m_mode = settings.value(QString::fromLatin1(kKeyBase) + QStringLiteral("mode"),
                            QStringLiteral("api")).toString();
    m_apiBase = settings.value(QString::fromLatin1(kKeyBase) + QStringLiteral("apiBase"),
                               QStringLiteral("https://api.deepseek.com/v1")).toString();
    m_apiKey = settings.value(QString::fromLatin1(kKeyBase) + QStringLiteral("apiKey")).toString();
    m_model = settings.value(QString::fromLatin1(kKeyBase) + QStringLiteral("model"),
                             QStringLiteral("deepseek-chat")).toString();
    /* 识别模型留空 = 用上面那个 model（见 Translate.h 里 ocrModel 的说明） */
    m_ocrModel = settings.value(QString::fromLatin1(kKeyBase) + QStringLiteral("ocrModel")).toString();
    m_localExe = settings.value(QString::fromLatin1(kKeyBase) + QStringLiteral("localExe")).toString();
    m_localModel = settings.value(QString::fromLatin1(kKeyBase) + QStringLiteral("localModel")).toString();
    m_localMmproj = settings.value(QString::fromLatin1(kKeyBase) + QStringLiteral("localMmproj")).toString();
    m_localPort = settings.value(QString::fromLatin1(kKeyBase) + QStringLiteral("localPort"), 8080).toInt();
    m_defaultTarget = defaultTargetLanguage();
    /*
     * 贴图"图上选字"：默认用 Windows 自带那套（离线、不用配，装上就能用）。
     * PP-OCR 那条命令默认填好（python + 随包脚本）—— 填好用户才看得见"要装什么"。
     */
    m_pinOcrEngine = settings.value(QString::fromLatin1(kKeyBase) + QStringLiteral("pinOcrEngine"),
                                    QStringLiteral("windows")).toString();
    m_pinOcrRunner = settings.value(QString::fromLatin1(kKeyBase) + QStringLiteral("pinOcrRunner"),
                                    PinOcr::defaultRunnerCommand()).toString();
}

LlmClient::~LlmClient() {
    stopLocal();
}

void LlmClient::persist(const QString &key, const QVariant &value) {
    QSettings().setValue(QString::fromLatin1(kKeyBase) + key, value);
}

QString LlmClient::mode() const { return m_mode; }
void LlmClient::setMode(const QString &value) {
    const QString clean = (value == QLatin1String("local")) ? QStringLiteral("local")
                                                            : QStringLiteral("api");
    if (m_mode == clean)
        return;
    m_mode = clean;
    persist(QStringLiteral("mode"), clean);
    emit settingsChanged();
}

QString LlmClient::apiBase() const { return m_apiBase; }
void LlmClient::setApiBase(const QString &value) {
    if (m_apiBase == value)
        return;
    m_apiBase = value;
    persist(QStringLiteral("apiBase"), value);
    emit settingsChanged();
}

QString LlmClient::apiKey() const { return m_apiKey; }
void LlmClient::setApiKey(const QString &value) {
    if (m_apiKey == value)
        return;
    m_apiKey = value;
    persist(QStringLiteral("apiKey"), value);
    emit settingsChanged();
}

QString LlmClient::model() const { return m_model; }
void LlmClient::setModel(const QString &value) {
    if (m_model == value)
        return;
    m_model = value;
    persist(QStringLiteral("model"), value);
    emit settingsChanged();
}

QString LlmClient::ocrModel() const { return m_ocrModel; }

void LlmClient::setOcrModel(const QString &value) {
    if (m_ocrModel == value)
        return;
    m_ocrModel = value;
    persist(QStringLiteral("ocrModel"), value);
    emit settingsChanged();
}

QString LlmClient::visionModel() const {
    const QString ocr = m_ocrModel.trimmed();
    return ocr.isEmpty() ? m_model.trimmed() : ocr;
}

QString LlmClient::localExe() const { return m_localExe; }
void LlmClient::setLocalExe(const QString &value) {
    if (m_localExe == value)
        return;
    m_localExe = value;
    persist(QStringLiteral("localExe"), value);
    emit settingsChanged();
}

QString LlmClient::localModel() const { return m_localModel; }
void LlmClient::setLocalModel(const QString &value) {
    if (m_localModel == value)
        return;
    m_localModel = value;
    persist(QStringLiteral("localModel"), value);
    emit settingsChanged();
}

int LlmClient::localPort() const { return m_localPort; }

void LlmClient::setLocalPort(int value) {
    const int clean = qBound(1, value, 65535);
    if (m_localPort == clean)
        return;
    m_localPort = clean;
    persist(QStringLiteral("localPort"), clean);
    emit settingsChanged();
}

QString LlmClient::localMmproj() const { return m_localMmproj; }

void LlmClient::setLocalMmproj(const QString &value) {
    if (m_localMmproj == value)
        return;
    m_localMmproj = value;
    persist(QStringLiteral("localMmproj"), value);
    emit settingsChanged();
}

QStringList LlmClient::localServerArgs() const {
    QStringList args;
    args << QStringLiteral("-m") << m_localModel.trimmed();
    /* 纯文本模型没有这一项；填了才加（见头文件里 localMmproj 的说明） */
    if (!m_localMmproj.trimmed().isEmpty())
        args << QStringLiteral("--mmproj") << m_localMmproj.trimmed();
    args << QStringLiteral("--port") << QString::number(m_localPort);
    return args;
}

QString LlmClient::localCommand() const {
    /* 只是给人看的：带空格的路径按命令行习惯加引号 */
    auto quoted = [](const QString &part) {
        return part.contains(QLatin1Char(' ')) ? QLatin1Char('"') + part + QLatin1Char('"')
                                               : part;
    };
    QStringList parts;
    parts << quoted(m_localExe.trimmed());
    const QStringList args = localServerArgs();
    for (const QString &arg : args)
        parts << quoted(arg);
    return parts.join(QLatin1Char(' ')).trimmed();
}

QString LlmClient::defaultTarget() const { return m_defaultTarget; }
void LlmClient::setDefaultTarget(const QString &value) {
    if (m_defaultTarget == value || value.isEmpty())
        return;
    m_defaultTarget = value;
    persist(QStringLiteral("target"), value);
    emit settingsChanged();
}

bool LlmClient::localRunning() const {
    return m_proc && m_proc->state() != QProcess::NotRunning;
}

/* ---- 贴图"图上选字"的两个设置（见 Translate.h） ---- */

QString LlmClient::pinOcrEngine() const { return m_pinOcrEngine; }

void LlmClient::setPinOcrEngine(const QString &value) {
    const QString clean = value.trimmed().toLower();
    if (m_pinOcrEngine == clean || clean.isEmpty())
        return;
    m_pinOcrEngine = clean;
    persist(QStringLiteral("pinOcrEngine"), clean);
    emit settingsChanged();
}

QString LlmClient::pinOcrRunner() const { return m_pinOcrRunner; }

void LlmClient::setPinOcrRunner(const QString &value) {
    if (m_pinOcrRunner == value)
        return;
    m_pinOcrRunner = value;
    persist(QStringLiteral("pinOcrRunner"), value);
    emit settingsChanged();
}

QStringList LlmClient::languages() const { return languageList(); }

QString LlmClient::ocrOriginal(const QString &result) const {
    const QString text = result.trimmed();
    const QString separator = QString::fromUtf8(kOcrSeparator);
    const int at = text.indexOf(separator);
    if (at < 0)
        return text;
    return text.left(at).trimmed();
}

QStringList LlmClient::targetLanguages() const {
    QStringList list = languageList();
    list.removeFirst();  /* "自动检测"不能当目标语言 */
    return list;
}

QString LlmClient::baseUrl() const {
    if (m_mode == QLatin1String("local"))
        return QStringLiteral("http://127.0.0.1:%1/v1").arg(m_localPort);
    return m_apiBase.trimmed();
}

QString LlmClient::chatUrl() const {
    QString base = baseUrl();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    if (base.isEmpty())
        return QString();
    /* 用户可能只填到 /v1，也可能把整条 chat/completions 都粘进来，两种都认 */
    if (base.endsWith(QLatin1String("/chat/completions")))
        return base;
    return base + QStringLiteral("/chat/completions");
}

void LlmClient::setStatus(const QString &text) {
    if (m_status == text)
        return;
    m_status = text;
    emit statusChanged();
}

void LlmClient::setBusy(bool on) {
    if (m_busy == on)
        return;
    m_busy = on;
    emit busyChanged();
}

/*
 * 在飞计数 +1，并且**保证**在函数返回前还回去。
 *
 * 把 +1 / -1 配成一对放在同一个作用域里：post() / postVision() / ask() 这三条
 * 路上有好几个提前 return 的失败分支，散着写迟早漏掉一个（漏掉一次 busy 就
 * 永远挂在"忙"上，之后再也不会变成可点）。这个守卫在析构时兜底。
 */
struct LlmClient::InFlightGuard {
    LlmClient *self;
    explicit InFlightGuard(LlmClient *s) : self(s) { ++self->m_inFlight; }
    ~InFlightGuard() {
        if (self->m_inFlight > 0 && --self->m_inFlight == 0)
            self->setBusy(false);
    }
    InFlightGuard(const InFlightGuard &) = delete;
    InFlightGuard &operator=(const InFlightGuard &) = delete;
};

QString LlmClient::translate(const QString &text, const QString &target, const QString &source) {
    const QString token = QStringLiteral("t%1").arg(++m_nextToken);
    post(token, text, target, source, false);
    return token;
}

/*
 * 识别：把框选出来的那块图交给视觉模型读字。
 *
 * 和 translate() 走同一条回调契约（token + finished/failed），区别只有两点：
 * 发的是带图的消息，用的是 visionModel()（见 ocrModel 的说明）。
 *
 * target 为空 = 只要图上那点字；填了 = 一边认一边翻，**同一次请求**里做完
 * （模型这会儿手里就有图，版式、表格、图上那些没写全的话都看得见，比"先抠出
 * 一段文字、再发一次纯文本翻译"少一次往返，也更不容易翻串行）。
 * 两种情况下回来都是纯文字：
 *   * 只识别   —— 就是图上那段原文；
 *   * 识别+翻译 —— "原文" + 一条分隔行 + "译文"（见 postVision 里的约定，
 *                  界面按它拆成上下两栏）。
 */
QString LlmClient::recognize(const QString &imageDataUrl, const QString &target,
                             const QString &source) {
    const QString token = QStringLiteral("r%1").arg(++m_nextToken);
    const bool wantTranslate = !target.trimmed().isEmpty();
    postVision(token, imageDataUrl, target, source,
               wantTranslate ? QString::fromLatin1(kPersonaOcrTranslate)
                             : QString::fromLatin1(kPersonaOcr));
    return token;
}

QString LlmClient::ask(const QString &systemPrompt, const QString &userText,
                       const QString &busyStatus) {
    const QString token = QStringLiteral("a%1").arg(++m_nextToken);

    auto failLater = [this, token](const QString &reason) {
        if (m_inFlight > 0 && --m_inFlight == 0)
            setBusy(false);
        setStatus(reason);
        QTimer::singleShot(0, this, [this, token, reason]() { emit failed(token, reason); });
    };

    if (userText.trimmed().isEmpty()) {
        failLater(QStringLiteral("没有要交给模型的内容"));
        return token;
    }
    if (chatUrl().isEmpty()) {
        failLater(QStringLiteral("还没配置接口地址（设置 → 模型）"));
        return token;
    }
    if (m_mode == QLatin1String("api") && m_model.trimmed().isEmpty()) {
        failLater(QStringLiteral("还没填模型名（设置 → 模型）"));
        return token;
    }
    /* 本地模式还没起来：和翻译那条一样排队等它加载完（见 post 里那段说明） */
    if (m_mode == QLatin1String("local") && !localRunning()) {
        PendingRequest pending;
        pending.token = token;
        pending.text = userText;
        pending.source = systemPrompt;   /* 见 flushPending：ask 那条靠它带回提示词 */
        pending.persona = QStringLiteral("ask");
        m_pending.append(pending);
        setBusy(true);
        setStatus(QStringLiteral("正在启动本地模型…"));
        if (!startLocal())
            failPending(m_status);
        return token;
    }

    QJsonArray messages;
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                {QStringLiteral("content"), systemPrompt}});
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"), userText}});

    QJsonObject body{
        {QStringLiteral("model"), m_model.trimmed()},
        {QStringLiteral("messages"), messages},
        /*
         * 温度给 0：校验要的是"同一份正文两次跑出同样的结果"。
         * 翻译那条用 0.2 是另一回事（稍微松一点译文更自然）。
         */
        {QStringLiteral("temperature"), 0},
        {QStringLiteral("stream"), false},
    };

    /* 这条已经在飞了（见 InFlightGuard）：和 post 里那处同一个位置 */
    const InFlightGuard guard(this);
    send(token, busyStatus.trimmed().isEmpty() ? QStringLiteral("正在请求模型…")
                                               : busyStatus,
         body, false);
    return token;
}

void LlmClient::probe() {
    /*
     * 试连接用一句真翻译：能和不能的唯一判据就是"这一次请求回不回得来"，
     * 单独发个 /models 探测会漏掉"模型名填错了"这类更常见的问题。
     */
    m_probeToken = QStringLiteral("probe%1").arg(++m_nextToken);
    setStatus(QStringLiteral("正在测试…"));
    post(m_probeToken, QStringLiteral("你好，世界"), defaultTargetLanguage(), QString(), true);
}

void LlmClient::post(const QString &token, const QString &text, const QString &target,
                     const QString &source, bool probe) {
    /*
     * 配置不全 / 输入是空的：**异步**回一个失败。
     *
     * 不能在这里直接 emit —— QML 那边是"先拿到 token 记下来，再等信号"，
     * 同步发出去的话回调跑在 translate() 返回之前，token 还没记上，
     * 界面上就是"点了翻译什么也没发生"。
     */
    auto failLater = [this, token](const QString &reason) {
        setBusy(false);
        setStatus(reason);
        if (token != m_probeToken)
            QTimer::singleShot(0, this, [this, token, reason]() { emit failed(token, reason); });
    };

    if (text.trimmed().isEmpty()) {
        failLater(QStringLiteral("还没有输入要翻译的内容"));
        return;
    }
    if (chatUrl().isEmpty()) {
        failLater(QStringLiteral("还没配置接口地址（设置 → 模型）"));
        return;
    }
    if (m_mode == QLatin1String("api") && m_model.trimmed().isEmpty()) {
        failLater(QStringLiteral("还没填模型名（设置 → 模型）"));
        return;
    }
    /*
     * 本地模式还没启动：**自动把它拉起来**，这条请求排在队里等着。
     *
     * 用户要的就是"配好一次，以后不用手点启动"：点翻译那一刻服务自己起来，
     * 模型加载完（见 pollLocalReady）这条请求原样发出去。模型加载几秒到几十秒，
     * 界面那边一直是"翻译中…"（busy 已经置上），不是报错。
     */
    if (m_mode == QLatin1String("local") && !localRunning()) {
        m_pending.append(PendingRequest{token, text, target, source, probe});
        setBusy(true);
        setStatus(QStringLiteral("正在启动本地模型…"));
        if (!startLocal()) {
            /* 配置不全（程序 / 模型没填）：startLocal 里已经把原因写进 status 了 */
            failPending(m_status);
        }
        return;
    }

    const QString to = target.trimmed().isEmpty() ? defaultTargetLanguage() : target.trimmed();
    const bool autoSource = source.trimmed().isEmpty()
                            || source.trimmed() == QStringLiteral("自动检测");

    QString system = QStringLiteral(
        "你是一名专业翻译。把用户给的内容翻译成%1。"
        "只输出译文本身：不要解释、不要加引号、不要写原文，保留原有的分段和格式。")
        .arg(to);
    system += autoSource ? QStringLiteral(" 源语言请自行判断。")
                         : QStringLiteral(" 源语言是%1。").arg(source.trimmed());

    QJsonArray messages;
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                {QStringLiteral("content"), system}});
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"), text}});

    QJsonObject body{
        {QStringLiteral("model"), m_model.trimmed()},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("temperature"), 0.2},
        {QStringLiteral("stream"), false},
    };

    /* 这条已经在飞了（见 InFlightGuard）：+1/-1 配成一对，下面所有 return 都安全 */
    const InFlightGuard guard(this);
    send(token, probe ? QStringLiteral("正在测试…") : QStringLiteral("翻译中…"), body, probe);
}


/*
 * 识别那条路：把图（png 的 data URL）和一句"要干什么"发给视觉模型。
 *
 * 消息体用 OpenAI 兼容的多模态格式（content 是个数组，元素里 type=text 是
 * 提示词、type=image_url 是图）。OpenAI / 通义 / 智谱 / Kimi / 本地 llama.cpp
 * 的 server 都是这一套；只填文字（content 是字符串）那边也认，但图片就等于
 * 没发 —— 模型会回一句"我没有收到图片"，所以这个格式不能省。
 *
 * 图片用 data URL（base64 内联）而不是先上传拿 URL：这些服务对图片地址的支持
 * 各不相同，内联是最通用的一种；截图那一块通常几十 KB，不至于撑坏请求。
 */
void LlmClient::postVision(const QString &token, const QString &imageDataUrl,
                           const QString &target, const QString &source,
                           const QString &persona) {
    auto failLater = [this, token](const QString &reason) {
        setBusy(false);
        setStatus(reason);
        QTimer::singleShot(0, this, [this, token, reason]() { emit failed(token, reason); });
    };

    if (imageDataUrl.trimmed().isEmpty()) {
        failLater(QStringLiteral("没拿到要识别的图像（选区是不是太小了？）"));
        return;
    }
    if (chatUrl().isEmpty()) {
        failLater(QStringLiteral("还没配置接口地址（设置 → 模型）"));
        return;
    }
    if (m_mode == QLatin1String("api") && visionModel().isEmpty()) {
        failLater(QStringLiteral("还没填识别用的模型名（设置 → 模型）"));
        return;
    }
    /* 本地模型还没起来：和翻译一样排队等它加载完（见 post 里那段说明） */
    if (m_mode == QLatin1String("local") && !localRunning()) {
        PendingRequest pending;
        pending.token = token;
        pending.target = target;
        pending.source = source;
        pending.image = imageDataUrl;
        pending.persona = persona;
        m_pending.append(pending);
        setBusy(true);
        setStatus(QStringLiteral("正在启动本地模型…"));
        if (!startLocal())
            failPending(m_status);
        return;
    }

    const bool wantTranslate = (persona == QLatin1String(kPersonaOcrTranslate));
    const QString to = target.trimmed();
    const bool autoSource = source.trimmed().isEmpty()
                            || source.trimmed() == QStringLiteral("自动检测");

    QString system = QStringLiteral(
        "你是一名文字识别助手。请把图片里的文字**原样**读出来：不要翻译、不要解释、"
        "不要加任何前后缀，按图片里的换行和版式分段。");
    if (wantTranslate) {
        /*
         * 一边认一边翻。中间那条分隔行是界面拆"原文 / 译文"两栏的依据
         * （见 kOcrSeparator 和 LlmClient::ocrOriginal），所以格式得说死 ——
         * 让模型自己发挥的话，界面上就分不清哪段是原文了。
         *
         * 分隔行是拼上去的（不写进带 %1 的那条串里）：它和 %1 都是"-"
         * 开头的替换位，混在一起容易数错第几个。
         */
        system = QStringLiteral(
                     "你是一名文字识别 + 翻译助手。先把图片里的文字**原样**读出来，"
                     "再把它翻译成%1。只按下面这个格式输出，不要加任何别的话：\n"
                     "第一段：读出来的原文，按图片里的换行和版式分段；\n"
                     "然后单独一行写 ")
                     .arg(to)
                 + QString::fromUtf8(kOcrSeparator)
                 + QStringLiteral("；\n第二段：译好的译文，同样保留原有分段。");
        system += autoSource ? QStringLiteral(" 源语言请自行判断。")
                             : QStringLiteral(" 源语言是%1。").arg(source.trimmed());
    }

    QJsonArray content;
    content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                               {QStringLiteral("text"), system}});
    content.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("image_url")},
        {QStringLiteral("image_url"),
         QJsonObject{{QStringLiteral("url"), imageDataUrl}}}});

    QJsonArray messages;
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"), content}});

    QJsonObject body{
        {QStringLiteral("model"), visionModel()},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("temperature"), 0.2},
        {QStringLiteral("stream"), false},
    };

    /* 这条已经在飞了（见 InFlightGuard） */
    const InFlightGuard guard(this);
    send(token, wantTranslate ? QStringLiteral("正在识别并翻译…") : QStringLiteral("正在识别…"),
         body, false);
}

/*
 * 发一条 chat/completions 请求，并把回来的那句话派给调用方。
 *
 * 这一步是翻译和识别共用的：两条路只有"拼什么 body / 用哪个模型 / 状态栏写什么"
 * 不同，发出去之后**出错怎么认、内容从哪儿取、token 怎么对上**完全一样 ——
 * 分成两份的话，迟早有一份会漏掉某个字段（比如只在一份里读 error.message）。
 */
void LlmClient::send(const QString &token, const QString &busyStatus, const QJsonObject &body,
                     bool probe) {
    QNetworkRequest request{QUrl(chatUrl())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!m_apiKey.trimmed().isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_apiKey.trimmed().toUtf8());
    /* 大模型第一条要等模型加载 / 排队，给足两分钟；卡住总比无限等强 */
    request.setTransferTimeout(120000);

    setBusy(true);
    setStatus(busyStatus);

    QNetworkReply *reply = m_net->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, token, probe]() {
        reply->deleteLater();
        /*
         * 在飞的请求数减一，**归零才算不忙**。
         *
         * 原来这里是无条件 setBusy(false)：翻译和识别（现在还有校验）同时在跑的
         * 时候，先回来的那条会把忙碌状态撤掉，界面上的转圈提前消失、
         * 按钮也提前变成可点的 —— 但另一条其实还在等。
         */
        if (m_inFlight > 0 && --m_inFlight == 0)
            setBusy(false);

        const QByteArray raw = reply->readAll();
        const QJsonObject root = QJsonDocument::fromJson(raw).object();

        /* 出错时优先报模型返回的那句话 —— 比 Qt 的 "Error transferring…" 有用 */
        QString error;
        const QJsonObject errorObj = root.value(QStringLiteral("error")).toObject();
        if (!errorObj.isEmpty())
            error = errorObj.value(QStringLiteral("message")).toString();
        if (error.isEmpty() && reply->error() != QNetworkReply::NoError)
            error = reply->errorString();

        QString content;
        if (error.isEmpty()) {
            const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
            if (!choices.isEmpty()) {
                content = choices.first().toObject()
                              .value(QStringLiteral("message")).toObject()
                              .value(QStringLiteral("content")).toString();
            }
            /* 有些服务在流式之外的字段上放结果，认不出就当空 */
            if (content.isEmpty())
                error = QStringLiteral("模型没有返回内容");
        }

        content = content.trimmed();

        if (!error.isEmpty()) {
            setStatus(error);
            if (!probe)
                emit failed(token, error);
            else
                m_probeToken.clear();
            return;
        }

        if (probe) {
            setStatus(QStringLiteral("连接正常，模型可用"));
            m_probeToken.clear();
            return;
        }
        setStatus(QString());
        emit finished(token, content);
    });
}

void LlmClient::appendLocalLog(const QString &chunk) {
    QString log = m_localLog + chunk;
    /* 只留最后一段：llama-server 起步就刷几十行，全留着界面也没地方放 */
    if (log.size() > 600)
        log = log.right(600);
    m_localLog = log;
    emit localLogChanged();
}

bool LlmClient::startLocal() {
    if (localRunning())
        return true;
    if (m_localExe.trimmed().isEmpty() || m_localModel.trimmed().isEmpty()) {
        setStatus(QStringLiteral("还没填本地推理程序或模型文件（设置 → 模型）"));
        return false;
    }

    m_localLog.clear();
    emit localLogChanged();

    m_proc = new QProcess(this);
    m_proc->setProgram(m_localExe.trimmed());
    /*
     * llama-server 的命令行：-m 模型 [--mmproj 多模态投影] --port 端口
     * （参数由 localServerArgs() 拼，设置面板显示的那条命令行就是它）。
     * Ollama / LM Studio 那种自带服务的，直接用 api 模式连它们的地址就行，
     * 不需要这里启动。
     */
    m_proc->setArguments(localServerArgs());
    m_proc->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_proc, &QProcess::readyReadStandardOutput, this, [this]() {
        if (m_proc)
            appendLocalLog(QString::fromLocal8Bit(m_proc->readAllStandardOutput()));
    });
    /*
     * 启动**不等**（原来这里是 waitForStarted(8000)）：翻译那条路上模型是自动
     * 拉起来的，阻塞 8 秒等于把界面冻住 8 秒。改成听信号：
     *   started        -> 报"模型加载中"，开始轮询 /models；
     *   FailedToStart  -> 把排队等模型的请求按这个原因失败掉；
     *   finished       -> 服务半路没了，同样把排队的请求失败掉（不然它们一直等）。
     */
    connect(m_proc, &QProcess::started, this, [this]() {
        setStatus(QStringLiteral("本地推理服务已启动，模型加载中…"));
        emit localRunningChanged();
        /* 起来之后轮询 /models：模型加载完那个口才通，通了才算就绪 */
        QTimer::singleShot(1200, this, [this]() { pollLocalReady(0); });
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;   /* 别的错误（崩了 / 被杀了）由 finished 那条兜底 */
        const QString reason =
            QStringLiteral("本地推理程序没能启动：%1").arg(m_proc ? m_proc->errorString()
                                                                 : QString());
        setStatus(reason);
        if (m_proc) {
            m_proc->deleteLater();
            m_proc = nullptr;
        }
        emit localRunningChanged();
        failPending(reason);
    });
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                const QString reason =
                    QStringLiteral("本地推理服务退出了（退出码 %1），看设置里的输出").arg(code);
                setStatus(reason);
                emit localRunningChanged();
                failPending(reason);
            });

    setStatus(QStringLiteral("正在启动本地推理服务…"));
    m_proc->start();
    emit localRunningChanged();
    return true;
}

void LlmClient::flushPending() {
    if (m_pending.isEmpty() || !localRunning())
        return;
    /* 先摘干净再发：post() 万一又把某条排回队里，也不会在这儿绕圈 */
    const QList<PendingRequest> waiting = m_pending;
    m_pending.clear();
    for (const PendingRequest &request : waiting) {
        /* image 非空 = 这是一条识别请求（见 PendingRequest 的说明） */
        if (!request.image.isEmpty()) {
            postVision(request.token, request.image, request.target, request.source,
                       request.persona);
        } else if (request.persona == QLatin1String("ask")) {
            /*
             * 自定义提示词那条（见 ask）：排进队时把 system 提示词寄存在
             * source 里（所以它没过 post 那道"翻译"的规矩），重新起来时原样还回去。
             */
            ask(request.source, request.text, QStringLiteral("正在请求模型…"));
        } else {
            post(request.token, request.text, request.target, request.source, request.probe);
        }
    }
}

void LlmClient::failPending(const QString &reason) {
    if (m_pending.isEmpty())
        return;
    const QList<PendingRequest> waiting = m_pending;
    m_pending.clear();
    setBusy(false);
    for (const PendingRequest &request : waiting) {
        /* 和别处一样异步回：QML 那边是"先记 token 再等信号" */
        if (request.probe)
            continue;
        QTimer::singleShot(0, this, [this, token = request.token, reason]() {
            emit failed(token, reason);
        });
    }
}

void LlmClient::pollLocalReady(int attempt) {
    if (!localRunning()) {
        emit localRunningChanged();
        return;
    }
    if (attempt > 90) {
        const QString reason =
            QStringLiteral("本地模型加载超时（模型太大 / 端口被占？看设置里的输出）");
        setStatus(reason);
        failPending(reason);
        return;
    }

    QNetworkRequest request{QUrl(baseUrl() + QStringLiteral("/models"))};
    request.setTransferTimeout(3000);
    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, attempt]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            setStatus(QStringLiteral("本地模型已就绪，可以翻译了"));
            /* 等模型的那条请求（见 post 里的自动启动）：现在可以发了 */
            flushPending();
            return;
        }
        QTimer::singleShot(1500, this, [this, attempt]() { pollLocalReady(attempt + 1); });
    });
}

void LlmClient::stopLocal() {
    if (!m_proc)
        return;
    QProcess *proc = m_proc;
    m_proc = nullptr;
    if (proc->state() != QProcess::NotRunning) {
        proc->kill();
        proc->waitForFinished(3000);
    }
    proc->deleteLater();
    setStatus(QStringLiteral("本地推理服务已停止"));
    emit localRunningChanged();
    failPending(m_status);
}

void LlmClient::shutdown() {
    stopLocal();
}

/* ===========================================================================
 * TranslateCard
 * ======================================================================== */

TranslateCard::TranslateCard(QQmlEngine *engine)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                          | Qt::Tool),
      m_engine(engine),
      m_sourceLang(QStringLiteral("自动检测")),
      m_targetLang(defaultTargetLanguage()) {
    /*
     * 和便签窗口一样：**不设 QWidget 的 parent**（顶层窗口），总管用显式指针管；
     * 置顶 / 无边框 / 不进任务栏都是窗口标志位。
     */
    setObjectName(QStringLiteral("TranslateCard"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
    setMinimumSize(kMinW, kMinH);
    setWindowTitle(QStringLiteral("SmartClip 翻译"));

    /*
     * QQuickWidget 必须挂**主引擎**（总管传进来的那个）：不传它自己 new 一个，
     * 卡片里的 QML 就 import 不到 SmartClip.Globals（Llm / Trans 那些单例）。
     * 见 StickyNoteWindow 构造函数里同一条踩坑记录。
     */
    m_view = m_engine ? new QQuickWidget(m_engine, this) : new QQuickWidget(this);
    m_view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_view->setClearColor(Qt::transparent);

    /*
     * 状态用 setInitialProperties 交给 QML（不是上下文属性）：根对象的属性
     * 一上来就有值，绑定不用等 —— 理由见 StickyNoteWindow 里那段。
     */
    QVariantMap props;
    props.insert(QStringLiteral("cardWin"), QVariant::fromValue<QObject *>(this));
    props.insert(QStringLiteral("textIn"), m_textIn);
    props.insert(QStringLiteral("textOut"), m_textOut);
    props.insert(QStringLiteral("sourceLang"), m_sourceLang);
    props.insert(QStringLiteral("targetLang"), m_targetLang);
    props.insert(QStringLiteral("pinned"), m_pinned);
    props.insert(QStringLiteral("cardNumber"), m_number);
    m_view->setInitialProperties(props);

    m_view->setSource(QUrl(QStringLiteral("qrc:/qt/qml/SmartClip/qml/translate/TranslateCard.qml")));
    if (m_view->status() == QQuickWidget::Error)
        qWarning("翻译卡片界面加载失败：TranslateCard.qml");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_view, 1);

    resize(kDefaultW, kDefaultH);
}

TranslateCard::~TranslateCard() = default;

QObject *TranslateCard::qmlRoot() const {
    return m_view ? m_view->rootObject() : nullptr;
}

QString TranslateCard::qmlError() const {
    if (!m_view)
        return QStringLiteral("没有 QQuickWidget");
    if (m_view->status() != QQuickWidget::Error)
        return QString();
    const QList<QQmlError> errors = m_view->errors();
    if (errors.isEmpty())
        return QStringLiteral("QML 加载失败（没有细节）");
    return errors.first().toString();
}

void TranslateCard::pushToQml(const char *name, const QVariant &value) {
    QObject *root = qmlRoot();
    if (!root)
        return;
    if (root->property(name) == value)
        return;
    root->setProperty(name, value);
}

void TranslateCard::setTextIn(const QString &text) {
    if (m_textIn == text)
        return;
    m_textIn = text;
    pushToQml("textIn", text);
    emit textInChanged();
    emit stateChanged();
}

void TranslateCard::setTextOut(const QString &text) {
    if (m_textOut == text)
        return;
    m_textOut = text;
    pushToQml("textOut", text);
    emit textOutChanged();
    emit stateChanged();
}

void TranslateCard::setSourceLang(const QString &lang) {
    if (m_sourceLang == lang || lang.isEmpty())
        return;
    m_sourceLang = lang;
    pushToQml("sourceLang", lang);
    emit languagesChanged();
    emit stateChanged();
}

void TranslateCard::setTargetLang(const QString &lang) {
    if (m_targetLang == lang || lang.isEmpty())
        return;
    m_targetLang = lang;
    pushToQml("targetLang", lang);
    emit languagesChanged();
    emit stateChanged();
}

void TranslateCard::setPinned(bool on) {
    if (m_pinned == on)
        return;
    m_pinned = on;
    pushToQml("pinned", on);
    applyWindowFlags();
    emit pinnedChanged();
    emit stateChanged();
}

void TranslateCard::applyWindowFlags() {
    const Qt::WindowFlags flags =
        Qt::Window | Qt::FramelessWindowHint | Qt::Tool
        | (m_pinned ? Qt::WindowStaysOnTopHint : Qt::WindowFlags());
    if (windowFlags() == flags)
        return;

    /*
     * 改标志位会把原生窗口拆掉重建，几何要自己收好再放回去 ——
     * 不收的话关掉"置顶"那一刻卡片会跳回左上角（便签那边同一个坑）。
     */
    const bool wasVisible = isVisible();
    const QRect geo = geometry();
    setWindowFlags(flags);
    setGeometry(geo);
    if (wasVisible)
        show();
}

void TranslateCard::togglePinned() {
    setPinned(!m_pinned);
}

QRect TranslateCard::screenBounds() const {
    const QScreen *screen = QWidget::screen();
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return QRect(0, 0, 1920, 1080);
    return screen->availableGeometry();
}

void TranslateCard::beginDrag() {
    /* 拖动交给窗口管理器（贴边吸附 / 多屏 DPI 都归它管），和便签一个做法 */
    if (windowHandle())
        windowHandle()->startSystemMove();
}

void TranslateCard::beginResize() {
    if (windowHandle())
        windowHandle()->startSystemResize(Qt::BottomEdge | Qt::RightEdge);
}

void TranslateCard::closeCard() {
    /* 藏起来（不是删）：状态还在，图标条 / 托盘能再叫出来 */
    hide();
    emit closed();
}

void TranslateCard::copyResult() {
    if (m_textOut.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(m_textOut);
}

void TranslateCard::swapLanguages() {
    if (m_sourceLang == QStringLiteral("自动检测")) {
        /* 源语言是"自动"时没有可对调的东西，就把目标语言抄过去 */
        setSourceLang(m_targetLang);
        setTargetLang(defaultTargetLanguage());
        return;
    }
    const QString oldSource = m_sourceLang;
    setSourceLang(m_targetLang);
    setTargetLang(oldSource);
}

void TranslateCard::clearAll() {
    setTextIn(QString());
    setTextOut(QString());
}

void TranslateCard::moveEvent(QMoveEvent *event) {
    QWidget::moveEvent(event);
    if (m_placing)
        return;
    emit stateChanged();
}

void TranslateCard::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_placing)
        return;
    emit stateChanged();
}

void TranslateCard::closeEvent(QCloseEvent *event) {
    /* 右上角那个 ✕ 走的是 closeCard()；窗口自己的关闭（Alt+F4）也按"藏起来"算 */
    QWidget::closeEvent(event);
    emit closed();
}

/* ===========================================================================
 * TranslateCards
 * ======================================================================== */

TranslateCards::TranslateCards(LlmClient *llm, QQmlEngine *engine, QObject *parent)
    : QObject(parent), m_llm(llm), m_engine(engine) {
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(500);
    connect(m_saveTimer, &QTimer::timeout, this, [this]() { save(); });
}

TranslateCards::~TranslateCards() = default;

void TranslateCards::attachEngine(QQmlEngine *engine) {
    if (engine)
        m_engine = engine;
}

TranslateCard *TranslateCards::ensureCard() {
    if (m_card)
        return m_card;

    auto *card = new TranslateCard(m_engine);
    card->m_number = m_nextNumber++;
    m_card = card;

    /* 卡片里的状态改了就排一次落盘（和便签 store 的防抖一个做法） */
    connect(card, &TranslateCard::stateChanged, this, &TranslateCards::scheduleSave);
    connect(card, &TranslateCard::closed, this, &TranslateCards::onCardClosed);

    /* 默认目标语言由 LLM 那份配置说了算（设置面板里改的就是它） */
    if (m_llm)
        card->setTargetLang(m_llm->defaultTarget());

    /*
     * 存档在**建卡片的时候就恢复**，不是"叫出来的时候"：用户把卡片收起来、
     * 下次再点图标条，看到的该是上次那份正文和语言，而不是一张白纸。
     */
    restore(card);
    emit changed();
    return card;
}

void TranslateCards::restore(TranslateCard *card) {
    if (!card)
        return;

    QSettings settings;
    const QString prefix = QString::fromLatin1(kKeyCard);

    /* 头一次用（没有存档）：给一个不挡事的位置 —— 屏幕右上角往里缩一点 */
    if (!settings.contains(prefix + QStringLiteral("geometry"))) {
        const QRect work = card->screenBounds();
        card->m_placing = true;
        card->setGeometry(QRect(work.right() - kDefaultW - 24, work.top() + 24,
                                kDefaultW, kDefaultH));
        card->m_placing = false;
        return;
    }

    /* 存档 -> 界面：一律走 setter，它们会顺手推给 QML 根对象（见 pushToQml） */
    card->setTextIn(settings.value(prefix + QStringLiteral("textIn")).toString());
    card->setTextOut(settings.value(prefix + QStringLiteral("textOut")).toString());
    card->setSourceLang(settings.value(prefix + QStringLiteral("sourceLang"),
                                       QStringLiteral("自动检测")).toString());
    card->setTargetLang(settings.value(prefix + QStringLiteral("targetLang"),
                                       defaultTargetLanguage()).toString());
    card->setPinned(settings.value(prefix + QStringLiteral("pinned"), true).toBool());

    QRect geo = settings.value(prefix + QStringLiteral("geometry")).toRect();
    geo.setSize(geo.size().expandedTo(QSize(kMinW, kMinH)));

    /*
     * 屏幕拔掉 / 分辨率改了之后，旧坐标可能整块落在屏幕外 —— 那样卡片就成了
     * "任务栏里看得见、桌面上找不到"。夹回工作区（和便签一个做法）。
     */
    const QRect work = card->screenBounds();
    if (!work.intersects(geo))
        geo.moveTopLeft(QPoint(work.right() - geo.width() - 24, work.top() + 24));

    card->m_placing = true;
    card->setGeometry(geo);
    card->m_placing = false;
}

void TranslateCards::start() {
    /*
     * 只有上次退出时卡片是摆着的才自动摆回来。用户主动收起来的（可见 = false）
     * 不自己冒出来 —— 点图标条 / 托盘时会按上次那份状态把它叫回来。
     *
     * 这里**只 show、不 activate**（showCard() 会 activateWindow）：启动时焦点
     * 该留在主窗口上。卡片去抢激活会把主窗口那边的弹窗顺手顶掉 —— 那些弹窗
     * （"有未保存改动"的 AskCard 之类）是 Qt::Popup，一失去激活就自己收，
     * 自检里先红的就是那两条（"未保存问句：有改动，卡片弹出来了"）。
     */
    if (!QSettings().value(QString::fromLatin1(kKeyCard) + QStringLiteral("visible"), false)
             .toBool())
        return;

    TranslateCard *card = ensureCard();
    if (!card)
        return;

    card->show();
    emit changed();
}

void TranslateCards::shutdown() {
    if (m_saveTimer)
        m_saveTimer->stop();
    if (m_card)
        save();
}

TranslateCard *TranslateCards::showCard() {
    TranslateCard *card = ensureCard();
    if (!card)
        return nullptr;

    card->show();
    card->raise();
    card->activateWindow();
    emit changed();
    return card;
}

void TranslateCards::hideCard() {
    if (m_card)
        m_card->closeCard();
}

void TranslateCards::onCardClosed() {
    save();
    emit changed();
}

int TranslateCards::visibleCount() const {
    return (m_card && m_card->isVisible()) ? 1 : 0;
}

void TranslateCards::scheduleSave() {
    if (m_saveTimer)
        m_saveTimer->start();
}

void TranslateCards::save() const {
    if (!m_card)
        return;

    QSettings settings;
    const QString prefix = QString::fromLatin1(kKeyCard);
    settings.setValue(prefix + QStringLiteral("visible"), m_card->isVisible());
    settings.setValue(prefix + QStringLiteral("geometry"), m_card->geometry());
    settings.setValue(prefix + QStringLiteral("textIn"), m_card->textIn());
    settings.setValue(prefix + QStringLiteral("textOut"), m_card->textOut());
    settings.setValue(prefix + QStringLiteral("sourceLang"), m_card->sourceLang());
    settings.setValue(prefix + QStringLiteral("targetLang"), m_card->targetLang());
    settings.setValue(prefix + QStringLiteral("pinned"), m_card->pinned());
}
