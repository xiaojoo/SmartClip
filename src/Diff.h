#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

/*
 * 两份正文的差异（"文件对比"功能）。
 *
 * 做成 QML 单例 `Differ`（见 src/main.cpp）：对比卡片要问它"这两份东西差在哪"，
 * 结果是一个**已经对齐好的行表**（见 compare）。
 *
 * 算法：行级 LCS（最长公共子序列）+ 差异块归组。
 *
 *   为什么用 LCS 而不是逐行比：两边插了一行之后，后面每一行都错位了，
 *   逐行比会报出"从插入点往后的每一行都不同"。LCS 找得到那个"对齐点"，
 *   报出来的就只有真正变掉的那一段。
 *
 *   为什么加"行内相似度"合并：LCS 只会说"A 区删了 2 行、B 区加了 2 行"，
 *   看着像两处独立改动，其实用户改的是同一段（改了两个字）。把**相邻的**
 *   删除区和新增区按行内相似度配成对（见 kSimilarEnough），报出来就是
 *   "这 2 行改了"，这正是用户在找的东西。
 *
 * 输出（compare 的返回）：按行给，**两侧等长**（短的那边补空行），每项
 *   {
 *     no,           行号（从 1 开始，给界面显示用）
 *     left, right,  两边这一行的文字（"" = 这一侧没有对应行，是补的空行）
 *     leftNo,       左侧原始行号（0 = 这一侧是补的）
 *     rightNo,      右侧原始行号（0 = 这一侧是补的）
 *     kind,         "same" / "del" / "add" / "mod"
 *     desc          （只有 mod 有）"第 N 行" 之类的简短说明
 *   }
 *
 * kind 用两个字符的记号给界面画色块：del = 左边多出来的（右边是空行）、
 * add = 右边多出来的、mod = 两边都有但内容不同。
 *
 * 相似度阈值（kSimilarEnough = 0.5）是试出来的：低于它之后，"整个函数被
 * 换掉"和"几行微调"会被混为一谈；高过它，改一个字的两行就配不上对了。
 */
class DiffEngine final : public QObject {
    Q_OBJECT

    /*
     * 两份正文的差异（见 compare 的说明）。
     *
     * 写成属性 + lastError：QML 那边是"先 loadFile / compare，再看这两样"，
     * 和 Store 那些单例一个用法（不做成"返回值 + 出参"是因为 QML 拿不到出参）。
     */
    Q_PROPERTY(QVariantList rows READ rows NOTIFY compared)
    Q_PROPERTY(QString lastError READ lastError NOTIFY compared)
    /* 对比结果的一句话概况："左侧 120 行 / 右侧 118 行，差异 7 处" */
    Q_PROPERTY(QString summary READ summary NOTIFY compared)
    /* 有结果可看没（卡片据此决定显示并排栏还是空状态） */
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY compared)

    /* 两份文件的名字（卡片标题栏显示） */
    Q_PROPERTY(QString leftTitle READ leftTitle NOTIFY compared)
    Q_PROPERTY(QString rightTitle READ rightTitle NOTIFY compared)

public:
    explicit DiffEngine(QObject *parent = nullptr);

    QVariantList rows() const { return m_rows; }
    QString lastError() const { return m_lastError; }
    QString summary() const { return m_summary; }
    bool hasResult() const { return m_hasResult; }
    QString leftTitle() const { return m_leftTitle; }
    QString rightTitle() const { return m_rightTitle; }

    /*
     * 对比两份正文。
     *
     * leftPath / rightPath 只用来取标题和判断编码，**内容由调用方给**
     * （编辑器里那份可能还没存盘，所以不能从磁盘读）。
     */
    Q_INVOKABLE void compare(const QString &leftText, const QString &rightText,
                             const QString &leftPath = QString(),
                             const QString &rightPath = QString());

    /*
     * 读一个文件来对比（带编码识别，和编辑器打开文件用同一套判断）。
     * 返回空串 = 失败，原因在 lastError。
     */
    Q_INVOKABLE QString readFile(const QString &path);

    /* 清空（关掉卡片时用） */
    Q_INVOKABLE void clear();

    /*
     * 把差异导出成**统一格式**的补丁文字（--- / +++ / @@ …）。
     * 复制到剪贴板 / 存成 .patch 用 —— 这是能把结果带走的那一种形式。
     */
    Q_INVOKABLE QString unifiedDiff() const;

signals:
    void compared();

private:
    /* 两侧不同的行数统计（summary 用） */
    int m_delCount = 0;
    int m_addCount = 0;
    int m_modCount = 0;
    int m_leftLines = 0;
    int m_rightLines = 0;

    QVariantList m_rows;
    QString m_lastError;
    QString m_summary;
    QString m_leftTitle;
    QString m_rightTitle;
    bool m_hasResult = false;
};
