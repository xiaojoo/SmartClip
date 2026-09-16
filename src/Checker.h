#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class LlmClient;

/*
 * 编辑区校验（中文用词 / 代码语法）。
 *
 * 做成 QML 单例 `Check`（见 src/main.cpp），因为设置面板要开关它、编辑区要读
 * 它的问题清单。
 *
 * 两层做法（为什么不是"全交给大模型"）：
 *
 *   1. **本地规则**：括号配对、中英文标点混用、重复字（"的的"）、
 *      "的地得"、问号叹号叠用、行尾空白、注释没闭合…这些是确定性判断，
 *      毫秒出结果、不花钱、离线也能用。而且它**知道准确位置** —— 大模型
 *      给的"第几行"经常是错的（它数的和我们数的不是一回事）。
 *
 *   2. **大模型**：把符号（括号 / 引号 / 注释）在本地全部配平之后，剩下的就是
 *      "这句中文用词对不对""这段代码语法对不对"，那正是模型擅长的。
 *      请求走 LlmClient::ask（自定义 system 提示词那条路），**只发正文、不发文件名**，
 *      并且要求它按固定行号格式回（见 kFormat）。
 *
 * 合并的时候**以本地为准**：模型报的每条都要在原文里定位到（位置 + 片段都对得上）
 * 才收，定位不到的直接丢掉 —— 那是它在编。宁可少报几条，也不能给用户一堆
 * 点不到的假问题。
 *
 * 开关在设置面板（QSettings 的 check/enabled，默认关）：这是要联网、
 * 要花 token 的功能，不打招呼就替用户发请求不合适。
 */
class Checker final : public QObject {
    Q_OBJECT

    /* 总开关（设置面板那个勾） */
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)

    /*
     * 上一次校验的结果：每项
     *   { row, col, endRow, endCol, severity, kind, message, snippet, source, suggestion }
     *     row/col      从 1 开始的行号、从 0 开始的列号（编辑区那边就是这个口径）
     *     endRow/endCol 片段结束位置（local 那条用来选中出错的那一段）
     *     severity     "error" / "warn" / "info"
     *     kind         "bracket" / "punct" / "repeat" / "de" / "llm-zh" / "llm-code" …
     *     message      人话说明
     *     snippet      出错的那一小段原文
     *     source       "local" / "llm"
     *     suggestion   改法（空串 = 没法自动改）
     */
    Q_PROPERTY(QVariantList issues READ issues NOTIFY issuesChanged)
    /* 有个校验在跑（本地那部分同步做完，等模型那部分时它也一直是真的） */
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    /* 一句话状态："本地发现 3 个问题，正在问模型…" / "校验完成：共 5 个问题" */
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    /* 大模型那条支路能不能用（没配好接口就只跑本地规则，界面要说清楚） */
    Q_PROPERTY(bool llmReady READ llmReady NOTIFY llmStateChanged)
    /*
     * 「校验」那一栏顶上那行"现在用的是哪个模型"。
     *
     * 和 llmReady 是**同一套判据**（本地模式看程序 + 模型文件，接口模式看地址 +
     * 模型名），所以那句话不会和"到底走不走模型"打架。界面直接显示这行，
     * 不自己在 QML 里拼（见 Checker.cpp 里 modelSummary 的说明）。
     */
    Q_PROPERTY(QString modelSummary READ modelSummary NOTIFY llmStateChanged)

public:
    explicit Checker(LlmClient *llm = nullptr, QObject *parent = nullptr);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);

    QVariantList issues() const { return m_issues; }
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    bool llmReady() const;
    QString modelSummary() const;

    /*
     * 让它去校验这份正文（当前标签的）。
     *
     * text 是正文，filePath 只用来判语言（**不发给模型**）。
     * 返回本地那部分发现的问题数（可能还有模型那部分在后面异步补上）。
     */
    Q_INVOKABLE int check(const QString &text, const QString &language,
                          const QString &filePath = QString());

    /*
     * 只跑本地规则（设置面板里"只看本地"/自检用）。
     * 返回问题条数，结果同样在 issues 里。
     */
    Q_INVOKABLE int checkLocalOnly(const QString &text, const QString &language);

    /* 清空结果（关掉卡片 / 换文档时用） */
    Q_INVOKABLE void clear();

    /*
     * 综合那一句话（状态栏 / 卡片顶上显示）。
     * 全过就返回"没发现问题"，这也是唯一值得发绿色的情况。
     */
    Q_INVOKABLE QString resultSummary() const;

    /*
     * system 提示词（自检会核"里面确实写死了行号格式和'只报错误'两条"）。
     * 按语言给两份：中文用词一份、代码语法一份。
     */
    Q_INVOKABLE QString promptFor(const QString &language) const;
    /* 提示词里那个固定输出格式（自检和文档都用它，改一处就够） */
    Q_INVOKABLE QString replyFormat() const;

    /* 上一次模型那部分失败的原因（空串 = 没失败） */
    Q_INVOKABLE QString lastError() const { return m_lastError; }

    /* 校验该不该走上模型那条路（开关开着 + 接口配好了 + 正文不太长） */
    Q_INVOKABLE bool wantsModel(const QString &text) const;

signals:
    void settingsChanged();
    void issuesChanged();
    void busyChanged();
    void statusChanged();
    void llmStateChanged();
    /* 模型那部分回来了（界面据此把列表滚到第一条） */
    void checked(int modelIssueCount);

private:
    /* 本地规则那一遍：返回找到的问题（行号 / 列号都按原文算） */
    QList<QVariantMap> analyzeLocal(const QString &text, const QString &language) const;
    /* 把模型的回复解析成问题（定位不到的丢掉），badLines 收"没解析出来的行" */
    QList<QVariantMap> parseReply(const QString &reply, const QString &text) const;

    void setBusy(bool on);
    void setStatus(const QString &text);

    LlmClient *m_llm = nullptr;

    bool m_enabled = false;
    bool m_busy = false;
    QString m_status;
    QString m_lastError;
    QVariantList m_issues;

    /* 正在等模型的那条请求 token（换文档 / 重新校验时用来丢掉旧结果） */
    QString m_token;
    /* 这次校验对应的正文（模型回来时按它定位 + 核对） */
    QString m_text;
    QString m_language;
    /* 本地那部分的结果（模型回来后合并成 m_issues） */
    QList<QVariantMap> m_local;
};
