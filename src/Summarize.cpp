#include "Summarize.h"

#include "ClipboardStore.h"
#include "PinOcr.h"
#include "Translate.h"

#include <QDateTime>
#include <QPointer>
#include <QRunnable>
#include <QSettings>
#include <QThreadPool>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>

namespace {

/*
 * 一批喂多少字。
 *
 * 6000 字是照着"一次请求稳稳当当地在 120 秒内回来"定的：DeepSeek 那一档
 * 生成几千字要十几到几十秒，再长就要开始赌超时；而一批的产出只是一个分类的
 * 几段笔记，切小一点对质量没坏处（合并那一步本来就是为切小了准备的）。
 */
constexpr int kBatchChars = 6000;

/*
 * 一轮最多发几条。
 *
 * 点一下按钮就悄悄烧掉两百次请求是不能接受的：超了直接让他缩短区间。
 * 30 批 ≈ 18 万字 ≈ 一个月的高强度复制，正常用碰不到。
 */
constexpr int kMaxBatches = 30;

const QString kFallbackCategory = QStringLiteral("未分类");

/*
 * 草稿正文里那行出处。区间已经写在上一级的小标题上了，这里就不重复，
 * 补上"吃进去多少段、跳过多少图片、哪一批"—— 过半年想回去查，靠的是批次号。
 */
QString provenance(const QString &runId, int sections, const QString &imageNote) {
    QString line = QStringLiteral("> 取自 %1 段原文 · 批次 %2").arg(sections).arg(runId);
    if (!imageNote.isEmpty())
        line += QStringLiteral(" · %1").arg(imageNote);
    return line + QStringLiteral("\n\n");
}

/* 草稿文件名前缀那个批次号。**不带 '-'**：草稿采纳时按第一个 '-' 拆出分类名 */
QString stampForRun() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
}

/*
 * 这一行是不是 "=== 分类: 前端 ===" 那样的分隔标记。
 *
 * 容忍三种写法上的漂移：全角冒号、结尾那串等号漏了、标记前后有空格。
 * 这几种是模型实际会给的（提示词里写的是半角冒号，它一顺手就换成中文冒号）。
 * 两个细节：冒号取**第一个**（分类名自己也可能带冒号），并且这一行必须真的
 * 写着"分类"两个字 —— 不然正文里出现 "=== 参见: 附录 ===" 就被当成标记，
 * 那一行内容悄无声息地没了。认不出来的一律当正文。
 */
bool markerLine(const QString &line, QString *category) {
    const QString trimmed = line.trimmed();
    if (!trimmed.startsWith(QStringLiteral("===")) || !trimmed.contains(QStringLiteral("分类")))
        return false;
    const int half = trimmed.indexOf(QLatin1Char(':'));
    const int full = trimmed.indexOf(QString::fromUtf8("："));
    const int colon = half < 0 ? full : (full < 0 ? half : qMin(half, full));
    if (colon < 0 || colon == trimmed.size() - 1)
        return false;
    QString name = trimmed.mid(colon + 1).split(QStringLiteral("===")).first().trimmed();
    if (name.isEmpty())
        return false;
    *category = name;
    return true;
}

/* 模型有时把整段正文裹在 ``` 里：那两行围栏不是内容 */
QString unwrapFence(const QString &body) {
    QString text = body.trimmed();
    if (!text.startsWith(QStringLiteral("```")))
        return text;
    const int firstBreak = text.indexOf(QLatin1Char('\n'));
    if (firstBreak < 0 || !text.endsWith(QStringLiteral("```")))
        return text;
    text = text.mid(firstBreak + 1);
    const int lastFence = text.lastIndexOf(QStringLiteral("```"));
    if (lastFence < 0)
        return text.trimmed();
    return text.left(lastFence).trimmed();
}

/*
 * 一段比一批还长时按行劈开。
 *
 * 原来写的是"让它独占一批，断在段中间不如不断" —— 真实数据把这条打穿了：
 * 2026-09-21 那天有一段 2.5 万字的原文（一整份日志），独占一批等于把批上限
 * 形同虚设，本地 gemma-3-4b 跑到 120 秒超时（实测：中断在 1/2 步）。
 * 按行劈至少不会把一条命令、一行代码切成半截；单行本身就超上限的那种不管，
 * 那种东西喂给模型也没意义。
 */
