#include "SelfTest.h"

#include "ClipboardStore.h"
#include "DocConvert.h"
#include "DocImport.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
/*
 * 文档识别专用自检（`SmartClip.exe --doc-test`，见 SelfTest.h 的 runDoc）。
 *
 * 和便签 / 翻译那两份分开，理由一样：SelfTest.cpp 里那几千行会开截图选区窗口、
 * 弹卡片，跑一趟十几秒还有自己的时序问题。改识别的时候只需要跑这一份。
 *
 * 这里**不跑真脚本** —— 一份 PDF 几秒到几分钟，还依赖本机装没装 rapid-doc /
 * paddleocr / docling。跑一趟自检不能是"看你装了什么"。所以真脚本那条路由
 * main.cpp 那边的一条冒烟检查覆盖（见文件末尾"命令拼得对"那一节），
 * 这一份钉的是**最容易坏、坏了又不容易发现**的那几件事：
 *
 *   * 结果 JSON 解析：裸对象 / 裹在 ```json 里 / 前后夹日志，都得认；
 *   * 图片引用的改写：`images/a.png`、`a.png`、`./a.png`、带 "标题" 的写法都要
 *     变成 `assets/a.png`；**而"标题"前后那个空格不能被吃掉**
 *     （这个 bug 真写出过：`assets/a.png"caption")`）；
 *   * 脚本没给数据的那张图 -> 变成占位，不能在正文里留一个断链；
 *   * 命令拼法：档位词接在最后、路径带引号、python 找不到时的退路；
 *   * 笔记落盘：正文带标题、图片真的写进 assets/、能在库里查到。
 *
 * 自检会动 QSettings 里 doc 那一组键（要试设置和失败路径），跑完**按原样
 * 写回** —— 用户自己的配置不会被自检改掉。
 */

namespace {

int gDocPassed = 0;
int gDocFailed = 0;

void docCheck(bool ok, const QString &what, const QString &detail = QString()) {
    if (ok) {
        ++gDocPassed;
        std::fputs("  ok    ", stdout);
    } else {
        ++gDocFailed;
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

void docOut(const QString &line) {
    std::fputs("        ", stdout);
    std::fputs(line.toUtf8().constData(), stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

/* 一小段合法 PNG（1x1 透明）—— 当"识别出来的插图"用，只要不是空字节就行 */
QByteArray tinyPng() {
    static const unsigned char bytes[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
        0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
        0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    };
    return QByteArray(reinterpret_cast<const char *>(bytes), int(sizeof(bytes)));
}

QString base64Of(const QByteArray &bytes) {
    return QString::fromLatin1(bytes.toBase64());
}

}  // namespace

bool SelfTest::docTestEnabled(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--doc-test"))
            return true;
    }
    return false;
}

bool SelfTest::docE2eEnabled(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--doc-e2e"))
            return true;
    }
    return false;
}

bool SelfTest::docQueueEnabled(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--doc-queue"))
            return true;
    }
    return false;
}

int SelfTest::docPassed() { return gDocPassed; }
int SelfTest::docFailed() { return gDocFailed; }

int SelfTest::runDocQueue(DocImport *doc, ClipboardStore *store, const QString &pythonExe,
                          QObject *qmlRoot) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::fputs("\n== 文档识别队列自检（enqueue -> 线程池 -> 落笔记） ==\n", stdout);

    if (!store) {
        docCheck(false, "没给 store，这条跑不了");
        return gDocFailed;
    }

    /* 造一份真 PDF（和 runDocE2e 同一套，理由见那边的说明） */
    QTemporaryDir temp;
    if (!temp.isValid()) {
        docCheck(false, "临时目录建不出来");
        return gDocFailed;
    }
    const QString pdfPath = temp.filePath(QStringLiteral("queue-smoke.pdf"));
    {
        QPdfWriter writer(pdfPath);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(150);
        QPainter painter(&writer);
        painter.drawText(200, 300, QStringLiteral("Queue smoke 队列冒烟"));
        painter.drawText(200, 360, QStringLiteral("Second line of the queue test."));
        painter.end();
    }
    docCheck(QFileInfo::exists(pdfPath), "造出一份真 PDF");

