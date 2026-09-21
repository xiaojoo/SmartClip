#include "SelfTest.h"

#include "ClipboardStore.h"
#include "Summarize.h"
#include "Translate.h"

#include <QDate>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QGuiApplication>
#include <QScreen>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>

/*
 * 汇总那一节的自检（`SmartClip.exe --summarize-test`，见 SelfTest.h 的 runSummarize）。
 *
 * 钉的是这条链路上**判断**对不对，不是"模型整理得好不好"（那只能靠眼睛看）：
 *
 *   * 一段时间里的原文取得对不对：同一天两条内容落在一个文件里、区间外的一天
 *     都不该出现、识别出来的笔记和手写的笔记**不能**被卷进汇总（它们和剪贴板
 *     内容同住一个日期目录、连文件名格式都一样）；
 *   * 模型回复的解析：正常标记 / 全角冒号 / 一个标记都没有 / 正文裹在 ``` 里 /
 *     正文里出现了长得像标记的那一行（那种最阴，被吃掉的一行没人发现）；
 *   * 草稿 -> 采纳：并进 <root>/文档/<分类>.md、已有分类是**追加**不是覆盖、
 *     采纳完草稿要从待审里消失、丢弃不能顺手删掉正式文档；
 *   * 归档：藏起来的只有那一份剪贴板原文，文档不受影响，已归档的不再被汇总
 *     第二遍，还原之后重新出现；
 *   * 最后把设置面板真开到「汇总」「归档」那两栏上，量它们画没画出来
 *     （一栏的高度塌成 0 是最典型的"绑定写错了但没人报错"）。
 *
 * 全程跑在一个临时保存目录里（setRootPath 到 QTemporaryDir），结束按原样换回去
 * —— 它写的是真库（元数据库在 AppData，不跟着保存目录走），所以最后那一下
 * 换回去必须执行：rescan 会照着实保存目录把元数据重建回来。
 *
 * 想亲眼看那两栏长什么样：`set SMARTCLIP_SUM_SHOT=< png 要落哪儿>` 再跑一遍，
 * 自检会把面板那块抓下来（不给这个变量就什么都不写，自检本身不留痕）。
 */

