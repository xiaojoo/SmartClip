#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class ClipboardStore;
class LlmClient;

/*
 * 把一段时间的剪贴板原文汇总成分类文档（QML 单例 `Sum`，见 src/main.cpp）。
 *
 * 一条链路：
 *
 *   选区间 -> ClipboardStore::sectionsInRange 取原文
 *          -> 按字数切批，一批一条 LlmClient::ask（分类 + 归并）
 *          -> 同一个分类被切到好几批时，再发一条"合并这几批"的请求
 *          -> 每个分类写成一个**待审草稿**（<root>/文档/待审/）
 *   人工在编辑器里看草稿 -> 采纳（并进 <root>/文档/<分类>.md）/ 丢弃
 *   都审完了 -> Store.archiveRange 把这一段的原文收进归档（树上不再显示）
 *
 * ===========================================================================
 * 为什么要切批：一次请求喂不下也等不起
 * ===========================================================================
 * 一天几十条复制内容拼起来轻松过几万字，而 LlmClient 那条请求是
 * **非流式 + 120 秒超时**（见 Translate.cpp 的 send）：整段塞进去，要么模型
 * 自己截断，要么超时，而且吐一篇长文比吐一段更容易跑题。所以这里固定切批，
 * 一批一批问，同一分类的多批结果最后再合一次。
 *
 * 批数还有上限（kMaxBatches）：点一下按钮就悄悄烧掉两百次请求是不该发生的，
 * 超了直接让他把区间缩短，而不是他自己去数。
 *
 * ===========================================================================
 * 为什么输出格式是 "=== 分类: X ===" 而不是 JSON
 * ===========================================================================
 * 要模型把成篇 Markdown 塞进 JSON 字符串，它就得给正文里每个换行、引号、
 * 反斜杠做转义 —— 那是长文本下最常见的翻车方式（少转一个引号整份解析不了，
 * 而这一轮的努力全废）。分隔标记只需要"行首几个字符"就能认，认不出来时
 * 还有明确的退路：整份回复当"未分类"收着，内容不会凭空丢。
 */
class Summarizer final : public QObject {
    Q_OBJECT

    /* 有一轮汇总在跑（界面上"开始汇总"该置灰，进度条该转） */
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    /* 一句话状态："正在汇总 3/7 批…" / "汇总中断：…" */
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    /* 进度：分母是"这一轮总共要发几条请求"（分类批数 + 后面算出来的合并条数） */
    Q_PROPERTY(int totalSteps READ totalSteps NOTIFY progressChanged)
    Q_PROPERTY(int doneSteps READ doneSteps NOTIFY progressChanged)

public:
    Summarizer(ClipboardStore *store, LlmClient *llm, QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    int totalSteps() const { return m_totalSteps; }
    int doneSteps() const { return m_doneSteps; }

    /*
     * 开跑。fromDate / toDate 是 `yyyy-MM-dd`（含两端）。
     *
     * 同步的部分只有"取原文 + 切批"，之后每一条请求都是异步的；结果不从这里
     * 返回，而是落成草稿文件（界面上刷新的是 Store.drafts() 那一栏），
     * 跑完发 runFinished。
     */
    Q_INVOKABLE void start(const QString &fromDate, const QString &toDate);

    /* 中途停掉：已经写出去的草稿留着，剩下的批次不发 */
    Q_INVOKABLE void cancel();

    /* 提示词（自检钉的是"输出格式那条有没有写进去"，界面不显示它们） */
    Q_INVOKABLE QString classifyPrompt(const QString &existingCategories = QString()) const;
    Q_INVOKABLE QString mergePrompt(const QString &category) const;

    /*
     * 解析模型的回复（自检直接喂假回复，不用发真请求就能验这条链路）。
     * 返回 [{category, body}]；一个标记都没认出来时给一条"未分类"兜住。
     */
    Q_INVOKABLE QVariantList parseReply(const QString &reply) const;

signals:
    void busyChanged();
    void statusChanged();
    void progressChanged();
    /* 一轮跑完（draftCount = 写出来的草稿数；为 0 时界面该说"什么都没汇总出来"） */
    void runFinished(int draftCount);

private:
    /* 一个待发的请求：要么"把这批原文分类整理"，要么"把同一分类的几批合成一份" */
    struct Task {
        bool merge = false;
        QString category;      /* merge 时是分类名 */
        QStringList parts;     /* merge 时是那几批的正文 */
        QString text;          /* 分类时是这批原文 */
    };

    void setBusy(bool on);
    void setStatus(const QString &text);
    void pump();
    /* 每一步回来之后决定下一步：接着发、切到合并阶段、还是收工 */
    void advance();
    /* 一条请求回来了 / 失败了 */
    void onFinished(const QString &token, const QString &text);
    void onFailed(const QString &token, const QString &error);
    /* 分类阶段跑完：单批的分类直接落草稿，多批的排合并 */
    void startMergePhase();
    /* 模型报出来的分类收拢到能审的份数（超出上限 / 正文太短的并进「未分类」） */
    void collapseCategories();
    void finishCategory(const QString &category, const QStringList &parts);
    void complete();

    ClipboardStore *m_store = nullptr;
    LlmClient *m_llm = nullptr;

    bool m_busy = false;
    QString m_status;
    int m_totalSteps = 0;
    int m_doneSteps = 0;
    int m_drafts = 0;

    /* 这一轮的批次号（草稿文件名的前缀）和区间（状态里要说清楚汇总的是哪几天） */
    QString m_runId;
    QString m_from;
    QString m_to;
    /* 开跑时抓一次现有分类，喂给模型当候选（一轮里面不变，免得它自己追着改） */
    QString m_categories;
    /* 这一轮吃进去的段数和跳过的图片段数，写进草稿开头那行出处 */
    int m_sections = 0;
    int m_images = 0;
    /* 没回来的批数（超时 / 网络错）和最后那一句错 —— 结论里要说，不能只报草稿数 */
    int m_failedSteps = 0;
    QString m_lastError;
    /* 被收拢进「未分类」的新分类数（模型报 28 个分类那种情况要说出来） */
    int m_collapsed = 0;

    QList<Task> m_queue;
    /* 在飞的那一条（token 对不上回来的结果一律丢掉，见 Checker 里同一套做法） */
    QString m_token;
    Task m_current;
    bool m_merging = false;
    bool m_stopping = false;

    /* 分类名 -> 已经归到它名下的各批正文 */
    QHash<QString, QStringList> m_bucket;
    QStringList m_order;   /* 分类第一次出现的顺序，草稿按它排 */
};