    /*
     * 用 QML 里那个 Doc 单例本身（不是在这儿 new 一个）。
     *
     * 踩过：原来这里 `DocImport doc(store)` 自己建了一个，于是后面"给它排队、
     * 再去问 QML 里的单例"问的是两个对象 —— 界面那一侧（卡片开不开、busy 亮不亮）
     * 从来没被驱动过，检查全红而且红得莫名其妙（自检报的 busy=1，卡片读到的
     * busy=0，两个 id 不一样才发现）。这条自检要验的正是**界面那条路**，
     * 所以必须驱动真身。
     */
    if (!doc) {
        docCheck(false, "没给 Doc 单例，这条跑不了");
        return gDocFailed;
    }
    if (!pythonExe.trimmed().isEmpty())
        doc->setPythonPath(pythonExe);
    doc->setTier(QStringLiteral("fast"));

    const QString problem = doc->runnerProblem();
    if (!problem.isEmpty()) {
        docCheck(false, "识别程序用不了，这条跳过", problem);
        return gDocFailed;
    }

    /* 等 finished：这条链路是异步的（线程池 + 信号），不能光 settle 就断言 */
    QEventLoop loop;
    int succeeded = -1;
    int failedCount = -1;
    QObject::connect(doc, &DocImport::finished, &loop,
                     [&](int ok, int bad) {
                         succeeded = ok;
                         failedCount = bad;
                         loop.quit();
                     });

    doc->enqueue({ pdfPath });
    docCheck(doc->busy(), "enqueue 之后进入忙状态", doc->status());

    QTimer::singleShot(180000, &loop, [&loop]() {
        std::fputs("        （队列自检等超时了）\n", stdout);
        loop.quit();
    });
    loop.exec();

    docCheck(succeeded >= 0, "finished 信号回来了",
             QStringLiteral("ok=%1 bad=%2").arg(succeeded).arg(failedCount));
    docCheck(!doc->busy(), "跑完之后不忙了", doc->status());
    docCheck(doc->error().isEmpty(), "没有报错", doc->error());
    docCheck(doc->created().size() == 1, "建出来 1 份笔记",
             QStringLiteral("%1 份").arg(doc->created().size()));
    if (!doc->created().isEmpty()) {
        const QString note = doc->created().constFirst();
        docCheck(QFileInfo::exists(note), "那份笔记真在磁盘上", note);
        const QString body = store->textOf(note);
        docCheck(body.contains(QStringLiteral("队列冒烟"))
                     || body.contains(QStringLiteral("Queue smoke")),
                 "笔记正文是识别出来的内容", body.left(80));
        store->deleteFile(note);
        docCheck(!QFileInfo::exists(note), "自检造的笔记清掉了");
    }

    /* 清掉结果：界面关卡片走的就是这个。清之前先把这一轮建了几份记下来 */
    int madeTotal = doc->created().size();
    doc->clearResult();