void splitOversizedBlock(const QString &block, int limit, QStringList *out) {
    if (block.size() <= limit) {
        out->append(block);
        return;
    }
    QString current;
    const QStringList lines = block.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (!current.isEmpty() && current.size() + line.size() + 1 > limit) {
            out->append(current);
            current.clear();
        }
        if (!current.isEmpty())
            current += QLatin1Char('\n');
        current += line;
    }
    if (!current.trimmed().isEmpty())
        out->append(current);
}

/*
 * 一轮最多让模型新建几个分类。
 *
 * 提示词里求过它"优先归入已有分类、新分类名要短通用、不要为单独一条内容开分类"
 * —— 2026-09-21 真跑本地 gemma-3-4b 直接回了 28 个分类（把一份 2.5 万字的 CSS
 * 粘贴里每个小标题都当成一个分类），那种待审清单没人会去逐份看。
 * 所以这条不靠模型自觉：超出来的正文一律并进「未分类」，一份都不丢，
 * 只是名字得他自己改。
 */
constexpr int kMaxNewCategories = 3;

/* 正文比这还短的分类撑不起一份文档（模型常拿一句"（样式表的变量定义）"占位） */
constexpr int kMinCategoryChars = 80;

/*
 * 一轮最多认几张图。
 *
 * 和 kMaxBatches 同一个道理，只是这儿疼的不是超时是**等**：OCR 一张几十到几百
 * 毫秒，多模态一张就是一次请求（本地模型十几秒）。一天截图上百张时整轮会跑到
 * 没完，所以超出的不认 —— 但**照样报数**（状态里、草稿那行出处里都写得清）。
 */
constexpr int kMaxShotsPerRun = 50;

/*
 * 认出来的那段文字在正文里长什么样：段首格式和原文段**一模一样**，
 * 后面才能和复制来的内容排进同一条时间线（见 sortKeyOf）。
 * 尾巴上那句"（OCR 认出来的截图）"是写给模型看的，也是写给半年后审稿的人看的。
 */
QString shotBlock(const QString &day, const QString &time, const QString &engine,
                  const QString &text) {
    return QStringLiteral("### %1 %2（%3 认出来的截图）\n\n%4").arg(day, time, engine, text);
}

/* 一块的排序键 = 它自己那行段首（"### yyyy-MM-dd HH:mm:ss…"）：字典序就是时间序 */
QString sortKeyOf(const QString &block) {
    const int newline = block.indexOf(QLatin1Char('\n'));
    return newline < 0 ? block : block.left(newline);
}

/*
 * 选项只认这三个值。脏配置（手改注册表、旧版本留下的别的字符串）一律回落到
 * "ocr"：默认档是它 —— 本机离线、不烧 token，配错了也不至于偷偷发起一堆请求。
 */
QString cleanImageMode(const QString &value) {
    if (value == QLatin1String("none") || value == QLatin1String("vision"))
        return value;
    return QStringLiteral("ocr");
}

/*
 * "ocr" 那一档的工人：读盘 + 认字都在线程池里（PinOcr 那两个函数都是阻塞的）。
 *
 * 和 PinWindow 里那个 OcrTask 同一套做法 —— 包括用 QPointer 兜住"认到一半这一轮
 * 已经没了"（他点了停，或者切了区间重开）。结果回主线程才敢动状态。
 */
class ShotOcrTask final : public QRunnable {
public:
    ShotOcrTask(Summarizer *sum, const QString &path, const QString &engine,
                const QString &command)
        : m_sum(sum), m_path(path), m_engine(engine), m_command(command) {
        setAutoDelete(true);
    }

    void run() override {
        QString error;
        QString text;
        const QImage image(m_path);
        if (image.isNull()) {
            error = QStringLiteral("这张图读不出来");
        } else {
            QVariantList lines;
            if (m_engine == QLatin1String("ppocr"))
                lines = PinOcr::recognizeWithProgram(image, m_command, &error);
            else
                lines = PinOcr::recognize(image, &error);
            QStringList rows;
            for (const QVariant &value : lines)
                rows.append(value.toMap().value(QStringLiteral("text")).toString());
            text = rows.join(QLatin1Char('\n'));
            if (text.trimmed().isEmpty() && error.trimmed().isEmpty())
                error = QStringLiteral("图上没认出字");
        }
        if (m_sum.isNull())
            return;
        const QPointer<Summarizer> sum = m_sum;
        QMetaObject::invokeMethod(sum, [sum, text, error]() {
            if (sum)
                sum->applyShotText(text, error);
        });
    }

private:
    QPointer<Summarizer> m_sum;
    QString m_path;
    QString m_engine;
    QString m_command;
};
}  // namespace

