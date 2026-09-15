#include "SelfTest.h"

#include "Translate.h"
#include "TrayIcon.h"

#include <QAction>
#include <QBuffer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMetaObject>
#include <QQuickItem>
#include <QScreen>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <cstdio>

/*
 * 翻译专用自检（`SmartClip.exe --translate-test`，见 SelfTest.h 的 runTranslate）。
 *
 * 和便签那份（src/SelfTestNotes.cpp）分开，理由一样：SelfTest.cpp 里那几千行
 * 检查会开截图选区窗口、弹卡片，跑一趟十几秒还有自己的时序问题。动翻译的时候
 * 只需要跑这一份。
 *
 * 这里**不发真请求**（机器上多半没有配好的模型，跑一趟只会在日志里留一串网络
 * 错误）。验的是那几件容易坏、坏了又不容易发现的事：
 *   * 卡片界面（TranslateCard.qml）真的加载起来了，两个框 / 语言 / 翻译按钮都在；
 *   * 界面和 C++ 那份状态是**双向**通的（用户敲的字进得来，存档推得回去）；
 *   * 语言表两份对得上；
 *   * 发请求那套的 token 契约：失败也是**异步**回来的，而且带对 token
 *     （QML 那边是"先拿到 token 记下来、再等信号"，同步回就等于丢结果）；
 *   * 收起来 / 再叫出来 / 落盘；
 *   * 截图识别：识别用哪个模型（留空退回主模型）、图片按多模态格式发出去、
 *     回来的"原文 ---- 译文"拆得开（见第 10 节）。
 *
 * 自检会动 QSettings 里 translate/card/* 和 translate/apiBase 这几个键（要试
 * 状态和失败路径），跑完**按原样写回** —— 用户自己的配置不会被自检改掉。
 */

