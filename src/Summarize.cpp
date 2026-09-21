#include "Summarize.h"

#include "ClipboardStore.h"
#include "Translate.h"

#include <QDateTime>
#include <QVariantList>
#include <QVariantMap>

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
QString provenance(const QString &runId, int sections, int images) {
    QString line = QStringLiteral("> 取自 %1 段原文 · 批次 %2").arg(sections).arg(runId);
    if (images > 0)
        line += QStringLiteral(" · 另有 %1 段是图片，本版不处理").arg(images);
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

}  // namespace

Summarizer::Summarizer(ClipboardStore *store, LlmClient *llm, QObject *parent)
    : QObject(parent), m_store(store), m_llm(llm) {
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
     * 切批按"段"切，不从一行中间断开：一段被劈成两半，模型两边各看到半条，
     * 整理出来就是两条残缺的笔记。一段本身比一批还长时让它独占一批 ——
     * 断在段中间不如不断。
     */
    QStringList blocks;
    for (const QVariant &row : rows) {
        const QVariantMap map = row.toMap();
        const QString text = map.value(QStringLiteral("text")).toString();
        QString current;
        const QStringList lines = text.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (line.startsWith(QStringLiteral("### ")) && !current.trimmed().isEmpty()) {
                blocks.append(current.trimmed());
                current.clear();
            }
            current += line + QLatin1Char('\n');
        }
        if (!current.trimmed().isEmpty())
            blocks.append(current.trimmed());
    }

    QList<Task> tasks;
    Task batch;
    QStringList pieces;
    for (const QString &block : std::as_const(blocks))
        splitOversizedBlock(block, kBatchChars, &pieces);
    for (const QString &block : std::as_const(pieces)) {
        if (!batch.text.isEmpty()
            && batch.text.size() + block.size() > kBatchChars) {
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
        setStatus(QStringLiteral("这段时间的内容太多（要分 %1 批，上限 %2）"
                                "—— 把区间缩短一点再汇总")
                      .arg(tasks.size())
                      .arg(kMaxBatches));
        return;
    }

    m_runId = stampForRun();
    m_from = from;
    m_to = to;
    m_categories = m_store->categories().join(QStringLiteral("、"));
    m_sections = 0;
    m_images = 0;
    m_failedSteps = 0;
    m_collapsed = 0;
    m_lastError.clear();
    for (const QVariant &row : rows) {
        const QVariantMap map = row.toMap();
        m_sections += map.value(QStringLiteral("count")).toInt();
        m_images += map.value(QStringLiteral("images")).toInt();
    }
    m_bucket.clear();
    m_order.clear();
    m_queue = tasks;
    m_drafts = 0;
    m_doneSteps = 0;
    m_totalSteps = tasks.size();
    m_merging = false;
    m_stopping = false;
    m_token.clear();

    setBusy(true);
    setStatus(QStringLiteral("正在汇总 %1 ~ %2（共 %3 段，分 %4 批问）")
                  .arg(from, to)
                  .arg(m_sections)
                  .arg(m_totalSteps));
    pump();
}

void Summarizer::cancel() {
    if (!m_busy)
        return;
    m_stopping = true;
    m_queue.clear();
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
         + provenance(m_runId, m_sections, m_images) + body.trimmed();
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
    const QString folded = m_collapsed > 0
                               ? QStringLiteral("%1 个分类太碎、并进了「未分类」· ")
                                     .arg(m_collapsed)
                               : QString();
    if (m_drafts > 0)
        setStatus(QStringLiteral("%1%2汇总完成：%3 份草稿在「待审」里，审完可以收进归档")
                      .arg(failed, folded)
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