Summarizer::Summarizer(ClipboardStore *store, LlmClient *llm, QObject *parent)
    : QObject(parent), m_store(store), m_llm(llm) {
    /*
     * 图片段怎么处理。默认 "ocr"：本机离线、不烧 token，装上就能用；
     * 要发给视觉模型的那档得他自己去「设置 → 汇总」切（每张图一次请求）。
     */
    QSettings settings;
    m_imageMode = cleanImageMode(settings.value(QStringLiteral("summarize/imageMode"),
                                                 QStringLiteral("ocr")).toString());

    if (!m_llm)
        return;

    /*
     * token 对不上的一律丢掉（和校验那套同一个契约）。
     *
     * 一轮汇总要串好几十条请求，期间用户完全可能又点了一次翻译 ——
     * LlmClient 是共用的，finished 会带上别人的 token。
     */
    connect(m_llm, &LlmClient::finished, this, &Summarizer::onFinished);
    connect(m_llm, &LlmClient::failed, this, &Summarizer::onFailed);
}

void Summarizer::setBusy(bool on) {
    if (m_busy == on)
        return;
    m_busy = on;
    emit busyChanged();
}

void Summarizer::setStatus(const QString &text) {
    if (m_status == text)
        return;
    m_status = text;
    emit statusChanged();
}

/*
 * 换图片段的处理方式，改一下立刻落盘（设置面板绑的就是这条属性，没有"应用"那一步，
 * 和 LlmClient 那几项一个约定）。
 */
void Summarizer::setImageMode(const QString &value) {
    const QString clean = cleanImageMode(value);
    if (m_imageMode == clean)
        return;
    m_imageMode = clean;
    QSettings().setValue(QStringLiteral("summarize/imageMode"), clean);
    emit settingsChanged();
}

/* ------------------------------------------------------------------ */
/* 提示词                                                              */
/* ------------------------------------------------------------------ */

QString Summarizer::classifyPrompt(const QString &existingCategories) const {
    const QString categories = existingCategories.trimmed().isEmpty()
                                   ? QStringLiteral("（还没有已有分类，可以按需新建）")
                                   : existingCategories.trimmed();
    return QStringLiteral(
               "你是中文笔记整理助手。用户会给你一段时间里他从各处复制下来的原文片段，"
               "每段前面标着它自己的日期和时间。\n"
               "\n"
               "分类规则：\n"
               "1) 已有分类：%1\n"
               "   优先归进这些已有分类。确实不属于任何一个时才可以新建；"
               "新分类名要短（2~6 字）、通用，不要为单独一条内容开分类。\n"
               "2) 一条内容只归一个分类。\n"
               "\n"
               "输出格式（严格遵守：不要任何解释、不要代码块围栏、不要大标题）：\n"
               "=== 分类: 分类名 ===\n"
               "（该分类整理后的 Markdown 正文）\n"
               "=== 分类: 另一个分类 ===\n"
               "（……）\n"
               "\n"
               "整理要求：\n"
               "- 每个分类内部用 ## 小标题把同类内容归到一起\n"
               "- 重复的片段合并成一条；没有长期价值的（一句闲聊、半成品、看不懂的残句）直接丢掉\n"
               "- 命令、代码、配置、报错原文**一字不改**地放进代码块，不要顺手修正\n"
               "- 每条内容末尾用 `（来源 yyyy-MM-dd HH:mm:ss）` 标它自己的时间\n"
               "- 段首标了「（… 认出来的截图）」的那段是从图片里认出来的，本身可能带识别错误："
               "照原样收录，残缺的地方不要替他补全\n"
               "- 原文里没有的东西不要编")
        .arg(categories);
}

QString Summarizer::mergePrompt(const QString &category) const {
    return QStringLiteral(
               "下面是分类「%1」分批整理出来的几份 Markdown，它们说的可能是同一件事。\n"
               "请合并成一份：\n"
               "- 同名的小标题合并；重复的条目去掉，保留信息更全的那条\n"
               "- 每个条目后面的 `（来源 …）` 一律保留，多条就并排写\n"
               "- 不要新增原文里没有的内容，不要改变原意，不要改分类名\n"
               "- 直接输出 Markdown 正文：不要解释、不要代码块围栏、不要 # 大标题、"
               "也不要再写 === 分类: === 这种标记")
        .arg(category.trimmed().isEmpty() ? kFallbackCategory : category.trimmed());
}