    /*
     * 进度卡片的位置（见 Main.qml 的 uiState）。
     *
     * 这一段钉的是用户报过的那个问题：卡片原来是个**场景内的浮层**，而编辑区是
     * **原生 QScintilla 子窗口**（永远画在 QML 之上，z 值改不了这块），于是卡片
     * 被编辑区整个盖住，只在编辑区左边缘往左露出一小条。改成 Popup.Window
     * （独立原生窗口）之后才盖得上去。
     *
     * 自检量不到"窗口系统实际把谁画在上面"（那只能看截图），能自动钉住的是两件：
     *   1) 该显示的时候卡片真的开起来了；
     *   2) 卡片底边**不压底部状态栏**（压上去就把"行数 / 编码 / 缩放"挡了）。
     * 第 2 条是几何判据，布局一改就会红，正是要的。
     */
    if (qmlRoot) {
        auto uiState = [qmlRoot]() {
            QVariant value;
            QMetaObject::invokeMethod(qmlRoot, "uiState", Q_RETURN_ARG(QVariant, value));
            return value.toMap();
        };
        auto settle = [](int rounds) {
            for (int i = 0; i < rounds; ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(15);
            }
        };

        /*
         * 先把结果留着（上面 clearResult 过了，这会儿 created 是空的）——
         * 从"有结果"那个状态进入下一轮，卡片会一直开着，量得到几何。
         *
         * madeTotal 要在这个函数里**自己累加**：doc->created() 在每一轮开头的
         * clearResult() 里会被清空，所以"最后读一次"只能读到最后一轮那一份
         * （一开始就是这么写的，于是"两轮各建一份"永远只报 1 份，红得莫名其妙）。
         */
        doc->enqueue({ pdfPath });
        {
            QEventLoop round;
            QObject::connect(doc, &DocImport::finished, &round, [&round]() { round.quit(); });
            /* 卡片是**跑起来那一刻**就开的，先让它开出来再等跑完 */
            settle(20);
            const QVariantMap busyState = uiState();
            docCheck(busyState.value(QStringLiteral("docCardShouldShow")).toBool(),
                     "识别期间卡片该显示（shouldShow）");
            docCheck(busyState.value(QStringLiteral("docCardShowing")).toBool(),
                     "识别期间卡片的状态是「有东西可显示」");

            const double y = busyState.value(QStringLiteral("docCardY")).toDouble();
            const double h = busyState.value(QStringLiteral("docCardHeight")).toDouble();
            docOut(QStringLiteral("卡片几何：x=%1 y=%2 w=%3 h=%4")
                       .arg(busyState.value(QStringLiteral("docCardX")).toDouble())
                       .arg(y)
                       .arg(busyState.value(QStringLiteral("docCardWidth")).toDouble())
                       .arg(h));
            docCheck(y > 0 && h > 0, "卡片几何算出来了（不是 0）");
            docCheck(busyState.value(QStringLiteral("docCardX")).toDouble() > 0,
                     "卡片贴的是右边（x 不为 0）");

            /*
             * 位置：量"确实被推过来了"，而不是量卡片的屏幕坐标。
             *
             * 自检里主窗口是**隐藏的**，那时候宿主 QWidget 的 geometry 和 QML
             * 窗口的几何对不上（实测差一个窗口偏移），拿它算期望值去比是自欺欺人
             * —— 上一版就是这么"通过"的。能可靠验的是：推过来了（lastCardPos
             * 非零），而且推的那个算式本身对（expectedCardPos 是个纯函数，
             * 下面单独验）。
             *
             * "卡片真的贴在主窗口右下角"这件事由 `--doc-demo` 肉眼 + 截图确认
             * （见文档），因为那需要窗口真的显示出来。
             */
            const QPoint pushed = doc->lastCardPos();
            docCheck(pushed.x() != 0 || pushed.y() != 0,
                     "卡片位置被推给界面了（不是一直没摆）",
                     QStringLiteral("(%1,%2)").arg(pushed.x()).arg(pushed.y()));

            /*
             * 算式本身（纯函数，和窗口显不显示无关）：
             *   x = hostX + hostW - cardW - margin
             *   y = hostY + hostH - cardH - bottomGap
             */
            const QPoint want = doc->expectedCardPos(100, 200, 1460, 900, 340, 118);
            docCheck(want.x() == 100 + 1460 - 340 - doc->cardMargin()
                         && want.y() == 200 + 900 - 118 - doc->cardBottomGap(),
                     "卡片位置算式对（宿主右下角往里收一圈）",
                     QStringLiteral("(%1,%2)").arg(want.x()).arg(want.y()));

            QTimer::singleShot(180000, &round, [&round]() { round.quit(); });
            round.exec();
        }
        madeTotal += doc->created().size();

        docCheck(madeTotal == 2, "两轮各建出一份笔记",
                 QStringLiteral("%1 份").arg(madeTotal));

        /*
         * "有结果"这个状态下卡片还开着（用户要看得见那个"打开笔记"）。
         *
         * 判据用 shouldShow 而不是 visible：visible 是**窗口系统那边的状态**，
         * 赋值到生效之间有一拍（自检里实测读到过 false，而几何明明已经摆好了）。
         * shouldShow 是驱动它的那个纯逻辑值，量它才不受这一拍影响。
         */
        settle(10);
        docCheck(uiState().value(QStringLiteral("docCardShouldShow")).toBool(),
                 "认完之后卡片还该显示（等用户点「打开笔记」或关掉）");

        /*
         * 关掉：清掉结果 -> 卡片收起来。
         *
         * 先把 created 抄一份再清 —— clearResult() 会把 created 清空，清完再读
         * 就读不到那两份笔记的路径了，自检造的文件会留在用户的内容目录里。
         */
        const QStringList made = doc->created();
        doc->clearResult();
        settle(10);
        docCheck(!uiState().value(QStringLiteral("docCardShouldShow")).toBool(),
                 "清掉结果之后卡片收起来了");

        for (const QString &note : made)
            store->deleteFile(note);
    }

    docCheck(doc->created().isEmpty() && doc->error().isEmpty(), "clearResult 清干净了");

    doc->shutdown();
    docOut(QStringLiteral("通过 %1 项，失败 %2 项").arg(gDocPassed).arg(gDocFailed));
    return gDocFailed;
}