namespace {

int gSumPassed = 0;
int gSumFailed = 0;

void sumcheck(bool ok, const QString &what, const QString &detail = QString()) {
    if (ok) {
        ++gSumPassed;
        std::fputs("  ok    ", stdout);
    } else {
        ++gSumFailed;
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

void sumout(const QString &line) {
    std::fputs("        ", stdout);
    std::fputs(line.toUtf8().constData(), stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

/* 让事件循环走一会儿：QML 那边的属性刷新 / 布局不是同步的 */
void settle(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/*
 * 把设置面板那块窗抓成图。
 *
 * 面板是 Popup.Window（自己一块原生窗），所以要绕到它的 popupItem / contentItem
 * 才拿得到 QQuickWindow —— 直接问面板对象要么没有 window()，要么拿到主窗口那块。
 */
bool grabSurface(QObject *popup, const QString &file) {
    if (!popup)
        return false;
    /* 设置面板现在自己就是一块顶层 Window（不再是 Popup）—— 先按窗口认 */
    QQuickWindow *window = qobject_cast<QQuickWindow *>(popup);
    if (!window) {
        QQuickItem *item = nullptr;
        for (const char *prop : {"popupItem", "contentItem"}) {
            item = popup->property(prop).value<QQuickItem *>();
            if (item)
                break;
        }
        window = item ? item->window() : nullptr;
    }
    if (!window)
        return false;
    const QImage image = window->grabWindow();
    return !image.isNull() && image.save(file);
}

/*
 * 整张屏幕抓一张。
 *
 * 上面那张是**面板自己的表面**：它能证明两栏画没画出来，证明不了"摆在哪" ——
 * 居中那件事的 ground truth 是屏幕（面板 x/y 现在是屏幕坐标，见 placeOverHost）。
 */
bool grabDesktop(const QString &file) {
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
        return false;
    const QPixmap shot = screen->grabWindow(0);
    return !shot.isNull() && shot.save(file);
}

QString readIt(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

/* `# ` 开头的行数：同一分类采纳两次之后还得是 1，不然并出来的文档有两个大标题 */
int h1Lines(const QString &text) {
    int n = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QStringLiteral("# ")))
            ++n;
    }
    return n;
}

/* 树上有没有某个路径的节点（文件夹按 dir: 前缀找，文件按路径本身找） */
bool treeHas(const QVariantList &nodes, const QString &key) {
    for (const QVariant &value : nodes) {
        const QVariantMap node = value.toMap();
        if (node.value(QStringLiteral("key")).toString() == key)
            return true;
        if (treeHas(node.value(QStringLiteral("children")).toList(), key))
            return true;
    }
    return false;
}

/* 树上那些文件节点的路径（数一数某一份还在不在） */
void treeFiles(const QVariantList &nodes, QStringList *out) {
    for (const QVariant &value : nodes) {
        const QVariantMap node = value.toMap();
        if (node.value(QStringLiteral("kind")).toString() == QLatin1String("file"))
            out->append(QDir::cleanPath(node.value(QStringLiteral("path")).toString()));
        treeFiles(node.value(QStringLiteral("children")).toList(), out);
    }
}

QVariantMap findNode(const QVariantList &nodes, const QString &key) {
    for (const QVariant &value : nodes) {
        const QVariantMap node = value.toMap();
        if (node.value(QStringLiteral("key")).toString() == key)
            return node;
        const QVariantMap deep = findNode(node.value(QStringLiteral("children")).toList(), key);
        if (!deep.isEmpty())
            return deep;
    }
    return QVariantMap();
}

const QString kClipOne =
    QStringLiteral("QScintilla 抢走键盘焦点时 QML 收不到按键，先 releaseActiveFocus 再 requestActivate");
const QString kClipTwo =
    QStringLiteral("jom 编 SmartClip 要先 call vcvars64.bat，PATH 里加上 Qt 自带的 jom 目录");

/*
 * 假模型服务：按脚本一份一份地回，让整条汇总流水线（分批 -> 分类 -> 合并 ->
 * 落草稿）能在没有网络、没有 key 的机器上跑通。
 *
 * 和 src/SelfTestTranslate.cpp 里那个 MockLlmServer 同一套做法（分帧在字节上算，
 * 理由见那边：正文里有中文时 QString::size() 和 Content-Length 永远对不齐）。
 * 区别是这里按请求顺序换回复 —— 要试的就是"同一批里两个分类、
 * 第二批又来一个同名分类，于是补一次合并"那条分支。
 */
class ScriptedLlm final : public QTcpServer {
public:
    QStringList replies;
    int served = 0;
    QStringList requests;
    /* 第几条请求故意回 500（-1 = 都不失败）：真跑本地模型时就是栽在一条超时上 */
    int failOn = -1;

    explicit ScriptedLlm(QObject *parent = nullptr) : QTcpServer(parent) {}

protected:
    void incomingConnection(qintptr descriptor) override {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(descriptor);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            raw += socket->readAll();
            const int sep = raw.indexOf("\r\n\r\n");
            if (sep < 0)
                return;
            const int at = raw.indexOf("Content-Length:");
            const int eol = raw.indexOf('\n', at);
            const int length = at < 0 ? 0 : raw.mid(at + 15, eol - at - 15).trimmed().toInt();
            if (raw.size() < sep + 4 + length)
                return;

            requests << QString::fromUtf8(raw);
            raw.clear();

            const int which = served;
            const QString content
                = replies.value(served % replies.size(), QStringLiteral("没有脚本回复"));
            ++served;

            if (which == failOn) {
                const QByteArray bad
                    = QByteArrayLiteral(R"({"error":{"message":"模拟：这条没成"}})");
                QByteArray head = "HTTP/1.1 500 Internal Server Error\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Connection: close\r\n"
                                  "Content-Length: ";
                head += QByteArray::number(bad.size());
                head += "\r\n\r\n";
                socket->write(head + bad);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }

            QJsonObject message{{QStringLiteral("role"), QStringLiteral("assistant")},
                                {QStringLiteral("content"), content}};
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

private:
    QByteArray raw;
};

}  // namespace

bool SelfTest::summarizeTestEnabled(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--summarize-test"))
            return true;
    }
    return false;
}

int SelfTest::summarizePassed() { return gSumPassed; }
int SelfTest::summarizeFailed() { return gSumFailed; }

int SelfTest::runSummarize(ClipboardStore *store, Summarizer *sum, QObject *qmlRoot,
                           LlmClient *llm) {
    /* 每条检查立刻落盘：崩了也能看到崩在哪一条 */
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::fputs("\n== 汇总自检（取原文 / 解析回复 / 草稿采纳 / 归档） ==\n", stdout);

    if (!store) {
        sumcheck(false, "没给 store，这条跑不了");
        return gSumFailed;
    }

    QTemporaryDir temp;
    if (!temp.isValid()) {
        sumcheck(false, "临时目录建不出来");
        return gSumFailed;
    }

    const QString savedRoot = store->rootPath();
    sumcheck(store->setRootPath(temp.path()), "自检跑在临时保存目录里", temp.path());

    const QString today = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));

    /* ------------------------------------------------------------------ */
    /* 1. 分段解析                                                          */
    /* ------------------------------------------------------------------ */
    std::fputs("\n-- 分段解析 --\n", stdout);
    {
        const QList<ClipboardStore::Section> sections = ClipboardStore::parseSections(
            QStringLiteral("# 2026-09-20\n\n## 07:31:00\n\n第一段\n\n## 08:02:00\n\n第二段\n"));
        sumcheck(sections.size() == 2, "两条内容切成两段", QString::number(sections.size()));
        if (sections.size() == 2) {
            sumcheck(sections.at(0).time == QLatin1String("07:31:00"), "段首时间是原文那一串");
            sumcheck(sections.at(0).body.contains(QStringLiteral("第一段")), "第一段正文归它自己");
            sumcheck(!sections.at(1).body.contains(QStringLiteral("第一段")),
                     "上一段没串到下一段里");
        }
        sumcheck(ClipboardStore::parseSections(QStringLiteral("### 三级标题\n正文")).isEmpty(),
                 "### 不会被当成段首（那是汇总输出的层级）");
        sumcheck(ClipboardStore::parseSections(QStringLiteral("## 结论\n识别笔记的小标题"))
                     .size() == 1,
                 "非时间的小标题也切成一段（时间解析不出来由调用方判）");
    }

    /* ------------------------------------------------------------------ */
    /* 2. 一段时间的原文                                                    */
    /* ------------------------------------------------------------------ */
    std::fputs("\n-- 汇总输入按区间取 --\n", stdout);
    QString clipFile;
    {
        sumcheck(store->captureText(kClipOne), "第一条采集进去了");
        sumcheck(store->captureText(kClipTwo), "第二条采集进去了");
        store->rescan();

        const QVariantList rows = store->sectionsInRange(today, today);
        sumcheck(rows.size() == 1, "同一天两条内容落在一个文件里", QString::number(rows.size()));
        if (!rows.isEmpty()) {
            const QVariantMap row = rows.first().toMap();
            clipFile = QDir::cleanPath(row.value(QStringLiteral("path")).toString());
            sumcheck(row.value(QStringLiteral("count")).toInt() == 2, "两段原文",
                     row.value(QStringLiteral("count")).toString());
            const QString text = row.value(QStringLiteral("text")).toString();
            sumcheck(text.contains(QStringLiteral("releaseActiveFocus"))
                         && text.contains(QStringLiteral("vcvars64")),
                     "两段正文都在喂给模型的文本里");
            sumcheck(text.contains(QStringLiteral("### %1 ").arg(today)),
                     "每段前面带着它自己的日期（跨天才能分先后）");
        }

        sumcheck(store->sectionsInRange(QStringLiteral("2000-01-01"),
                                        QStringLiteral("2000-01-02"))
                     .isEmpty(),
                 "区间外一天都不该有");
        sumcheck(store->sectionsInRange(today, QStringLiteral("2000-01-01")).isEmpty(),
                 "开始晚于结束时不给结果");

        /* 文档识别落的那份笔记：同一个日期目录里，但不能被卷进汇总 */
        const QString note = store->createNote(
            QStringLiteral("季度报告"),
            QStringLiteral("# 季度报告\n\n## 结论\n\n这份是识别出来的，不是复制来的"),
            {});
        sumcheck(!note.isEmpty(), "造出一份识别笔记");
        const QVariantList after = store->sectionsInRange(today, today);
        sumcheck(after.size() == 1, "识别笔记没被卷进汇总", QString::number(after.size()));
    }

    /* ------------------------------------------------------------------ */
    /* 3. 模型回复的解析                                                     */
    /* ------------------------------------------------------------------ */
    std::fputs("\n-- 模型回复解析 --\n", stdout);
    if (sum) {
        const QVariantList two = sum->parseReply(
            QStringLiteral("好的，整理如下：\n"
                           "=== 分类: 前端 ===\n## Vue\n内容一\n"
                           "=== 分类: 生活 ===\n买牛奶"));
        sumcheck(two.size() == 2, "两个分类解成两条", QString::number(two.size()));
        if (two.size() == 2) {
            sumcheck(two.at(0).toMap().value(QStringLiteral("category")).toString()
                         == QStringLiteral("前端"),
                     "第一个分类名");
            sumcheck(two.at(0).toMap().value(QStringLiteral("body")).toString()
                         .contains(QStringLiteral("## Vue")),
                     "第一个分类的正文");
            sumcheck(!two.at(0).toMap().value(QStringLiteral("body")).toString()
                         .contains(QStringLiteral("好的")),
                     "标记之前那句废话不留在正文里");
        }

        const QVariantList full = sum->parseReply(
            QStringLiteral("=== 分类：Qt ===\n编译姿势"));
        sumcheck(full.size() == 1
                     && full.at(0).toMap().value(QStringLiteral("category")).toString()
                            == QStringLiteral("Qt"),
                 "全角冒号也认（模型一顺手就换成中文冒号）");

        const QVariantList none = sum->parseReply(QStringLiteral("整段都是正文，没有标记"));
        sumcheck(none.size() == 1
                     && none.at(0).toMap().value(QStringLiteral("category")).toString()
                            == QStringLiteral("未分类"),
                 "一个标记都没有时整份兜进「未分类」，不白丢");

        const QVariantList fenced = sum->parseReply(
            QStringLiteral("=== 分类: A ===\n```markdown\n内容\n```"));
        sumcheck(fenced.size() == 1
                     && !fenced.at(0).toMap().value(QStringLiteral("body")).toString()
                                .contains(QStringLiteral("```")),
                 "正文裹在代码块围栏里时把围栏剥掉");

        const QVariantList lookalike = sum->parseReply(
            QStringLiteral("=== 分类: A ===\n第一行\n=== 参见: 附录 ===\n第二行"));
        sumcheck(lookalike.size() == 1
                     && lookalike.at(0).toMap().value(QStringLiteral("body")).toString()
                                .contains(QStringLiteral("=== 参见: 附录 ===")),
                 "正文里长得像标记的那一行不被吃掉",
                 lookalike.size() == 1
                     ? lookalike.at(0).toMap().value(QStringLiteral("body")).toString()
                     : QStringLiteral("%1 条").arg(lookalike.size()));

        sumcheck(sum->classifyPrompt().contains(QStringLiteral("=== 分类:")),
                 "提示词里写着输出格式（改了格式要同步改解析）");
        sumcheck(sum->classifyPrompt(QStringLiteral("前端、Qt")).contains(QStringLiteral("前端、Qt")),
                 "已有分类喂给了模型（防它另起一套）");
        sumcheck(sum->mergePrompt(QStringLiteral("前端")).contains(QStringLiteral("前端")),
                 "合并提示词带着分类名");
    } else {
        sumout("没给 Sum 单例，解析那一节跳过");
    }

    /* ------------------------------------------------------------------ */
    /* 4. 草稿 -> 采纳                                                      */
    /* ------------------------------------------------------------------ */
    std::fputs("\n-- 待审草稿与采纳 --\n", stdout);
    QString docPath;
    {
        const QString draft = store->writeDraft(QStringLiteral("20260921_0800"),
                                                QStringLiteral("前端"),
                                                QStringLiteral("## 汇总 a ~ b\n\n> 出处\n\n## Vue\n内容一"));
        sumcheck(!draft.isEmpty() && QFileInfo::exists(draft), "草稿落到待审那一层", draft);
        sumcheck(draft.endsWith(QStringLiteral("20260921_0800-前端.md")), "草稿文件名带批次号和分类");
        sumcheck(store->drafts().size() == 1, "待审清单看得到它");
        if (!store->drafts().isEmpty()) {
            sumcheck(store->drafts().first().toMap().value(QStringLiteral("category")).toString()
                         == QStringLiteral("前端"),
                     "从文件名读回分类名（改文件名=改分类）");
        }

        docPath = store->adoptDraft(draft);
        sumcheck(!docPath.isEmpty() && QFileInfo::exists(docPath), "采纳之后文档建出来了", docPath);
        sumcheck(docPath.endsWith(QStringLiteral("文档/前端.md")), "文档在 <root>/文档 下面", docPath);
        const QString first = readIt(docPath);
        sumcheck(first.startsWith(QStringLiteral("# 前端\n")), "文档开头是那份文档自己的大标题",
                 first.left(24));
        sumcheck(first.contains(QStringLiteral("## Vue")), "正文并进来了");
        sumcheck(store->drafts().isEmpty(), "采纳之后草稿不在待审里了");
        sumcheck(store->categories().contains(QStringLiteral("前端")), "分类清单里有了它");

        /* 同一个分类再来一次：追加，不能覆盖 */
        const QString draft2 = store->writeDraft(QStringLiteral("20260921_0900"),
                                                 QStringLiteral("前端"),
                                                 QStringLiteral("## 汇总 c ~ d\n\n内容二"));
        const QString doc2 = store->adoptDraft(draft2);
        const QString merged = readIt(doc2);
        sumcheck(doc2 == docPath, "同分类第二次采纳并进同一份文档", doc2);
        sumcheck(merged.contains(QStringLiteral("内容一")) && merged.contains(QStringLiteral("内容二")),
                 "两批内容都在（追加不是覆盖）");
        sumcheck(h1Lines(merged) == 1, "文档从头到尾只有那一个大标题",
                 QStringLiteral("%1 个").arg(h1Lines(merged)));

        /* 丢弃只删草稿 */
        const QString draft3 = store->writeDraft(QStringLiteral("20260921_1000"),
                                                 QStringLiteral("生活"),
                                                 QStringLiteral("## 汇总 e ~ f\n\n买牛奶"));
        sumcheck(store->drafts().size() == 1, "丢弃之前待审里就是它");
        sumcheck(store->discardDraft(draft3), "丢弃能删掉草稿");
        sumcheck(store->drafts().isEmpty() && !store->categories().contains(QStringLiteral("生活")),
                 "丢弃不会留下任何文档");
        sumcheck(!store->discardDraft(docPath), "采纳那条路只认待审下面的文件（删不掉正式文档）");

        /* 分类名是模型给的，得洗成能当文件名的样子（'#' / '+' 这些是合法字符，留着） */
        const QString messy = store->writeDraft(QStringLiteral("20260921_1100"),
                                                QStringLiteral("  C++/前端 ## 指南 "),
                                                QStringLiteral("正文"));
        const QString base = QFileInfo(messy).fileName();
        sumcheck(!base.contains(QLatin1Char('/')) && !base.contains(QLatin1Char('\\'))
                     && !base.contains(QLatin1Char(':')),
                 "分类名里不能当文件名的字符被洗掉", base);
        sumcheck(!base.contains(QStringLiteral("未分类")) && base.contains(QStringLiteral("C++")),
                 "洗名没把有内容的分类名洗成「未分类」", base);
        sumcheck(store->adoptDraft(messy).endsWith(QStringLiteral("C++_前端 ## 指南.md")),
                 "采纳时用的就是洗过的名字", messy);
    }

    /* ------------------------------------------------------------------ */
    /* 5. 树                                                               */
    /* ------------------------------------------------------------------ */
    std::fputs("\n-- 树上的「文档」 --\n", stdout);
    {
        store->rescan();
        const QVariantList tree = store->tree();
        const QString docsKey = QStringLiteral("dir:") + QDir::cleanPath(store->docsRoot());
        const QVariantMap docs = findNode(tree, docsKey);
        sumcheck(!docs.isEmpty(), "树上有「文档」这个节点");
        sumcheck(docs.value(QStringLiteral("label")).toString() == QStringLiteral("文档"),
                 "节点名就是那个目录名", docs.value(QStringLiteral("label")).toString());
        /*
         * kind 必须是普通 folder：画成 imported 的话右键会给出
         * "移除此导入目录"，而它根本不在导入清单里（点了什么也不会发生）。
         */
        sumcheck(docs.value(QStringLiteral("kind")).toString() == QLatin1String("folder"),
                 "「文档」不是导入目录（右键不该给\"移除\"）",
                 docs.value(QStringLiteral("kind")).toString());
        sumcheck(treeHas(tree, QDir::cleanPath(docPath)), "采纳出来的文档在树上看得见");
        sumcheck(treeHas(tree, QStringLiteral("dir:")
                                       + QDir::cleanPath(store->docsRoot())
                                       + QStringLiteral("/待审")),
                 "待审那一层也在树上（草稿能直接点开看）");
    }

    /* ------------------------------------------------------------------ */
    /* 6. 归档                                                             */
    /* ------------------------------------------------------------------ */
    std::fputs("\n-- 归档 / 还原 --\n", stdout);
    {
        const QStringList before = [&]() {
            QStringList out;
            treeFiles(store->tree(), &out);
            return out;
        }();
        sumcheck(before.contains(clipFile), "归档之前那份剪贴板原文在树上");

        const int archived = store->archiveRange(today, today);
        sumcheck(archived == 1, "按区间收，收的就是那一份原文", QString::number(archived));

        store->rescan();
        const QStringList after = [&]() {
            QStringList out;
            treeFiles(store->tree(), &out);
            return out;
        }();
        sumcheck(!after.contains(clipFile), "归档之后它不在树上了");
        sumcheck(after.contains(QDir::cleanPath(docPath)), "「文档」不受归档影响");
        sumcheck(store->archivedFiles().size() == 1, "归档清单里能翻到它");
        sumcheck(store->sectionsInRange(today, today).isEmpty(),
                 "归档过的原文不会再被汇总第二遍");
        sumcheck(QFileInfo::exists(clipFile), "归档没把文件删掉（内容还在磁盘上）");

        sumcheck(store->unarchiveFile(clipFile), "能还原");
        const QStringList back = [&]() {
            QStringList out;
            treeFiles(store->tree(), &out);
            return out;
        }();
        sumcheck(back.contains(clipFile), "还原之后它又回到树上");
        sumcheck(store->archivedFiles().isEmpty(), "归档清单清空了");
    }

    /* ------------------------------------------------------------------ */
    /* 7. 整条流水线（接一个假模型服务，真发请求）                             */
    /* ------------------------------------------------------------------ */
    std::fputs("\n-- 汇总流水线（分批 / 合并 / 落草稿） --\n", stdout);
    if (sum && llm) {
        /* 模型那四项配置改了就写回原样（和翻译自检同一个约定：自检不留痕） */
        const QString savedMode = llm->mode();
        const QString savedBase = llm->apiBase();
        const QString savedKey = llm->apiKey();
        const QString savedModel = llm->model();

        ScriptedLlm mock;
        sumcheck(mock.listen(QHostAddress::LocalHost, 0), "假模型服务起来了",
                 QString::number(mock.serverPort()));
        llm->setMode(QStringLiteral("api"));
        llm->setApiBase(QStringLiteral("http://127.0.0.1:%1/v1").arg(mock.serverPort()));
        llm->setApiKey(QStringLiteral("test-key"));
        llm->setModel(QStringLiteral("mock-model"));
        /*
         * 假回复的正文都要够长：短于 kMinCategoryChars（80 字）的分类会被护栏
         * 收拢进「未分类」，那种草稿撑不起一份文档。
         */
        const QString pad = QStringLiteral("这一段够长，撑得起一个分类自己的正文，不是占位句。"
                                           "这一段够长，撑得起一个分类自己的正文，不是占位句。");
        /* 第 1、2 条是两批的分类结果（Qt 那类两批都有），第 3 条是那一次合并 */
        mock.replies = {
            QStringLiteral("=== 分类: Qt ===\n## 编译\n第一批里的内容\n%1\n"
                           "=== 分类: 网络 ===\n另一类的正文\n%1")
                .arg(pad),
            QStringLiteral("=== 分类: Qt ===\n第二批里的内容\n%1").arg(pad),
            QStringLiteral("## 编译\n合并之后的正文\n%1").arg(pad)
        };

        /* 两段各 3500 字：拼起来超过一批的 6000，必然切成两批 */
        const QString longOne = QStringLiteral("编译要用 jom。")
                              + QString(3500, QLatin1Char('A'));
        const QString longTwo = QStringLiteral("超时要看请求那一段。")
                              + QString(3500, QLatin1Char('B'));
        sumcheck(store->captureText(longOne), "喂进第一段长内容");
        sumcheck(store->captureText(longTwo), "喂进第二段长内容");

        /* 等一轮跑完：每轮都真发请求，得走事件循环 */
        auto waitRun = [&]() {
            QEventLoop loop;
            int n = -1;
            const auto conn = QObject::connect(sum, &Summarizer::runFinished, &loop,
                                               [&](int got) {
                                                   n = got;
                                                   loop.quit();
                                               });
            QTimer::singleShot(20000, &loop, &QEventLoop::quit);
            loop.exec();
            QObject::disconnect(conn);
            return n;
        };

        /*
         * A. 其中一批失败。真跑本地 gemma-3-4b 就是这个样子：第 1 批回来了、
         * 第 2 批 120 秒超时，而当时的写法是整轮作废 —— 已经整理好的那一批
         * 跟着一起扔，跑完 0 份草稿。现在要求：剩下的照样落地，结论里说清几批没成。
         */
        const QStringList threeReplies = mock.replies;
        mock.failOn = 0;
        mock.served = 0;
        mock.requests.clear();
        mock.replies = { QStringLiteral("=== 分类: Qt ===\n只有这一类的正文\n%1").arg(pad) };
        sum->start(today, today);
        const int survived = waitRun();
        sumcheck(mock.served == 2, "第一批失败没掐掉第二批（两批都发出去了）",
                 QString::number(mock.served));
        sumcheck(survived == 1, "成了的那一批照样落成了草稿", QString::number(survived));
        sumcheck(sum->status().contains(QStringLiteral("1 批没整理出来")),
                 "结论里说清了几批没成", sum->status());
        for (const QVariant &value : store->drafts())
            store->discardDraft(value.toMap().value(QStringLiteral("path")).toString());

        /* B. 都不失败：2 批分类 + 1 次合并 */
        mock.failOn = -1;
        mock.served = 0;
        mock.requests.clear();
        mock.replies = threeReplies;
        sum->start(today, today);
        const int drafts = waitRun();

        sumcheck(drafts == 2, "跑完写了 2 份草稿（Qt / 网络）", QString::number(drafts));
        sumcheck(mock.served == 3, "一共发了 3 条请求：2 批分类 + 1 次合并",
                 QString::number(mock.served));
        sumcheck(sum->totalSteps() == 3 && sum->doneSteps() == 3, "进度走到了 3/3",
                 QStringLiteral("%1/%2").arg(sum->doneSteps()).arg(sum->totalSteps()));
        sumcheck(!sum->busy(), "跑完 busy 放下了");
        if (mock.requests.size() == 3) {
            sumcheck(mock.requests.at(0).contains(QStringLiteral("已有分类"))
                         && mock.requests.at(0).contains(QStringLiteral("### ")),
                     "分类那条请求里带着已有分类和原文");
            sumcheck(mock.requests.at(2).contains(QStringLiteral("合并")),
                     "第三次发的是合并请求");
            sumcheck(!mock.requests.at(0).contains(QStringLiteral("BBBB"))
                         || !mock.requests.at(1).contains(QStringLiteral("AAAA")),
                     "两批原文没有混成一条请求");
        }

        const QVariantList pending = store->drafts();
        sumcheck(pending.size() == 2, "待审里就是那两份草稿", QString::number(pending.size()));
        QString qtBody;
        QString netBody;
        for (const QVariant &value : pending) {
            const QVariantMap row = value.toMap();
            const QString text = readIt(row.value(QStringLiteral("path")).toString());
            if (row.value(QStringLiteral("category")).toString() == QLatin1String("Qt"))
                qtBody = text;
            else
                netBody = text;
        }
        sumcheck(qtBody.contains(QStringLiteral("合并之后的正文")), "Qt 那份用的是合并后的正文",
                 qtBody.left(60));
        sumcheck(!qtBody.contains(QStringLiteral("第一批里的内容"))
                     && !qtBody.contains(QStringLiteral("第二批里的内容")),
                 "合并结果把两批原文换掉了（没留残骸）");
        sumcheck(netBody.contains(QStringLiteral("另一类的正文")), "只有一个批的分类没发合并");
        sumcheck(qtBody.contains(QStringLiteral("## 汇总 ")) && qtBody.contains(QStringLiteral("批次")),
                 "草稿带着区间和批次号（采纳之后还能查是哪一批）");

        /* 这两份草稿不留到下一节：下一节自己造一份，"清单是 1 行"那条断言才简单 */
        for (const QVariant &value : store->drafts())
            store->discardDraft(value.toMap().value(QStringLiteral("path")).toString());
        sumcheck(store->drafts().isEmpty(), "这一节的草稿收干净了");

        /*
         * C. 分类收拢：模型一口气报 5 个新分类（真跑本地 gemma-3-4b 时它报了
         * 28 个 —— 把一份 CSS 粘贴里每个小标题都当成分类）。一轮最多留 3 个新的，
         * 剩下两个的正文并进「未分类」，一份都不能丢。
         */
        mock.served = 0;
        mock.requests.clear();
        /* 五类正文有大有小：最大的三个留下，最小的戊类一定被收拢 */
        const QString five = QStringLiteral("=== 分类: 甲类 ===\n%1\n%1\n%1\n"
                                            "=== 分类: 乙类 ===\n%1\n%1\n"
                                            "=== 分类: 丙类 ===\n%1\n"
                                            "=== 分类: 丁类 ===\n%1\n"
                                            "=== 分类: 戊类 ===\n戊类只有一句话")
                                 .arg(pad);
        mock.replies = { five };
        sum->start(today, today);
        const int foldedDrafts = waitRun();
        sumcheck(foldedDrafts == 4, "5 个新分类收成 4 份（3 个新的 + 未分类）",
                 QString::number(foldedDrafts));
        sumcheck(sum->status().contains(QStringLiteral("2 个分类")),
                 "结论里说了收拢掉几个", sum->status());
        int foldedMarks = 0;
        bool sawSmallest = false;
        for (const QVariant &value : store->drafts()) {
            const QVariantMap row = value.toMap();
            /* 这里要比字符串，不能用 QLatin1String —— 中文放 Latin1 里永不相等 */
            if (row.value(QStringLiteral("category")).toString() != QStringLiteral("未分类"))
                continue;
            const QString text = readIt(row.value(QStringLiteral("path")).toString());
            foldedMarks = text.count(QStringLiteral("【"));
            sawSmallest = text.contains(QStringLiteral("【戊类】"));
        }
        sumcheck(foldedMarks >= 2 && sawSmallest,
                 "被收拢的分类在「未分类」里留了自己原来叫什么",
                 QStringLiteral("%1 处标记 / 草稿分类：%2")
                     .arg(foldedMarks)
                     .arg([&]() {
                         QStringList names;
                         for (const QVariant &v : store->drafts())
                             names << v.toMap().value(QStringLiteral("category")).toString();
                         return names.join(QStringLiteral("、"));
                     }()));
        for (const QVariant &value : store->drafts())
            store->discardDraft(value.toMap().value(QStringLiteral("path")).toString());

        /*
         * 一段比一批还长的原文（2026-09-21 真跑本地 gemma-3-4b 就是栽在这上面：
         * 一段 2.5 万字独占一批，120 秒超时）。现在必须按行劈开，
         * 判据用**发出去的请求有多大** —— 那才是模型实际承受的。
         */
        QString huge;
        for (int i = 0; i < 260; ++i)
            huge += QStringLiteral("第 %1 行：编译要用 jom，超时要看请求那一段。\n").arg(i);
        sumcheck(huge.size() > 7000, "造出了一段比一批还长的原文", QString::number(huge.size()));
        sumcheck(store->captureText(huge), "喂进这段超长原文");

        mock.served = 0;
        mock.requests.clear();
        mock.replies = { QStringLiteral("=== 分类: 杂项 ===\n劈开之后各段的内容") };
        sum->start(today, today);
        const int bigDrafts = waitRun();

        int biggest = 0;
        for (const QString &raw : std::as_const(mock.requests))
            biggest = qMax(biggest, raw.toUtf8().size());
        sumcheck(bigDrafts >= 1, "劈开之后照样跑出了草稿", QString::number(bigDrafts));
        sumcheck(mock.served >= 3, "那段被劈成了好几批", QString::number(mock.served));
        sumcheck(biggest < 24000, "单条请求没超过一批该有多大（模型承受的住）",
                 QStringLiteral("最大 %1 字节").arg(biggest));
        for (const QVariant &value : store->drafts())
            store->discardDraft(value.toMap().value(QStringLiteral("path")).toString());

        llm->setMode(savedMode);
        llm->setApiBase(savedBase);
        llm->setApiKey(savedKey);
        llm->setModel(savedModel);
        sumcheck(llm->apiBase() == savedBase, "模型配置按原样写回去了");
    } else {
        sumout("没给 Sum 单例或 Llm 单例，流水线那一节跳过");
    }

    /* ------------------------------------------------------------------ */
    /* 8. 设置面板那两栏                                                     */
    /* ------------------------------------------------------------------ */
    std::fputs("\n-- 设置面板「汇总」「归档」 --\n", stdout);
    if (qmlRoot) {
        const QString pending = store->writeDraft(QStringLiteral("20260921_1200"),
                                                  QStringLiteral("Qt"),
                                                  QStringLiteral("## 汇总 g ~ h\n\n## 编译\n内容"));
        sumcheck(!pending.isEmpty(), "先造一份待审草稿给界面画", pending);

        QObject *panel = qmlRoot->findChild<QObject *>("settingsPanel");
        sumcheck(panel != nullptr, "按 objectName 找到了设置面板");
        if (panel) {
            sumcheck(!panel->property("opened").toBool(), "开之前面板是收着的",
                     QString::number(panel->property("opened").toBool()));
            const bool called = QMetaObject::invokeMethod(
                panel, "openSection", Q_ARG(QVariant, QStringLiteral("summarize")));
            settle(260);
            sumcheck(called, "openSection(QVariant) 这一枪真的打到了");
            /*
             * 这一条是这一节里唯一"面板确实摆在屏幕上"的凭据，别省。
             *
             * 踩过的那次：placeOverHost() 里用了 `Screen.virtualGeometry`（那块窗是
             * QQuickWidget 里造的顶层 Window，show 之前 attached Screen 的属性全是
             * undefined）→ openSection 从那一行就抛了，visible / reloadSummarize
             * 都没轮到执行，而**QML 的报错不进自检日志**（grep 什么都 grep 不到）。
             * 当时只有"草稿清单 1 行"那条间接红了一下 —— 判 section 的那条照样绿
             * （section 在抛错之前就赋值了）。所以要直接判 visible，不要靠副作用。
             */
            sumcheck(panel->property("opened").toBool(), "面板真的开出来了（visible）",
                     panel->property("section").toString());
            sumcheck(panel->property("section").toString() == QLatin1String("summarize"),
                     "开到了「汇总」那一栏", panel->property("section").toString());

            QObject *column = panel->findChild<QObject *>("summarizeColumn");
            sumcheck(column != nullptr, "找得到「汇总」那一栏");
            sumcheck(column && column->property("visible").toBool(), "那一栏是可见的");
            const qreal height = column ? column->property("implicitHeight").toReal() : 0.0;
            sumcheck(height > 320, "那一栏画出了内容（塌成 0 就是绑定写错了）",
                     QString::number(height));
            const QVariantList rows = panel->property("draftRows").toList();
            sumcheck(rows.size() == 1, "面板自己那份草稿清单是 1 行", QString::number(rows.size()));

            /* 想亲眼看这两栏时再抓图（不给 SMARTCLIP_SUM_SHOT 就什么都不写） */
            const QString shot = qEnvironmentVariable("SMARTCLIP_SUM_SHOT");
            if (!shot.isEmpty()) {
                grabSurface(panel, shot + QStringLiteral("-summarize.png"));
                grabDesktop(shot + QStringLiteral("-desktop.png"));
                QMetaObject::invokeMethod(panel, "openSection",
                                          Q_ARG(QVariant, QStringLiteral("archive")));
                settle(260);
                grabSurface(panel, shot + QStringLiteral("-archive.png"));
                sumout(QStringLiteral("面板已抓图：%1-summarize.png / -archive.png / -desktop.png")
                           .arg(shot));
            }

            QMetaObject::invokeMethod(panel, "openSection", Q_ARG(QVariant, QStringLiteral("archive")));
            settle(260);
            sumcheck(panel->property("section").toString() == QLatin1String("archive"),
                     "开到了「归档」那一栏");
            QObject *archived = panel->findChild<QObject *>("archiveColumn");
            sumcheck(archived && archived->property("visible").toBool(),
                     "「归档」那一栏是可见的");
            /*
             * 空列表那一栏本来就只有"标题 + 两句说明 + 一个空框"，60 是
             * "画出来了"的下限，不是"好看"的门槛 —— 这条钉的是绑定写错时
             * 整栏塌成 0 那种事故。
             */
            sumcheck(archived && archived->property("implicitHeight").toReal() > 60,
                     "「归档」那一栏画出了内容",
                     archived ? QString::number(archived->property("implicitHeight").toReal())
                              : QStringLiteral("没找到那一栏"));

            /* 两份清单得在 Store 变化之后自己重取（它们是函数不是属性，没人替谁绑） */
            store->discardDraft(pending);
            settle(260);
            sumcheck(panel->property("draftRows").toList().isEmpty(),
                     "草稿丢了之后面板自己重取了清单");

            QMetaObject::invokeMethod(qmlRoot, "closeSettings");
            settle(120);
        }
    } else {
        sumout("没给 qmlRoot，界面那一节跳过");
    }

    /* 换回原来的保存目录：元数据会照实目录重建（见文件头那段） */
    sumcheck(store->setRootPath(savedRoot), "自检跑完把保存目录换回去", savedRoot);

    std::fputs("\n== 汇总自检结束 ==\n", stdout);
    sumout(QStringLiteral("通过 %1 项，失败 %2 项").arg(gSumPassed).arg(gSumFailed));
    return gSumFailed;
}