QVariantList Summarizer::parseReply(const QString &reply) const {
    QVariantList out;
    QString category;
    QString body;
    bool started = false;

    const QStringList lines = reply.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line =
            raw.endsWith(QLatin1Char('\r')) ? raw.left(raw.size() - 1) : raw;
        QString found;
        if (markerLine(line, &found)) {
            if (started) {
                const QString text = unwrapFence(body);
                if (!text.isEmpty()) {
                    out.append(QVariantMap{{QStringLiteral("category"),
                                           category.trimmed().isEmpty()
                                               ? kFallbackCategory
                                               : category.trimmed()},
                                          {QStringLiteral("body"), text}});
                }
            }
            category = found;
            body.clear();
            started = true;
        } else if (started) {
            body += line + QLatin1Char('\n');
        }
        /* 第一个标记之前的那些"好的，下面是…"：不是内容，丢掉 */
    }
    if (started) {
        const QString text = unwrapFence(body);
        if (!text.isEmpty()) {
            out.append(QVariantMap{{QStringLiteral("category"),
                                   category.trimmed().isEmpty() ? kFallbackCategory
                                                                : category.trimmed()},
                                  {QStringLiteral("body"), text}});
        }
    }

    /*
     * 一个标记都没认出来 -> 整份回复当"未分类"收着。
     *
     * 这一步不能省：模型没按格式来是常事，那时光丢掉就等于把这一批的整理结果
     * 全扔了，而它内容往往是对的。宁可给他一个名字叫"未分类"的草稿去改，
     * 也不要什么都不剩。
     */
    if (out.isEmpty() && !reply.trimmed().isEmpty()) {
        out.append(QVariantMap{{QStringLiteral("category"), kFallbackCategory},
                               {QStringLiteral("body"), unwrapFence(reply)}});
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* 这一轮                                                             */
/* ------------------------------------------------------------------ */

void Summarizer::start(const QString &fromDate, const QString &toDate) {
    if (m_busy) {
        setStatus(QStringLiteral("上一轮还没跑完（可以点「停」）"));
        return;
    }
    if (!m_store || !m_llm) {
        setStatus(QStringLiteral("汇总功能没接上"));
        return;
    }
    const QString from = fromDate.trimmed();
    const QString to = toDate.trimmed();
    if (from.isEmpty() || to.isEmpty() || from > to) {
        setStatus(QStringLiteral("区间不对：开始日期不能晚于结束日期"));
        return;
    }

    const QVariantList rows = m_store->sectionsInRange(from, to);
    if (rows.isEmpty()) {
        setStatus(QStringLiteral("%1 ~ %2 没有可汇总的剪贴板内容").arg(from, to));
        return;
    }

    /*
     * 这一轮的状态先归零：下面两处是往里**加**的，留着上一轮的数会更糟
     * （草稿数错、进度倒退，而这两种都比直接不跑难查）。
     */
    m_runId = stampForRun();
    m_from = from;
    m_to = to;
    m_categories = m_store->categories().join(QStringLiteral("、"));
    m_sections = 0;
    m_failedSteps = 0;
    m_collapsed = 0;
    m_lastError.clear();
    m_bucket.clear();
    m_order.clear();
    m_blocks.clear();
    m_shots.clear();
    m_shotBlocks.clear();
    m_nextShot = 0;
    m_shotOk = 0;
    m_shotFailed = 0;
    m_shotSkipped = 0;
    m_inShots = false;
    m_queue.clear();
    m_drafts = 0;
    m_doneSteps = 0;
    m_totalSteps = 0;
    m_merging = false;
    m_stopping = false;
    m_token.clear();

    /*
     * 先把原文段收齐，图片段单独收成 m_shots —— 两段活儿不能并成一段：
     * 认图是一张一张来的（本机 OCR 几百毫秒，多模态一次请求），而切批要的
     * 是"所有段已经按时间排好"。所以现在只攒料，认完才进 startBatchPhase。
     *
     * 切批按"段"切，不从一行中间断开：一段被劈成两半，模型两边各看到半条，
     * 整理出来就是两条残缺的笔记。一段本身比一批还长时按行劈开（见
     * splitOversizedBlock 里那次翻车）。
     */
    for (const QVariant &row : rows) {
        const QVariantMap map = row.toMap();
        const QString text = map.value(QStringLiteral("text")).toString();
        QString current;
        const QStringList lines = text.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (line.startsWith(QStringLiteral("### ")) && !current.trimmed().isEmpty()) {
                m_blocks.append(current.trimmed());
                current.clear();
            }
            current += line + QLatin1Char('\n');
        }
        if (!current.trimmed().isEmpty())
            m_blocks.append(current.trimmed());

        m_sections += map.value(QStringLiteral("count")).toInt();
        const QString day = map.value(QStringLiteral("dateKey")).toString();
        for (const QVariant &value : map.value(QStringLiteral("imageList")).toList()) {
            const QVariantMap shot = value.toMap();
            Shot item;
            item.day = day;
            item.time = shot.value(QStringLiteral("time")).toString();
            item.path = shot.value(QStringLiteral("path")).toString();
            m_shots.append(item);
        }
    }

    setBusy(true);

    if (m_imageMode == QLatin1String("none") || m_shots.isEmpty()) {
        startBatchPhase();
        return;
    }

    /*
     * 超出上限的砍在尾巴上（m_shots 本来就是按日期先后收的）：宁可认最早那几张，
     * 也不打乱他"这段先汇总"的预期。砍掉几张在状态里、在草稿那行出处里都报得出来。
     */
    if (m_shots.size() > kMaxShotsPerRun) {
        m_shotSkipped = m_shots.size() - kMaxShotsPerRun;
        m_shots = m_shots.mid(0, kMaxShotsPerRun);
    }

    m_inShots = true;
    m_doneSteps = 0;
    m_totalSteps = m_shots.size();
    recognizeNextImage();   /* 状态由它写：那一句本来就带着"第几张 / 共几张" */
}