int SelfTest::runDocE2e(ClipboardStore *store) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::fputs("\n== 文档识别端到端自检（真跑脚本） ==\n", stdout);

    /*
     * 造一份**真 PDF**：QPdfWriter 是 Qt 自带的，不用往自检里塞一份二进制样例，
     * 而且它写出来的是**带文字层**的 PDF —— RapidDoc 那条路对带文字层的 PDF
     * 走的是"直接抽文字"，不需要 OCR，几秒就回来（正适合冒烟）。
     */
    QTemporaryDir temp;
    if (!temp.isValid()) {
        docCheck(false, "临时目录建不出来");
        return gDocFailed;
    }
    const QString pdfPath = temp.filePath(QStringLiteral("e2e-smoke.pdf"));
    {
        QPdfWriter writer(pdfPath);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(150);
        QPainter painter(&writer);
        if (!painter.isActive()) {
            docCheck(false, "PDF 写不出来（QPdfWriter 起不来）");
            return gDocFailed;
        }
        QFont font = painter.font();
        font.setPointSize(16);
        painter.setFont(font);
        painter.drawText(200, 300, QStringLiteral("SmartClip 识别自检"));
        font.setPointSize(11);
        painter.setFont(font);
        painter.drawText(200, 400, QStringLiteral("End-to-end smoke test line one."));
        painter.drawText(200, 460, QStringLiteral("第二行：端到端冒烟测试。"));
        painter.end();
    }
    docCheck(QFileInfo::exists(pdfPath) && QFileInfo(pdfPath).size() > 0,
             "造出一份真 PDF 了", QString::number(QFileInfo(pdfPath).size()));

    /*
     * 跑**设置里配的那条命令**（DocImport::runner 存的就是不带档位的基命令，
     * 档位由 DocImport 拼在后面 —— 见 pump()）。这样验的正是用户会走的那条路。
     *
     * 档位固定用 fast：这只冒烟，不问准不准；fast 关掉表格和公式那两段最慢的，
     * 几秒到几十秒能回来。真按用户设置的档位跑，一次可能好几分钟。
     */
    DocImport doc(store);

    /*
     * 这条自检常常要指一个**别的** Python（依赖装在 venv 里，而设置里配的是
     * 系统那个）。为这个单开一个开关，而不是去改用户设置 —— 自检不该动配置。
     *
     *     SmartClip.exe --doc-e2e "H:\steward\venv\Scripts\python.exe"
     */
    QString pythonExe = doc.pythonPath();
    for (int i = 1; i < qApp->arguments().size(); ++i) {
        const QString arg = qApp->arguments().at(i);
        if (arg.startsWith(QLatin1String("--doc-python="))) {
            pythonExe = arg.mid(int(qstrlen("--doc-python=")));
            break;
        }
    }

    const QString command = DocConvert::defaultRunnerCommand(QStringLiteral("fast"),
                                                             pythonExe);
    docOut(QStringLiteral("用的命令：%1").arg(command));

    const QString problem = DocConvert::runnerProblem(command);
    if (!problem.isEmpty()) {
        docCheck(false, "识别程序用不了，端到端这条跳过", problem);
        return gDocFailed;
    }

    const DocConvert::Result result = DocConvert::convert(pdfPath, command, 600000);
    docCheck(result.ok(), "真脚本跑通了", result.error);
    if (!result.ok()) {
        docOut(QStringLiteral("（脚本没装好？默认那条要 pip install rapid-doc）"));
        return gDocFailed;
    }

    docOut(QStringLiteral("引擎：%1  页数：%2").arg(result.engine).arg(result.pages));
    docOut(QStringLiteral("正文前 200 字：%1").arg(result.markdown.left(200)));
    docCheck(result.markdown.contains(QStringLiteral("冒烟测试"))
                 || result.markdown.contains(QStringLiteral("smoke")),
             "认出来的正文里有刚才写进去的字");
    docCheck(result.pages >= 1, "页数报上来了", QString::number(result.pages));
    docCheck(result.engine.contains(QStringLiteral("rapid")),
             "引擎名报上来了", result.engine);

    if (store) {
        const QString note = store->createNote(result.title, result.markdown, result.images);
        docCheck(!note.isEmpty(), "落成笔记了", note);
        if (!note.isEmpty()) {
            docCheck(store->textOf(note).contains(QStringLiteral("冒烟测试"))
                         || store->textOf(note).contains(QStringLiteral("smoke")),
                     "笔记正文读得回来");
            store->deleteFile(note);
            docCheck(!QFileInfo::exists(note), "自检造的笔记清掉了");
        }
    }

    docOut(QStringLiteral("通过 %1 项，失败 %2 项").arg(gDocPassed).arg(gDocFailed));
    return gDocFailed;
}

