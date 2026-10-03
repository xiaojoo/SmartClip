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
 *          -> （图片段按 imageMode 先认成文字：本机 OCR 或 多模态，见下面那段）
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

    /*
     * 剪贴板里的**图片段**（截图）要不要认、用哪条路认（设置 → 汇总 那一栏）：
     *
     *   "none"    不认，照旧只汇总文字（这功能之前的行为）
     *   "ocr"     本机 OCR：Windows 自带 / PP-OCRv6 用哪个引擎，跟着
     *             「设置 → 模型 → 图上选字」那一项走（Llm.pinOcrEngine），
     *             不联网、不烧 token
     *   "vision"  多模态：每张图一次请求，发给「识别模型」那一栏（留空则用 model）
     *
     * 两条路都只干"图 -> 一段文字"这一件事，认完的文字和复制来的文字一起切批
     * 分类。为什么不给图片单开一条汇总流水线：分批 / 合并 / 分类收拢那几道护栏
     * 是照着"一次喂太多会超时"设计的，图片进来的数量只会更多，不该例外。
     */
    Q_PROPERTY(QString imageMode READ imageMode WRITE setImageMode NOTIFY settingsChanged)

public:
    Summarizer(ClipboardStore *store, LlmClient *llm, QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    int totalSteps() const { return m_totalSteps; }
    int doneSteps() const { return m_doneSteps; }

    /* 图片段怎么处理（见上面 imageMode 那条属性）；只认 "none"/"ocr"/"vision" */
    QString imageMode() const { return m_imageMode; }
    void setImageMode(const QString &value);

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

    /*
     * 线程池里那张图认完了（只有 "ocr" 那一档会走到这儿）。
     *
     * public 是因为发它的 ShotOcrTask 在同一个 .cpp 的匿名 namespace 里，
     * 而它要跨线程 invokeMethod 回来 —— 和 PinWindow::applyOcrResult 同一个做法。
     */
    void applyShotText(const QString &text, const QString &error);

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
    void settingsChanged();
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
    /* ---- 图片段：先一张一张认成文字，认完的才去切批（见 imageMode） ---- */
    void recognizeNextImage();
    /* 一张有结果了：text 是认出来的字，error 非空 = 这张没认出来（正文里不留它） */
    void onImageRecognized(const QString &text, const QString &error);
    /* 图片全认完（或这轮压根没有图要认）：把认出来的块并进原文，切批发请求 */
    void startBatchPhase();
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

    /* 图片段怎么处理："none" / "ocr" / "vision"（QSettings 的 summarize/imageMode） */
    QString m_imageMode;

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
    /* 这一轮吃进去的段数（写进草稿开头那行出处） */
    int m_sections = 0;
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

    /* ---- 图片段（截图）：认成文字之前，这一轮先卡在这一阶段 ---- */

    /* 一张待认的图：它自己那段时间（认出来的正文要带出处）和文件绝对路径 */
    struct Shot {
        QString day;
        QString time;
        QString path;
    };
    /* 这一轮要认的图（已经按 kMaxShotsPerRun 截过），串行一张一张来 */
    QList<Shot> m_shots;
    int m_nextShot = 0;
    /* 认出来的正文块，和复制来的段一起按时间排序后才切批 */
    QStringList m_shotBlocks;
    /* 这一轮的原文块：先攒着，等图片认完一起排（不然截图会全落在时间线尾巴上） */
    QStringList m_blocks;
    bool m_inShots = false;
    int m_shotOk = 0;
    int m_shotFailed = 0;
    /* 超出每轮上限、这一轮根本没试着认的张数（报数用，不当失败） */
    int m_shotSkipped = 0;

    /* 图片和文字合起来切批之后那一刻起，进度归 pump/advance 管 */
    QString shotNote() const;
    /* 出现在正文和状态里的那个引擎名："OCR" / "多模态" */
    QString shotKindLabel() const;
    QStringList m_order;   /* 分类第一次出现的顺序，草稿按它排 */
};