/*
 * 认下一张。一次只让一张在飞：
 *   * OCR 那条虽然不占网络，但 PP-OCR 是**每张起一个程序**，并发起来机器就没了；
 *   * 多模态那条要守 LlmClient 的 token 契约（同时两条回来分不清是谁的）。
 */
void Summarizer::recognizeNextImage() {
    if (m_stopping) {
        m_inShots = false;
        m_shots.clear();
        advance();   /* 走原来那条"已停在第 N/M 步"的收尾 */
        return;
    }
    if (m_nextShot >= m_shots.size()) {
        m_inShots = false;
        startBatchPhase();
        return;
    }

    const Shot shot = m_shots.at(m_nextShot);
    if (m_imageMode == QLatin1String("vision"))
        m_token = m_llm->recognizeFile(shot.path, QStringLiteral("note"));
    else
        /* 本机 OCR 是阻塞调用（一张几十到几百毫秒），搬到线程池里，别冻住界面 */
        QThreadPool::globalInstance()->start(new ShotOcrTask(
            this, shot.path, m_llm->pinOcrEngine(), m_llm->pinOcrRunner()));

    /* 状态在发出去之后才写：那两条路自己都会改状态，我们这句要盖在它后面 */
    setStatus(QStringLiteral("正在汇总 %1 ~ %2：认第 %3/%4 张截图（%5）…")
                  .arg(m_from, m_to)
                  .arg(m_nextShot + 1)
                  .arg(m_shots.size())
                  .arg(shotKindLabel()));
}

void Summarizer::applyShotText(const QString &text, const QString &error) {
    onImageRecognized(text, error);
}

/*
 * 一张有结果了。
 *
 * 没认出字的（读不出来 / 引擎报错 / 图上确实没字）**不留正文**，只记一笔数：
 * 往正文里塞一句"（没认出来）"，模型会把它当成内容抄进草稿 —— 那比少一段更糟。
 */
void Summarizer::onImageRecognized(const QString &text, const QString &error) {
    if (!m_inShots)
        return;   /* 点了停、或者已经切到下一阶段才回来的：丢掉 */

    const Shot shot = m_shots.value(m_nextShot);
    ++m_nextShot;
    ++m_doneSteps;
    emit progressChanged();

    const QString body = text.trimmed();
    const QString why = error.trimmed();
    if (!why.isEmpty() || body.isEmpty()) {
        ++m_shotFailed;
        if (!why.isEmpty())
            m_lastError = why;
    } else {
        ++m_shotOk;
        m_shotBlocks.append(shotBlock(shot.day, shot.time, shotKindLabel(), body));
    }
    recognizeNextImage();
}

/*
 * 图片阶段结束（这一轮没有图要认时也走这儿）：认出来的块并进原文，切批开跑。
 *
 * 是**并进排序**而不是拼在最后 —— 段首那行就是时间，排完截图会落回它自己那个
 * 时间点上；拼在尾巴上等于把晚上十一点的截图当成这天最后一条内容喂给模型。
 */