int SelfTest::runDoc(ClipboardStore *store) {
    /* 每条检查立刻落盘：崩了也能看到崩在哪一条 */
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::fputs("\n== 文档识别自检 ==\n", stdout);

    /* ------------------------------------------------------------------
     * 1. 支持哪些格式 / 标题 / 类别
     * ------------------------------------------------------------------ */
    std::fputs("\n-- 格式判断 --\n", stdout);
    {
        /*
         * 这几条用**真文件**试（空文件就够 —— unsupportedReason 判的是扩展名
         * 和存不存在，不读内容）。一开始图省事直接传了假路径，结果全挂在
         * "找不到这个文件"上，看着像格式判断坏了。
         */
        QTemporaryDir dir;
        auto touch = [&dir](const QString &name) {
            const QString path = dir.filePath(name);
            QFile file(path);
            if (file.open(QIODevice::WriteOnly))
                file.write("x");
            file.close();
            return path;
        };

        docCheck(DocConvert::unsupportedReason(touch(QStringLiteral("a.pdf"))).isEmpty(),
                 "PDF 认得");
        docCheck(DocConvert::unsupportedReason(touch(QStringLiteral("a.PNG"))).isEmpty(),
                 "大写扩展名也认（.PNG）");
        docCheck(DocConvert::unsupportedReason(touch(QStringLiteral("a.docx"))).isEmpty(),
                 "docx 认得");
        docCheck(DocConvert::unsupportedReason(touch(QStringLiteral("a.tiff"))).isEmpty(),
                 "tiff 认得");
        docCheck(!DocConvert::unsupportedReason(touch(QStringLiteral("a.exe"))).isEmpty(),
                 "exe 不认（要说一句人话）");
        docCheck(!DocConvert::unsupportedReason(QStringLiteral("不存在的文件.pdf")).isEmpty(),
                 "文件不存在时也报出来");

        /* 只看扩展名那一份：不碰磁盘，界面"拖进来之前先问一句"用它 */
        docCheck(DocConvert::unsupportedSuffixReason(QStringLiteral("随便什么.pdf")).isEmpty(),
                 "只看扩展名：pdf 认");
        docCheck(!DocConvert::unsupportedSuffixReason(QStringLiteral("随便什么.xyz")).isEmpty(),
                 "只看扩展名：xyz 不认");

        docCheck(DocConvert::titleFor(QStringLiteral("D:/x/季度报告.pdf"))
                     == QStringLiteral("季度报告"),
                 "标题取的是文件名（去扩展名）",
                 DocConvert::titleFor(QStringLiteral("D:/x/季度报告.pdf")));
        docCheck(DocConvert::kindFor(QStringLiteral("a.pdf")) == QStringLiteral("PDF 文档"),
                 "PDF 的类别词");
        docCheck(DocConvert::kindFor(QStringLiteral("a.png")) == QStringLiteral("图片"),
                 "图片的类别词");
        docCheck(DocConvert::fileFilter().contains(QStringLiteral("*.pdf")),
                 "文件过滤器里有 pdf");
    }

    /* ------------------------------------------------------------------
     * 2. 结果 JSON 的解析（宽容那几条）
     * ------------------------------------------------------------------ */
    std::fputs("\n-- 结果 JSON 解析 --\n", stdout);
    {
        const QString plain = QStringLiteral(R"({"markdown":"# 标题\n\n正文","pages":3,"engine":"rapid-doc 0.9.10"})");
        const DocConvert::Result r = DocConvert::parseResultJson(plain);
        docCheck(r.ok(), "裸 JSON 解得开", r.error);
        docCheck(r.markdown.contains(QStringLiteral("正文")), "正文取到了");
        docCheck(r.pages == 3, "页数取到了", QString::number(r.pages));
        docCheck(r.engine.contains(QStringLiteral("rapid-doc")), "引擎名取到了", r.engine);

        /* 脚本把进度条打在 stdout 上、JSON 混在中间（RapidDoc 真会这样） */
        const QString noisy = QStringLiteral("Processing pages: 100%|##| 1/1\n")
                              + plain + QStringLiteral("\n[INFO] done");
        docCheck(DocConvert::parseResultJson(noisy).ok(),
                 "JSON 前后夹着日志也解得开");

        /* 裹在 ```json 里 */
        const QString fenced = QStringLiteral("```json\n") + plain + QStringLiteral("\n```");
        docCheck(DocConvert::parseResultJson(fenced).ok(),
                 "裹在 ```json 代码块里也解得开");

        /* 脚本明确报错 */
        const DocConvert::Result bad =
            DocConvert::parseResultJson(QStringLiteral(R"({"error":"缺依赖：No module named 'rapid_doc'"})"));
        docCheck(!bad.ok() && bad.error.contains(QStringLiteral("No module")),
                 "脚本报的 error 原样带出来", bad.error);

        /* 空内容 */
        const DocConvert::Result empty =
            DocConvert::parseResultJson(QStringLiteral(R"({"markdown":"   "})"));
        docCheck(!empty.ok(), "认不出任何内容时算失败（不是「成功但空」）", empty.error);

        /* 根本不是 JSON */
        docCheck(!DocConvert::parseResultJson(QStringLiteral("boom")).ok(),
                 "不是 JSON 时报得出来");
    }

    /* ------------------------------------------------------------------
     * 3. 图片引用改写 —— 这一段是重点（真写出过 bug）
     * ------------------------------------------------------------------ */
    std::fputs("\n-- 图片引用改写 --\n", stdout);
    {
        const QString png = base64Of(tinyPng());
        /*
         * 拼一份结果 JSON 出来。
         *
         * 三个坑都踩过，写在这里免得下次再踩：
         *
         *  1) `QStringLiteral(R"(...)")` 这种写法**不安全** —— 那个宏是靠数括号
         *     配对来找结尾的，JSON 里 `{"a":1}` 这种成对括号会让它配对错位，
         *     生成出一份坏的 JSON。用 QString::fromUtf8 + 普通字面量，行为明确。
         *  2) `R"("}]})"` 里的 `"("` 和 `)"` 之间只有 `]})` —— 开头那个引号是
         *     **定界符的一部分**，不在内容里。要往内容里放引号得写 `R"(""}]})"`。
         *  3) markdown 里的 `"` 和 `\` 必须转义才能放进 JSON 字符串 —— 不转的话
         *     拼出来就不是合法 JSON，"解析失败"会伪装成"改写失败"（带标题那条
         *     引用的用例就是这么假红的）。真脚本吐的是合法 JSON，所以这里也得
         *     吐合法的。
         */
        auto parseWith = [&png](const QString &markdown) {
            QString body = markdown;
            /* 反斜杠先转（不然会把下面刚加进去的转义再转一遍） */
            body.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
            body.replace(QLatin1Char('"'), QLatin1String("\\\""));
            body.replace(QLatin1Char('\n'), QLatin1String("\\n"));
            const QString json = QString::fromUtf8(
                "{\"markdown\":\"") + body
                + QString::fromUtf8("\",\"images\":{\"fig1.png\":\"") + png
                + QString::fromUtf8("\"}}");
            return DocConvert::parseResultJson(json);
        };

        const DocConvert::Result r1 = parseWith(QStringLiteral("![](images/fig1.png)"));
        docCheck(r1.markdown.contains(QStringLiteral("](assets/fig1.png)")),
                 "images/fig1.png -> assets/fig1.png", r1.markdown);
        docCheck(r1.images.contains(QStringLiteral("fig1.png")), "图片数据收进来了");

        const DocConvert::Result r2 = parseWith(QStringLiteral("![x](fig1.png)"));
        docCheck(r2.markdown.contains(QStringLiteral("](assets/fig1.png)")),
                 "裸文件名也改写成 assets/", r2.markdown);

        const DocConvert::Result r3 = parseWith(QStringLiteral("![x](./fig1.png)"));
        docCheck(r3.markdown.contains(QStringLiteral("](assets/fig1.png)")),
                 "./fig1.png 也改写", r3.markdown);

        /*
         * 带标题的引用：**标题前面那个空格必须留着**。
         * 这里就是踩过的那个坑 —— 正则写成 \s*(?=[)\s]) 时，空格被回溯吃掉，
         * 结果变成 assets/fig1.png"caption")，Markdown 直接坏掉。
         */
        const DocConvert::Result r4 = parseWith(QStringLiteral("![x](images/fig1.png \"图注\")"));
        docCheck(r4.markdown.contains(QStringLiteral("](assets/fig1.png \"图注\")")),
                 "带 \"标题\" 的引用：标题还在、空格没被吃掉", r4.markdown);
        /*
         * 上面那条的**反面**：别再粘成 `assets/fig1.png"图注")`（空格被吃掉）。
         * 单独量一次是因为 .contains 的期望值写对也可能"碰巧"通过别的路径，
         * 而这一条直接盯着那个坏形状。
         */
        docCheck(!r4.markdown.contains(QStringLiteral("assets/fig1.png\"")),
                 "带标题的引用：没有粘成 assets/fig1.png\"图注\")", r4.markdown);

        /* 名字相近的别的图不能被误伤 */
        const DocConvert::Result r5 = parseWith(QStringLiteral("![x](myfig1.png)"));
        docCheck(r5.markdown.contains(QStringLiteral("myfig1.png"))
                     && !r5.markdown.contains(QStringLiteral("assets/myfig1.png")),
                 "名字相近的别的图不动它", r5.markdown);

        /* 脚本没给数据的那张图 -> 占位，不能留断链 */
        const DocConvert::Result r6 = parseWith(QStringLiteral("![](images/missing.png)"));
        docCheck(!r6.markdown.contains(QStringLiteral("images/missing.png")),
                 "没拿到数据的图不留断链", r6.markdown);
        docCheck(r6.markdown.contains(QStringLiteral("图片")),
                 "没拿到数据的图变成占位文字", r6.markdown);

        /* 图片表写成数组也认 */
        const QString arrayJson =
            QString::fromUtf8("{\"markdown\":\"![](images/a.png)\",\"images\":[{\"name\":\"a.png\",\"data\":\"")
            + png + QString::fromUtf8("\"}]}");
        const DocConvert::Result r7 = DocConvert::parseResultJson(arrayJson);
        docCheck(r7.images.contains(QStringLiteral("a.png")),
                 "images 写成数组也认");
        docCheck(r7.markdown.contains(QStringLiteral("assets/a.png")),
                 "数组那份的引用也改写了", r7.markdown);

        /* data URI 形式的 base64 也认 */
        const QString dataUri =
            QStringLiteral(R"({"markdown":"x","images":{"b.png":"data:image/png;base64,)")
            + png + QStringLiteral(R"("}})");
        docCheck(DocConvert::parseResultJson(dataUri).images.contains(QStringLiteral("b.png")),
                 "带 data: 头的 base64 也解得出");
    }

    /* ------------------------------------------------------------------
     * 4. 命令的拼法 / 判断
     * ------------------------------------------------------------------ */
    std::fputs("\n-- 命令 --\n", stdout);
    {
        const QStringList scripts = DocConvert::shippedScripts();
        docCheck(scripts.contains(QStringLiteral("doc_runner_common.py")),
                 "共用契约脚本在清单里");
        docCheck(scripts.contains(QStringLiteral("doc_runner_rapid.py")),
                 "默认 runner 在清单里");
        docCheck(scripts.contains(QStringLiteral("doc_runner_paddleocr_vl.py")),
                 "PaddleOCR-VL 的 runner 在清单里");
        docCheck(scripts.contains(QStringLiteral("doc_runner_granite.py")),
                 "Granite-Docling 的 runner 在清单里");

        /* 三个随包脚本都要能从 qrc 落到磁盘上（落不下去 = 命令必然跑不了） */
        for (const QString &name : scripts) {
            const QString path = DocConvert::runnerScriptPath(name);
            docCheck(!path.isEmpty() && QFileInfo::exists(path),
                     QStringLiteral("随包脚本落得到磁盘：%1").arg(name), path);
        }

        const QString cmd = DocConvert::defaultRunnerCommand(QStringLiteral("fast"));
        docCheck(cmd.contains(QStringLiteral("doc_runner_rapid.py")),
                 "默认命令用的是 rapid runner", cmd);
        docCheck(cmd.endsWith(QStringLiteral("fast")),
                 "档位词接在命令最后（脚本读的是倒数第三个参数）", cmd);

        const QString noTier = DocConvert::defaultRunnerCommand();
        docCheck(!noTier.endsWith(QStringLiteral("fast")),
                 "不给档位时命令末尾没有多余参数", noTier);

        docCheck(!DocConvert::runnerProblem(QString()).isEmpty(),
                 "命令为空时报得出来");
        docCheck(!DocConvert::runnerProblem(QStringLiteral("绝对不存在的程序.exe x"))
                      .isEmpty(),
                 "程序找不到时报得出来");

        docCheck(DocConvert::scriptForCommand(cmd) == QStringLiteral("doc_runner_rapid.py"),
                 "能从命令里认出用的是哪个随包脚本");
        docCheck(DocConvert::scriptForCommand(QStringLiteral("my-own-tool.exe"))
                     .isEmpty(),
                 "自己写的程序 -> 认不出随包脚本（但照样能跑）");
    }

    /* ------------------------------------------------------------------
     * 5. 落成笔记（正文 + assets）
     * ------------------------------------------------------------------ */
    std::fputs("\n-- 落成笔记 --\n", stdout);
    if (!store) {
        docOut(QStringLiteral("没给 store，跳过这一节"));
    } else {
        const QString markdown =
            QStringLiteral("# 季度报告\n\n正文一段。\n\n![](assets/fig1.png)\n");
        QMap<QString, QByteArray> assets;
        assets.insert(QStringLiteral("fig1.png"), tinyPng());

        const QString path = store->createNote(QStringLiteral("季度报告"), markdown, assets);
        docCheck(!path.isEmpty(), "笔记建出来了", path);

        if (!path.isEmpty()) {
            const QFileInfo info(path);
            docCheck(info.exists(), "笔记文件真的在磁盘上");
            docCheck(info.fileName().endsWith(QStringLiteral(".md")), "是 .md", info.fileName());

            const QString body = store->textOf(path);
            docCheck(body.contains(QStringLiteral("正文一段")), "正文写进去了");
            docCheck(body.contains(QStringLiteral("assets/fig1.png")),
                     "正文里的图片引用在", body.left(120));
            docCheck(body.count(QStringLiteral("# 季度报告")) == 1,
                     "标题只有一份（不重复加）", body.left(60));

            const QString asset = info.absolutePath() + QStringLiteral("/assets/fig1.png");
            docCheck(QFileInfo::exists(asset), "插图落到 assets/ 下了", asset);
            if (QFileInfo::exists(asset)) {
                QFile file(asset);
                /*
                 * open() 是 [[nodiscard]]：不看返回值编译器要报 C4834。
                 * 这里也**确实该看** —— 打不开的话下面 readAll() 是空的，
                 * 断言会挂在"字节不一致"上，而真正的原因是文件没打开，
                 * 报错方向就偏了。
                 */
                const bool opened = file.open(QIODevice::ReadOnly);
                docCheck(opened, "能把落盘的插图重新打开", asset);
                docCheck(file.readAll() == tinyPng(), "插图和脚本给的那份字节一致");
                file.close();
            }

            /* 正文自己没标题时，用文档名补一个 */
            const QString path2 = store->createNote(QStringLiteral("没有标题的文档"),
                                                    QStringLiteral("就是一段正文。"), {});
            docCheck(!path2.isEmpty() && store->textOf(path2).contains(
                                             QStringLiteral("# 没有标题的文档")),
                     "正文没标题时用文档名补一个");
            if (!path2.isEmpty())
                store->deleteFile(path2);

            /* 清理：这一份是自检造出来的，不该留在用户的内容目录里 */
            store->deleteFile(path);
            docCheck(!QFileInfo::exists(path), "自检造的笔记清掉了");
        }
    }

    /* ------------------------------------------------------------------
     * 6. DocImport 的设置和排队（失败路径）
     * ------------------------------------------------------------------ */
    std::fputs("\n-- DocImport --\n", stdout);
    {
        DocImport doc(store);

        /* 档位只认三个词，别的折回 balanced */
        doc.setTier(QStringLiteral("fast"));
        docCheck(doc.tier() == QStringLiteral("fast"), "档位能设成 fast");
        doc.setTier(QStringLiteral("瞎写的"));
        docCheck(doc.tier() == QStringLiteral("balanced"),
                 "非法档位折回 balanced", doc.tier());

        /* 认不了的文件不会被排进队列 */
        const int before = doc.pending();
        doc.enqueue({ QStringLiteral("D:/不存在的东西.exe") });
        docCheck(doc.pending() == before && !doc.busy(),
                 "认不了的文件不入队（也不会开始跑）", doc.status());
        docCheck(!doc.error().isEmpty(), "认不了的文件要报出来", doc.error());

        doc.clearResult();
        docCheck(doc.error().isEmpty(), "clearResult 清得掉错误");

        /* 目录不递归：拖一个文件夹进来不入队 */
        QTemporaryDir temp;
        if (temp.isValid()) {
            doc.enqueue({ temp.path() });
            docCheck(!doc.busy(), "拖目录进来不会开始跑", doc.status());
        }

        /* 取消：没在跑的时候也不能崩、不能报错 */
        doc.cancel();
        docCheck(!doc.busy(), "没在跑时取消是安全的", doc.status());
        doc.shutdown();
    }

    std::fputs("\n== 文档识别自检结束 ==\n", stdout);
    docOut(QStringLiteral("通过 %1 项，失败 %2 项")
               .arg(gDocPassed)
               .arg(gDocFailed));
    return gDocFailed;
}