namespace {

int gTranslatePassed = 0;
int gTranslateFailed = 0;

void trCheck(bool ok, const QString &what, const QString &detail = QString()) {
    if (ok) {
        ++gTranslatePassed;
        std::fputs("  ok    ", stdout);
    } else {
        ++gTranslateFailed;
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

void trOut(const QString &line) {
    std::fputs("        ", stdout);
    std::fputs(line.toUtf8().constData(), stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

/* 让事件处理一轮：原生窗口几何 / QML 绑定都是下一帧才落定的 */
void settle() {
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
}

/* 从 QML 根对象上找一个具名子项（界面上的关键控件都带 objectName） */
QObject *qmlChild(QObject *root, const char *name) {
    return root ? root->findChild<QObject *>(QString::fromLatin1(name)) : nullptr;
}

/*
 * 一张图 -> png 的 data URL。
 *
 * 和 Screenshot::selectionImage 编出来的**必须是同一种东西**（那边真的去裁
 * 屏幕上那块选区，自检没有真截图，只能自己造一张）。所以这里也走
 * QImage::save(&buffer, "PNG") + toBase64 —— 格式对不上的话，这一节测出来的
 * "请求里带了图"就是假的。
 */
QString imageToDataUrl(const QImage &image) {
    if (image.isNull())
        return QString();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return QString();
    buffer.close();
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}

/*
 * 一个假的大模型服务（本地回环上随机端口），只回答一次。
 *
 * 为什么自检要自己搭一个：真请求需要一台配好的模型服务，跑一趟自检就得联网，
 * 而且回什么由对面说了算，检查会飘。这里用一个最小的 OpenAI 兼容应答，把
 * **请求长什么样 / 回来的东西认不认得出来**这两件事钉住 —— 这正是翻译这条链路
 * 里最容易悄悄坏掉的一段（提示词里没有目标语言、Authorization 没带上、
 * choices[0].message.content 读错层级…都不会崩，只是"翻译结果不对"）。
 */
class MockLlmServer final : public QTcpServer {
public:
    /* 收到的那份请求（头和体拼在一起，检查里按子串找） */
    QString request;
    /* 原始字节（分帧按它算，见下面那段说明） */
    QByteArray raw;
    /* 每一步的判断（自检失败时打出来看，见下） */
    QString trace;
    /* 回给客户端的那句译文 */
    QString replyContent = QStringLiteral("你好，世界");

    explicit MockLlmServer(QObject *parent = nullptr) : QTcpServer(parent) {}

protected:
    void incomingConnection(qintptr socketDescriptor) override {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(socketDescriptor);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            const QByteArray chunk = socket->readAll();
            raw += chunk;
            request = QString::fromUtf8(raw);
            if (request.contains(QStringLiteral("__answered")))
                return;

            /*
             * 分帧一律在**字节**上算：Content-Length 和头部偏移都是字节数，
             * 拿 QString::size()（UTF-16 字符数）去比，正文里有中文时永远等不齐
             * （自检第一版就踩了这个：455 个字符 vs 601 个字节）。
             */
            const int headerEnd = raw.indexOf("\r\n\r\n");
            const int headerEndLf = headerEnd < 0 ? raw.indexOf("\n\n") : -1;
            const int sep = headerEnd >= 0 ? headerEnd : headerEndLf;
            const int sepLen = headerEnd >= 0 ? 4 : 2;
            if (sep < 0) {
                trace += QStringLiteral("[+%1 no-header-end]").arg(chunk.size());
                return;
            }
            /* 等 body 收齐（Content-Length 说了多少就是多少）再回 */
            int length = 0;
            const int at = raw.indexOf("Content-Length:", 0);
            if (at < 0) {
                const QByteArray lower = raw.toLower();
                const int lowerAt = lower.indexOf("content-length:");
                if (lowerAt >= 0) {
                    const int eol = raw.indexOf('\n', lowerAt);
                    length = raw.mid(lowerAt + 15, eol - lowerAt - 15).trimmed().toInt();
                }
            } else {
                const int eol = raw.indexOf('\n', at);
                length = raw.mid(at + 15, eol - at - 15).trimmed().toInt();
            }
            trace += QStringLiteral("[+%1 total=%2 sep=%3 len=%4]")
                         .arg(chunk.size())
                         .arg(raw.size())
                         .arg(sep)
                         .arg(length);
            if (raw.size() < sep + sepLen + length)
                return;

            request += QStringLiteral("__answered");

            QJsonObject message{{QStringLiteral("role"), QStringLiteral("assistant")},
                                {QStringLiteral("content"), replyContent}};
            QJsonObject choice{{QStringLiteral("index"), 0},
                               {QStringLiteral("message"), message}};
            QJsonObject body{{QStringLiteral("choices"), QJsonArray{choice}}};
            const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

            QByteArray response = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Connection: close\r\n"
                                  "Content-Length: ";
            response += QByteArray::number(payload.size());
            response += "\r\n\r\n";
            response += payload;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
        });
    }
};

}  // namespace

bool SelfTest::translateTestEnabled(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--translate-test"))
            return true;
    }
    return false;
}

int SelfTest::translatePassed() { return gTranslatePassed; }
int SelfTest::translateFailed() { return gTranslateFailed; }

int SelfTest::runTranslate(TranslateCards *cards, LlmClient *llm, TrayIcon *tray) {
    /* 每条检查立刻落盘：崩了也能看到崩在哪一条 */
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (!cards || !llm) {
        trOut(QStringLiteral("翻译对象为空，没法测"));
        return 1;
    }

    /*
     * 先把可能要改的那几个键存下来，跑完写回去（见文件头）。
     * geometry / visible 由 showCard + hideCard 走一遍，也一起还原。
     */
    QSettings settings;
    const QString cardPrefix = QStringLiteral("translate/card/");
    const QStringList savedKeys{
        cardPrefix + QStringLiteral("textIn"),     cardPrefix + QStringLiteral("textOut"),
        cardPrefix + QStringLiteral("sourceLang"), cardPrefix + QStringLiteral("targetLang"),
        cardPrefix + QStringLiteral("pinned"),     cardPrefix + QStringLiteral("geometry"),
        cardPrefix + QStringLiteral("visible"),
        /* 本地模型那几项：下面"本地模型"那一节会动它们（见那一段的说明） */
        QStringLiteral("translate/localExe"),    QStringLiteral("translate/localModel"),
        QStringLiteral("translate/localMmproj"), QStringLiteral("translate/localPort"),
        /* 识别模型：下面"截图识别"那一节会动它（见那一段的说明） */
        QStringLiteral("translate/ocrModel"),
    };
    QHash<QString, QVariant> saved;
    for (const QString &key : savedKeys)
        saved.insert(key, settings.value(key));
    const QString savedBase = llm->apiBase();
    const QString savedMode = llm->mode();
    const QString savedModel = llm->model();
    const QString savedKey = llm->apiKey();
    const bool hadBase = settings.contains(QStringLiteral("translate/apiBase"));
    const bool hadModel = settings.contains(QStringLiteral("translate/model"));
    const bool hadKey = settings.contains(QStringLiteral("translate/apiKey"));

    trOut(QStringLiteral("翻译自检（只测翻译，不碰编辑区 / 截图，也不发真请求）"));

    /* =====================================================================
     * 1) 卡片建得出来、界面加载得起来
     * =================================================================== */
    TranslateCard *card = cards->showCard();
    settle();

    trCheck(card != nullptr, QStringLiteral("卡片：叫得出来"));
    if (!card) {
        trOut(QStringLiteral("卡片没建出来，后面的检查没法做"));
        return gTranslateFailed;
    }

    trCheck(card->qmlError().isEmpty(), QStringLiteral("卡片：界面加载没有报错"),
            card->qmlError());
    trCheck(card->isVisible(), QStringLiteral("卡片：叫出来之后是摆着的"));

    /* 摆的位置要落在那块屏的工作区里（不然就是"桌面上找不到"） */
    const QRect bounds = card->screenBounds();
    trCheck(bounds.intersects(card->geometry()),
            QStringLiteral("卡片：落在屏幕工作区里"),
            QStringLiteral("%1,%2 %3x%4")
                .arg(card->geometry().x()).arg(card->geometry().y())
                .arg(card->geometry().width()).arg(card->geometry().height()));

    /* =====================================================================
     * 2) 用户要的那几样控件都在：两个框、语言选择、翻译按钮
     * =================================================================== */
    QObject *root = card->qmlRoot();
    QObject *input = qmlChild(root, "translateInput");
    QObject *output = qmlChild(root, "translateOutput");
    QObject *runBtn = qmlChild(root, "translateRun");
    QObject *srcBtn = qmlChild(root, "translateSource");
    QObject *tgtBtn = qmlChild(root, "translateTarget");

    trCheck(root != nullptr, QStringLiteral("界面：根对象拿得到"));
    trCheck(input != nullptr, QStringLiteral("界面：上面那个输入框在"));
    trCheck(output != nullptr, QStringLiteral("界面：下面那个译文框在"));
    trCheck(runBtn != nullptr, QStringLiteral("界面：翻译按钮在"));
    trCheck(srcBtn != nullptr && tgtBtn != nullptr, QStringLiteral("界面：源 / 目标语言选择在"));

    /* =====================================================================
     * 3) 两份状态双向通：界面敲的字进得来，存档推得回界面
     * =================================================================== */
    if (input) {
        input->setProperty("text", QStringLiteral("界面里敲的字"));
        settle();
        trCheck(card->textIn() == QStringLiteral("界面里敲的字"),
                QStringLiteral("状态：界面里敲的字回到了 C++ 那份（要落盘的就是它）"),
                card->textIn());
    }
    if (input) {
        card->setTextIn(QStringLiteral("存档推回来的字"));
        settle();
        trCheck(input->property("text").toString() == QStringLiteral("存档推回来的字"),
                QStringLiteral("状态：C++ 推回去之后输入框跟着变（重启恢复走这条路）"),
                input->property("text").toString());
    }

    card->setTextOut(QStringLiteral("译文占位"));
    card->setSourceLang(QStringLiteral("英语"));
    card->setTargetLang(QStringLiteral("日语"));
    settle();
    if (output) {
        trCheck(output->property("text").toString() == QStringLiteral("译文占位"),
                QStringLiteral("状态：译文框显示 C++ 那份译文"),
                output->property("text").toString());
    }
    trCheck(card->sourceLang() == QStringLiteral("英语")
                && card->targetLang() == QStringLiteral("日语"),
            QStringLiteral("状态：源 / 目标语言改得动"));

    /* 对调：源 <-> 目标（"自动检测"那条特殊路不在这里测，见 swapLanguages） */
    card->swapLanguages();
    trCheck(card->sourceLang() == QStringLiteral("日语")
                && card->targetLang() == QStringLiteral("英语"),
            QStringLiteral("状态：对调把源 / 目标换了个个儿"),
            card->sourceLang() + QStringLiteral(" -> ") + card->targetLang());

    /* 清空 */
    card->clearAll();
    trCheck(card->textIn().isEmpty() && card->textOut().isEmpty(),
            QStringLiteral("状态：清空把两个框都清掉"));

    /* =====================================================================
     * 4) 语言表：两份对得上，"自动检测"不能当目标语言
     * =================================================================== */
    const QStringList langs = llm->languages();
    const QStringList targets = llm->targetLanguages();
    trCheck(langs.size() >= 10, QStringLiteral("语言：清单有十来种"),
            QString::number(langs.size()));
    trCheck(langs.value(0) == QStringLiteral("自动检测"),
            QStringLiteral("语言：第一项是自动检测（只当源语言）"), langs.value(0));
    trCheck(targets.size() == langs.size() - 1
                && !targets.contains(QStringLiteral("自动检测")),
            QStringLiteral("语言：目标语言里没有自动检测"),
            QString::number(targets.size()));

    /* =====================================================================
     * 5) 请求那套的契约：失败也是**异步**回来的，而且带对 token
     *
     * 输入空的时候 translate() 会立刻判失败 —— 但它必须推到事件循环下一轮再发
     * 信号：QML 那边是 `pendingToken = Llm.translate(...)`，同步发就等于
     * "信号跑在 token 记上之前"，界面上是点了翻译什么也没发生。
     * =================================================================== */
    {
        QString gotToken;
        QString gotError;
        bool arrivedInsideCall = false;
        bool inCall = true;

        QObject probe;
        QObject::connect(llm, &LlmClient::failed, &probe,
                         [&](const QString &token, const QString &error) {
                             if (inCall)
                                 arrivedInsideCall = true;
                             gotToken = token;
                             gotError = error;
                         });

        const QString token = llm->translate(QString(), QStringLiteral("英语"));
        trCheck(!token.isEmpty(), QStringLiteral("请求：translate() 给了一个 token"), token);
        inCall = false;
        trCheck(!arrivedInsideCall,
                QStringLiteral("请求：失败不能同步回（QML 要先记下 token）"));

        QEventLoop loop;
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        QObject::connect(llm, &LlmClient::failed, &loop, &QEventLoop::quit);
        loop.exec();
        settle();

        trCheck(gotToken == token && !gotError.isEmpty(),
                QStringLiteral("请求：失败异步回来了，而且 token 对得上"),
                gotToken + QStringLiteral(" / ") + gotError);
    }

    /* =====================================================================
     * 6) 真发一次请求：对着一台**假模型服务**（本地回环，见 MockLlmServer）
     *
     * 这一节验的是翻译这条链路的正路：请求里带对模型名 / 密钥 / 目标语言，
     * 回来的 choices[0].message.content 认得出、而且带对 token。
     * =================================================================== */
    {
        MockLlmServer mock;
        const bool listening = mock.listen(QHostAddress::LocalHost, 0);
        trCheck(listening, QStringLiteral("请求：假模型服务起得来（本地回环随机端口）"));

        if (listening) {
            llm->setMode(QStringLiteral("api"));
            llm->setApiBase(QStringLiteral("http://127.0.0.1:%1/v1").arg(mock.serverPort()));
            llm->setModel(QStringLiteral("自检模型"));
            llm->setApiKey(QStringLiteral("自检密钥"));

            QString gotToken;
            QString gotText;
            QString gotError;
            QObject probe;
            QObject::connect(llm, &LlmClient::finished, &probe,
                             [&](const QString &token, const QString &text) {
                                 gotToken = token;
                                 gotText = text;
                             });
            QObject::connect(llm, &LlmClient::failed, &probe,
                             [&](const QString &, const QString &error) { gotError = error; });

            /* 中文名（源语言）和"中文（简体）"（目标）都要进提示词 */
            const QString token = llm->translate(QStringLiteral("Hello world"),
                                                 QStringLiteral("中文（简体）"),
                                                 QStringLiteral("英语"));

            QEventLoop loop;
            QTimer::singleShot(5000, &loop, &QEventLoop::quit);
            QObject::connect(llm, &LlmClient::finished, &loop, &QEventLoop::quit);
            QObject::connect(llm, &LlmClient::failed, &loop, &QEventLoop::quit);
            loop.exec();
            settle();

            trCheck(gotText == mock.replyContent && gotToken == token,
                    QStringLiteral("请求：译文原样回来了，token 也对得上"),
                    (gotError.isEmpty() ? gotToken + QStringLiteral(" / ") + gotText : gotError)
                        + QStringLiteral("  [answered=%1 req=%2 %3]")
                              .arg(mock.request.contains(QStringLiteral("__answered")))
                              .arg(mock.request.size())
                              .arg(mock.trace));

            const QString sent = mock.request;
            trCheck(sent.contains(QStringLiteral("自检模型")),
                    QStringLiteral("请求：带上了模型名"));
            trCheck(sent.contains(QStringLiteral("Bearer 自检密钥")),
                    QStringLiteral("请求：带上了密钥（Authorization: Bearer …）"));
            trCheck(sent.contains(QStringLiteral("chat/completions")),
                    QStringLiteral("请求：打的是 OpenAI 兼容的 chat/completions"));
            trCheck(sent.contains(QStringLiteral("中文（简体）")) && sent.contains(QStringLiteral("Hello world")),
                    QStringLiteral("请求：提示词里有目标语言，正文也带上了"));
            trCheck(sent.contains(QStringLiteral("system")) && sent.contains(QStringLiteral("user")),
                    QStringLiteral("请求：是一问一答两条消息（system 定翻译规矩）"));
        }
    }

    /* =====================================================================
     * 7) 本地模型：mmproj 参数 + "用到的时候自己启动"
     * =================================================================== */
    {
        const QString oldExe = llm->localExe();
        const QString oldModel = llm->localModel();
        const QString oldMmproj = llm->localMmproj();
        const int oldPort = llm->localPort();
        const QString oldMode = llm->mode();

        llm->setLocalExe(QStringLiteral("C:/tools/llama-server.exe"));
        llm->setLocalModel(QStringLiteral("C:/models/qwen2-vl-2b.gguf"));
        llm->setLocalPort(8080);

        /* 纯文本模型：不该带 --mmproj */
        llm->setLocalMmproj(QString());
        const QStringList plain = llm->localServerArgs();
        trCheck(plain.contains(QStringLiteral("C:/models/qwen2-vl-2b.gguf"))
                    && plain.contains(QStringLiteral("8080")),
                QStringLiteral("本地模型：启动参数里有模型文件和端口"),
                plain.join(QLatin1Char(' ')));
        trCheck(!plain.contains(QStringLiteral("--mmproj")),
                QStringLiteral("本地模型：没填 mmproj 时**不**加这个参数"));

        /* 多模态：填了就要带 --mmproj <文件> */
        llm->setLocalMmproj(QStringLiteral("C:/models/mmproj-model-f16.gguf"));
        const QStringList vision = llm->localServerArgs();
        const int mmAt = vision.indexOf(QStringLiteral("--mmproj"));
        trCheck(mmAt >= 0 && vision.value(mmAt + 1) == QStringLiteral("C:/models/mmproj-model-f16.gguf"),
                QStringLiteral("本地模型：填了 mmproj 就带 --mmproj 和那个文件"),
                vision.join(QLatin1Char(' ')));
        trCheck(llm->localCommand().contains(QStringLiteral("--mmproj")),
                QStringLiteral("本地模型：设置里显示的命令行和真启动的是同一份"),
                llm->localCommand());

        /*
         * 自动启动：本地模式 + 还没起来时点翻译，**不该**回"还没启动"，
         * 而是自己去拉起服务（这里故意给一个不存在的程序，看它报的是"启动失败"）。
         */
        llm->setMode(QStringLiteral("local"));
        llm->setLocalExe(QStringLiteral("C:/__self_test__/not-there/llama-server.exe"));

        QString gotError;
        QString gotToken;
        QObject probe;
        QObject::connect(llm, &LlmClient::failed, &probe,
                         [&](const QString &token, const QString &error) {
                             gotToken = token;
                             gotError = error;
                         });

        const QString token = llm->translate(QStringLiteral("你好"), QStringLiteral("英语"));
        QEventLoop loop;
        QTimer::singleShot(6000, &loop, &QEventLoop::quit);
        QObject::connect(llm, &LlmClient::failed, &loop, &QEventLoop::quit);
        loop.exec();
        settle();

        trCheck(gotToken == token && gotError.contains(QStringLiteral("启动")),
                QStringLiteral("本地模型：没启动时点翻译会自己去启动（报的是启动失败）"),
                gotToken + QStringLiteral(" / ") + gotError);
        trCheck(!gotError.contains(QStringLiteral("还没启动")),
                QStringLiteral("本地模型：不再要求用户先去设置里手动点启动"), gotError);

        /* 还原（配置由最后那段统一写回） */
        llm->setMode(oldMode);
        llm->setLocalExe(oldExe);
        llm->setLocalModel(oldModel);
        llm->setLocalMmproj(oldMmproj);
        llm->setLocalPort(oldPort);
    }

    /* =====================================================================
     * 8) 收起来 / 再叫出来 / 落盘
     * =================================================================== */
    card->setTextIn(QStringLiteral("自检写的正文"));
    card->setTargetLang(QStringLiteral("英语"));
    cards->hideCard();
    settle();
    trCheck(cards->visibleCount() == 0, QStringLiteral("收起：卡片从桌面上收掉了"));

    cards->shutdown();  /* 落盘（正常退出时走的就是它） */
    QSettings check;
    trCheck(check.value(cardPrefix + QStringLiteral("textIn")).toString()
                == QStringLiteral("自检写的正文"),
            QStringLiteral("落盘：正文写进了 translate/card/textIn"),
            check.value(cardPrefix + QStringLiteral("textIn")).toString());
    trCheck(check.value(cardPrefix + QStringLiteral("targetLang")).toString()
                == QStringLiteral("英语"),
            QStringLiteral("落盘：目标语言写进去了"));
    trCheck(check.value(cardPrefix + QStringLiteral("visible")).toBool() == false,
            QStringLiteral("落盘：收起来的状态写进去了（重启不会自己冒出来）"));

    TranslateCard *again = cards->showCard();
    settle();
    trCheck(again == card, QStringLiteral("再叫出来：还是那一张卡片"));
    trCheck(cards->visibleCount() == 1, QStringLiteral("再叫出来：桌面上又有了"));
    trCheck(card->textIn() == QStringLiteral("自检写的正文"),
            QStringLiteral("再叫出来：正文还是刚才那份（不是白纸）"), card->textIn());

    /* =====================================================================
     * 9) 托盘里那条入口（自检从外面点不到托盘图标，只能看菜单）
     * =================================================================== */
    if (tray) {
        bool found = false;
        const QList<QAction *> actions = tray->menu()->actions();
        for (QAction *action : actions) {
            if (action->text() == QStringLiteral("翻译卡片"))
                found = true;
        }
        trCheck(found, QStringLiteral("托盘：菜单里有「翻译卡片」那一条"));
    }

    /* =====================================================================
     * 10) 截图识别：把框选那块图交给视觉模型，回来的文字摆进卡片
     *
     * 这一节和上面那几节一样**不发真请求**（真识别要一台配好的视觉模型）：用
     * 本地回环上的假服务把"请求长什么样 / 回来的东西认不认得出来"钉住。
     * 这条链路里最容易悄悄坏掉的正是这两头 —— 图片没按多模态格式发出去
     * （模型回一句"我没有收到图片"）、模型名还是那个纯文本的、回来的
     * "原文 ---- 译文"没拆开，都不会崩，只是"识别结果不对"。
     * =================================================================== */
    {
        /* ---- 10.1 识别用哪个模型：留空退回主模型 ---- */
        llm->setModel(QStringLiteral("自检文本模型"));
        llm->setOcrModel(QString());
        trCheck(llm->visionModel() == QStringLiteral("自检文本模型"),
                QStringLiteral("识别：识别模型留空时用主模型（只配了一个视觉模型不用填两遍）"),
                llm->visionModel());

        llm->setOcrModel(QStringLiteral("自检视觉模型"));
        trCheck(llm->visionModel() == QStringLiteral("自检视觉模型"),
                QStringLiteral("识别：填了识别模型就用它（翻译仍走上面那个主模型）"),
                llm->visionModel());
        trCheck(llm->model() == QStringLiteral("自检文本模型"),
                QStringLiteral("识别：填识别模型不会把翻译那个模型也改掉"), llm->model());

        /* ---- 10.2 空图 / 没配模型：异步回失败，token 对得上 ---- */
        {
            QString gotToken;
            QString gotError;
            bool arrivedInsideCall = false;
            bool inCall = true;
            QObject probe;
            QObject::connect(llm, &LlmClient::failed, &probe,
                             [&](const QString &token, const QString &error) {
                                 if (inCall)
                                     arrivedInsideCall = true;
                                 gotToken = token;
                                 gotError = error;
                             });

            const QString token = llm->recognize(QString());
            trCheck(!token.isEmpty(), QStringLiteral("识别：recognize() 给了一个 token"), token);
            inCall = false;
            trCheck(!arrivedInsideCall,
                    QStringLiteral("识别：空图的失败也不能同步回（界面要先记下 token）"));

            QEventLoop loop;
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            QObject::connect(llm, &LlmClient::failed, &loop, &QEventLoop::quit);
            loop.exec();
            settle();
            trCheck(gotToken == token && gotError.contains(QStringLiteral("图像")),
                    QStringLiteral("识别：没拿到图时异步回一句人话（不发空请求出去）"),
                    gotToken + QStringLiteral(" / ") + gotError);
        }

        /* ---- 10.3 真发一次：对着一台假视觉服务 ---- */
        MockLlmServer mock;
        mock.replyContent = QStringLiteral("Hello world\n----\n你好，世界");
        const bool listening = mock.listen(QHostAddress::LocalHost, 0);
        trCheck(listening, QStringLiteral("识别：假视觉服务起得来（本地回环随机端口）"));

        if (listening) {
            llm->setMode(QStringLiteral("api"));
            llm->setApiBase(QStringLiteral("http://127.0.0.1:%1/v1").arg(mock.serverPort()));
            llm->setModel(QStringLiteral("自检文本模型"));
            llm->setOcrModel(QStringLiteral("自检视觉模型"));
            llm->setApiKey(QStringLiteral("自检密钥"));

            /* 一张真图（png 的 data URL 就是界面那边真正会发的东西） */
            QImage image(80, 40, QImage::Format_ARGB32);
            image.fill(QColor(0x2b, 0x2d, 0x30));
            const QString imageUrl = imageToDataUrl(image);
            trCheck(imageUrl.startsWith(QStringLiteral("data:image/png;base64,")),
                    QStringLiteral("识别：选区那块图编码成了 png 的 data URL"),
                    imageUrl.left(32));

            QString gotToken;
            QString gotText;
            QString gotError;
            QObject probe;
            QObject::connect(llm, &LlmClient::finished, &probe,
                             [&](const QString &token, const QString &text) {
                                 gotToken = token;
                                 gotText = text;
                             });
            QObject::connect(llm, &LlmClient::failed, &probe,
                             [&](const QString &, const QString &error) { gotError = error; });

            const QString target = QStringLiteral("中文（简体）");
            const QString token = llm->recognize(imageUrl, target, QStringLiteral("自动检测"));

            QEventLoop loop;
            QTimer::singleShot(5000, &loop, &QEventLoop::quit);
            QObject::connect(llm, &LlmClient::finished, &loop, &QEventLoop::quit);
            QObject::connect(llm, &LlmClient::failed, &loop, &QEventLoop::quit);
            loop.exec();
            settle();

            trCheck(gotText == mock.replyContent && gotToken == token,
                    QStringLiteral("识别：模型回的文字原样回来了，token 也对得上"),
                    (gotError.isEmpty() ? gotToken + QStringLiteral(" / ") + gotText : gotError)
                        + QStringLiteral("  [answered=%1 req=%2]")
                              .arg(mock.request.contains(QStringLiteral("__answered")))
                              .arg(mock.request.size()));

            const QString sent = mock.request;
            trCheck(sent.contains(QStringLiteral("自检视觉模型")),
                    QStringLiteral("识别：请求里带的是**识别模型**的名字"));
            trCheck(sent.contains(imageUrl),
                    QStringLiteral("识别：图片按 data URL 内联发出去了（不是只发了句提示词）"));
            trCheck(sent.contains(QStringLiteral("image_url")),
                    QStringLiteral("识别：用的是 OpenAI 兼容的多模态消息体（content 数组）"));
            trCheck(sent.contains(QStringLiteral("原文")) && sent.contains(target),
                    QStringLiteral("识别：提示词里写明了要「原文 + 译文」两段和翻成哪个语言"));
            trCheck(!sent.contains(QStringLiteral("没有收到图片")),
                    QStringLiteral("识别：请求里没有把图片漏掉（模型不会回「我没收到图」）"));

            /* 回来的"原文 ---- 译文"要拆得开（界面上是上下两栏） */
            const QString original = llm->ocrOriginal(mock.replyContent);
            trCheck(original == QStringLiteral("Hello world"),
                    QStringLiteral("识别：结果里的原文能单独抠出来（界面上下两栏就靠它）"),
                    original);
            trCheck(original != mock.replyContent,
                    QStringLiteral("识别：抠出来的原文不是整段（分隔行真的起作用了）"));

            /* 模型不按格式回（只说了一句话）时不能把内容吞掉 */
            const QString loose = QStringLiteral("图上写着：你好");
            trCheck(llm->ocrOriginal(loose) == loose,
                    QStringLiteral("识别：模型不按格式回时，整段当原文（不吞内容）"),
                    llm->ocrOriginal(loose));
        }

        /* ---- 10.4 没配模型 / 接口：回的是能看懂的一句话 ---- */
        {
            const QString goodBase = llm->apiBase();
            const QString goodOcr = llm->ocrModel();
            llm->setOcrModel(QString());
            llm->setModel(QString());
            llm->setMode(QStringLiteral("api"));

            QString gotError;
            QObject probe;
            QObject::connect(llm, &LlmClient::failed, &probe,
                             [&](const QString &, const QString &error) { gotError = error; });

            QImage image(24, 24, QImage::Format_ARGB32);
            image.fill(Qt::black);
            llm->recognize(imageToDataUrl(image), QStringLiteral("英语"));

            QEventLoop loop;
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            QObject::connect(llm, &LlmClient::failed, &loop, &QEventLoop::quit);
            loop.exec();
            settle();
            trCheck(gotError.contains(QStringLiteral("模型名")),
                    QStringLiteral("识别：没填识别模型时报的是「缺模型名」（而不是「缺接口地址」）"),
                    gotError);

            llm->setApiBase(goodBase);
            llm->setOcrModel(goodOcr);
        }
    }

    /* =====================================================================
     * 收尾：把自检动过的配置写回去，卡片收起来
     * =================================================================== */
    cards->hideCard();
    llm->setMode(savedMode);
    if (hadBase)
        llm->setApiBase(savedBase);
    else
        settings.remove(QStringLiteral("translate/apiBase"));
    if (hadModel)
        llm->setModel(savedModel);
    else
        settings.remove(QStringLiteral("translate/model"));
    if (hadKey)
        llm->setApiKey(savedKey);
    else
        settings.remove(QStringLiteral("translate/apiKey"));

    QSettings restore;
    for (auto it = saved.constBegin(); it != saved.constEnd(); ++it) {
        if (it.value().isValid())
            restore.setValue(it.key(), it.value());
        else
            restore.remove(it.key());
    }
    restore.sync();

    trOut(QStringLiteral("翻译自检：通过 %1 项，失败 %2 项")
              .arg(gTranslatePassed)
              .arg(gTranslateFailed));
    return gTranslateFailed;
}