void Summarizer::startBatchPhase() {
    QStringList blocks = m_blocks;
    blocks += m_shotBlocks;
    std::stable_sort(blocks.begin(), blocks.end(), [](const QString &a, const QString &b) {
        return sortKeyOf(a) < sortKeyOf(b);
    });

    QList<Task> tasks;
    Task batch;
    QStringList pieces;
    for (const QString &block : std::as_const(blocks))
        splitOversizedBlock(block, kBatchChars, &pieces);
    for (const QString &block : std::as_const(pieces)) {
        if (!batch.text.isEmpty() && batch.text.size() + block.size() > kBatchChars) {
            tasks.append(batch);
            batch = Task();
        }
        if (!batch.text.isEmpty())
            batch.text += QStringLiteral("\n\n");
        batch.text += block;
    }
    if (!batch.text.isEmpty())
        tasks.append(batch);

    if (tasks.size() > kMaxBatches) {
        setBusy(false);
        setStatus(QStringLiteral("这段时间的内容太多（要分 %1 批，上限 %2）"
                                "—— 把区间缩短一点再汇总")
                      .arg(tasks.size())
                      .arg(kMaxBatches));
        emit runFinished(0);
        return;
    }
    if (tasks.isEmpty()) {
        /*
         * 只有截图、又一张都没认出字：得说一句人话。原来这条路会直接停在
         * "正在汇总…"上不放 busy（界面那个按钮一直灰着），因为过去不可能
         * 出现"一批都没有"—— 现在有了图片这一档，就可能。
         */
        setBusy(false);
        setStatus(QStringLiteral("%1 ~ %2：%3，没有可整理的内容")
                      .arg(m_from, m_to)
                      .arg(shotNote().isEmpty() ? QStringLiteral("没有内容") : shotNote()));
        emit runFinished(0);
        return;
    }

    m_queue = tasks;
    /* 认图那几步已经记在 doneSteps 上了，分母要在它上面接着数，不然进度条会倒退 */
    m_totalSteps = m_doneSteps + tasks.size();
    setStatus(QStringLiteral("正在汇总 %1 ~ %2（共 %3 段，分 %4 批问）")
                  .arg(m_from, m_to)
                  .arg(m_sections + m_shotOk)
                  .arg(tasks.size()));
    pump();
}

/* 出现在正文 / 状态 / 草稿出处里的那个引擎名（短，且和设置面板上说的对得上） */
QString Summarizer::shotKindLabel() const {
    if (m_imageMode == QLatin1String("vision"))
        return QStringLiteral("多模态");
    if (m_llm && m_llm->pinOcrEngine() == QLatin1String("ppocr"))
        return QStringLiteral("PP-OCR");
    return QStringLiteral("OCR");
}

/*
 * 图片这一档的报数，一句话。草稿那行出处和界面状态**共用**这一句 ——
 * 两边各写一份的话，哪天对不上数就没人能查出到底认了几张。
 */
QString Summarizer::shotNote() const {
    const int total = m_shots.size() + m_shotSkipped;
    if (total == 0)
        return QString();
    if (m_imageMode == QLatin1String("none"))
        return QStringLiteral("另有 %1 张截图没认（设置 → 汇总 可以切成 OCR 或多模态）")
            .arg(total);
    QString note = QStringLiteral("截图 %1 张：%2 认出 %3 张")
                       .arg(total)
                       .arg(shotKindLabel())
                       .arg(m_shotOk);
    if (m_shotFailed > 0)
        note += QStringLiteral("，%1 张没认出字").arg(m_shotFailed);
    if (m_shotSkipped > 0)
        note += QStringLiteral("，另有 %1 张超出了每轮 %2 张的上限")
                    .arg(m_shotSkipped)
                    .arg(kMaxShotsPerRun);
    return note;
}

void Summarizer::cancel() {
    if (!m_busy)
        return;
    m_stopping = true;
    m_queue.clear();
    /*
     * 认到一半也在这儿收手：标志一放下，线程池里那张回来就没人认了
     * （onImageRecognized 开头那道守卫），不会再往这一轮里添正文。
     */
    m_inShots = false;
    m_shots.clear();
    setStatus(QStringLiteral("正在收尾…"));
    advance();
}

void Summarizer::pump() {
    if (m_stopping || !m_token.isEmpty() || m_queue.isEmpty())
        return;

    m_current = m_queue.takeFirst();
    const QString busyStatus = m_current.merge
                                   ? QStringLiteral("正在合并「%1」…").arg(m_current.category)
                                   : QStringLiteral("正在整理第 %1/%2 批…")
                                         .arg(m_doneSteps + 1)
                                         .arg(m_totalSteps);
    const QString prompt = m_current.merge ? mergePrompt(m_current.category)
                                           : classifyPrompt(m_categories);
    QString userText = m_current.text;
    if (m_current.merge) {
        /* 同一分类的几批之间给个分隔，免得模型以为它们本来就是一份 */
        userText = m_current.parts.join(QStringLiteral("\n\n----\n\n"));
    }
    m_token = m_llm->ask(prompt, userText, busyStatus);
    setStatus(busyStatus);
}

void Summarizer::advance() {
    if (!m_token.isEmpty())
        return;

    if (m_stopping) {
        setBusy(false);
        setStatus(QStringLiteral("已停在第 %1/%2 步（已写出的 %3 份草稿还在）")
                      .arg(m_doneSteps)
                      .arg(m_totalSteps)
                      .arg(m_drafts));
        emit runFinished(m_drafts);
        return;
    }

    if (m_queue.isEmpty() && !m_merging)
        startMergePhase();

    if (m_queue.isEmpty()) {
        complete();
        return;
    }
    pump();
}

/*
 * 分类收拢：已有分类照旧，新分类最多留 kMaxNewCategories 个（按正文大小挑），
 * 其余的、以及正文短到撑不起一份文档的，并进「未分类」。
 *
 * 并进去的时候在原正文前面留一行「【它自己起的分类名】」—— 那名字是模型给的唯一
 * 线索，丢了他就只能对着一坨正文猜"这块原来叫细分样式"。
 */
void Summarizer::collapseCategories() {
    const QStringList known = m_store ? m_store->categories() : QStringList();

    QList<QPair<QString, int>> fresh;   /* 新分类名 -> 正文字数 */
    for (const QString &name : std::as_const(m_order)) {
        if (name == kFallbackCategory || known.contains(name))
            continue;
        int bytes = 0;
        const QStringList parts = m_bucket.value(name);
        for (const QString &part : parts)
            bytes += part.size();
        fresh.append(qMakePair(name, bytes));
    }
    std::sort(fresh.begin(), fresh.end(), [](const QPair<QString, int> &a,
                                             const QPair<QString, int> &b) {
        return a.second > b.second;
    });

    QSet<QString> keep;
    for (int i = 0; i < fresh.size() && i < kMaxNewCategories; ++i)
        keep.insert(fresh.at(i).first);

    for (const QPair<QString, int> &item : std::as_const(fresh)) {
        if (keep.contains(item.first) && item.second >= kMinCategoryChars)
            continue;
        const QStringList parts = m_bucket.value(item.first);
        for (const QString &part : parts) {
            m_bucket[kFallbackCategory].append(
                QStringLiteral("【%1】\n\n%2").arg(item.first, part));
        }
        m_bucket.remove(item.first);
        m_order.removeAll(item.first);
        if (!m_order.contains(kFallbackCategory))
            m_order.append(kFallbackCategory);
        ++m_collapsed;
    }
}

/*
 * 分类阶段跑完：只有一个批的分类当场落草稿，被切成好几批的排队等一次合并。
 *
 * 合并只在"同一分类确实来了两份以上"时才发请求 —— 能省一次就省一次，
 * 而且模型重写一遍已经整理好的正文，本身就有改写走样的风险。
 */
void Summarizer::startMergePhase() {
    m_merging = true;
    collapseCategories();
    for (const QString &category : std::as_const(m_order)) {
        const QStringList parts = m_bucket.value(category);
        /*
         * 「未分类」不合并：那是一堆收拢剩下的东西，每段前面还带着它自己原来
         * 叫什么（【细分样式】那一行）。让模型重写一遍，最可能的结果就是把那些
         * 名字洗没了 —— 而他回来要做的第一件事正是照着名字重新分类。
         * 顺带省一次请求。
         */
        if (parts.size() <= 1 || category == kFallbackCategory) {
            finishCategory(category, parts);
            continue;
        }
        Task task;
        task.merge = true;
        task.category = category;
        task.parts = parts;
        m_queue.append(task);
    }
    if (!m_queue.isEmpty()) {
        /* 合并是分类之后多出来的步骤，分母得跟着涨，不然进度条会跑到 120% */
        m_totalSteps += m_queue.size();
    }
}

void Summarizer::finishCategory(const QString &category, const QStringList &parts) {
    QString body = parts.join(QStringLiteral("\n\n"));
    if (body.trimmed().isEmpty())
        return;
    body = QStringLiteral("## 汇总 %1 ~ %2\n\n").arg(m_from, m_to)
         + provenance(m_runId, m_sections, shotNote()) + body.trimmed();
    const QString path = m_store->writeDraft(m_runId, category, body);
    if (!path.isEmpty())
        ++m_drafts;
}

void Summarizer::complete() {
    setBusy(false);
    m_merging = false;
    /*
     * 有批次失败时把数字说在结论前面 —— 只报"N 份草稿"会让人以为这段时间的
     * 内容都进去了，而那一批的原文根本没被模型看过。
     */
    const QString failed = m_failedSteps > 0
                               ? QStringLiteral("%1 批没整理出来（%2）· ").arg(m_failedSteps)
                                     .arg(m_lastError)
                               : QString();
    /* 有几张图没认出来也一样要说在前面，光报草稿数会让他以为这轮把图都吃了 */
    const QString shots = (m_shotFailed > 0 || m_shotSkipped > 0)
                              ? shotNote() + QStringLiteral(" · ")
                              : QString();
    const QString folded = m_collapsed > 0
                               ? QStringLiteral("%1 个分类太碎、并进了「未分类」· ")
                                     .arg(m_collapsed)
                               : QString();
    if (m_drafts > 0)
        setStatus(QStringLiteral("%1%2汇总完成：%3 份草稿在「待审」里，审完可以收进归档")
                      .arg(failed, shots + folded)
                      .arg(m_drafts));
    else if (m_failedSteps > 0)
        setStatus(QStringLiteral("%1一份草稿都没有：把区间缩短，或者换「模型」那一栏里"
                                "快一点的模型再试").arg(failed));
    else
        setStatus(QStringLiteral("没能整理出内容（模型把这一批都判成没价值了）"));
    emit runFinished(m_drafts);
}

void Summarizer::onFinished(const QString &token, const QString &text) {
    if (token != m_token || m_token.isEmpty())
        return;
    m_token.clear();
    if (m_inShots) {
        /* 这条回来的是**一张截图认出来的正文**，不是某一批的分类结果 */
        onImageRecognized(text, QString());
        return;
    }
    ++m_doneSteps;
    emit progressChanged();

    const Task done = m_current;
    m_current = Task();

    if (done.merge) {
        /*
         * 合并回来的是正文本身；万一模型还是写了 === 分类: === 标记，
         * 解析出来的正文照用、名字仍按合并前那个分类走 —— 分类在合并这一步
         * 被改名，草稿就并进另一份文档去了，那是最不容易被发现的一种错。
         */
        const QVariantList parts = parseReply(text);
        QStringList bodies;
        for (const QVariant &part : parts) {
            const QString body = part.toMap().value(QStringLiteral("body")).toString();
            if (!body.trimmed().isEmpty())
                bodies.append(body.trimmed());
        }
        finishCategory(done.category, bodies.isEmpty() ? QStringList{text.trimmed()} : bodies);
    } else {
        const QVariantList parts = parseReply(text);
        for (const QVariant &part : parts) {
            const QVariantMap map = part.toMap();
            const QString category = map.value(QStringLiteral("category")).toString();
            const QString body = map.value(QStringLiteral("body")).toString();
            if (body.trimmed().isEmpty())
                continue;
            if (!m_bucket.contains(category))
                m_order.append(category);
            m_bucket[category].append(body.trimmed());
        }
    }

    setStatus(QStringLiteral("正在汇总 %1 ~ %2（%3/%4 步）")
                  .arg(m_from, m_to)
                  .arg(m_doneSteps)
                  .arg(m_totalSteps));
    advance();
}

void Summarizer::onFailed(const QString &token, const QString &error) {
    if (token != m_token || m_token.isEmpty())
        return;
    m_token.clear();
    if (m_inShots) {
        onImageRecognized(QString(), error);
        return;
    }
    m_current = Task();

    /*
     * 一批没回来**不作废整轮**。
     *
     * 2026-09-21 真跑本地 gemma-3-4b：5 批里第 1 批正常回来、第 2 批 120 秒超时，
     * 而当时的写法是直接中断整轮 —— 已经整理好的那一批跟着一起扔，跑完 0 份草稿。
     * 现在记下这一批失败、接着跑剩下的，最后一起报"哪几批没成"。
     * 原文不会因此丢：它还在自己的日期目录里，收进归档是单独一步（而且没草稿
     * 的时候界面那一步也不会替他藏东西）。
     */
    ++m_failedSteps;
    m_lastError = error.trimmed().isEmpty() ? QStringLiteral("请求没成功") : error.trimmed();
    setStatus(QStringLiteral("第 %1/%2 批没整理出来（%3）—— 接着跑剩下的")
                  .arg(m_doneSteps + 1)
                  .arg(m_totalSteps)
                  .arg(m_lastError));
    ++m_doneSteps;
    emit progressChanged();
    advance();
}
