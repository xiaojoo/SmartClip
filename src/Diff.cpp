#include "Diff.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStringConverter>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <algorithm>

namespace {

/*
 * 行内相似度够不够高，把这两行算成"同一行改了内容"（mod）而不是"删一行加一行"。
 *
 * 算法用**二元组（bigram）重合率**：比编辑距离便宜（不需要 O(n·m) 的表），
 * 对"改了几个字"这类行内小改动的判断又足够准。
 *
 * 0.5 是试出来的：低于它，"整个函数被换掉"和"几行微调"会被混成一片；
 * 高过它，改一个字的两行就配不上对了（那反而更常出现）。
 */
constexpr double kSimilarEnough = 0.5;

QStringList splitLines(const QString &text) {
    QStringList lines = text.split(QLatin1Char('\n'));
    /* \r 去掉：CRLF 文件比 LF 文件时不该每一行都算"不同" */
    for (QString &l : lines) {
        if (l.endsWith(QLatin1Char('\r')))
            l.chop(1);
    }
    /*
     * 末尾那个换行会在 split 结果里留下一个空尾巴（"a\n" -> ["a", ""]）。
     * 它不算一行 —— 不摘掉的话，每份以换行结尾的文件都会多出一条"右侧多一行"。
     */
    if (!lines.isEmpty() && lines.last().isEmpty() && text.endsWith(QLatin1Char('\n')))
        lines.removeLast();
    return lines;
}

double similarity(const QString &a, const QString &b) {
    if (a == b)
        return 1.0;
    if (a.isEmpty() || b.isEmpty())
        return 0.0;
    /*
     * 太短的行（一个字符）只看是不是完全一样：给它们算 bigram 重合率会得到一个
     * "看起来像改了"的分数（"a" 和 "c" 这种也能凑出分来），界面上就分不清
     * "这一行改了"和"这一行是多出来的"。
     */
    if (a.size() < 2 || b.size() < 2)
        return 0.0;

    /* a 的 bigram 多重集合 */
    QHash<QString, int> bag;
    for (int i = 0; i + 1 < a.size(); ++i)
        bag[a.mid(i, 2)]++;

    int hit = 0;
    for (int i = 0; i + 1 < b.size(); ++i) {
        const QString g = b.mid(i, 2);
        auto it = bag.find(g);
        if (it != bag.end() && it.value() > 0) {
            --it.value();
            ++hit;
        }
    }
    const int total = qMax(1, qMin(a.size(), b.size()) - 1);
    return double(hit) / double(total);
}

/* 一行差异块：要么只在一边，要么两边都有一批 */
struct Block {
    int leftStart = 0;    /* 左起（下标，0 基） */
    int leftCount = 0;
    int rightStart = 0;
    int rightCount = 0;
};

/*
 * 行级差异块（把"连续不一样的那些行"攒成一块）。
 *
 * 算法是**逐行往前走 + 多行前瞻**，不是 LCS 表：
 *
 *   1. 两边当前行一样 -> 往前走，这一行是相同的；
 *   2. 不一样 -> 从 [1, kLookahead] 里找**头一个**能让两边重新对齐的位置：
 *        左走 dx 行、右走 dy 行之后 a[i+dx] == b[j+dy]；
 *      dx / dy 就是这一块里左边 / 右边各"多出来"的行数；
 *   3. 找不到 -> 两边各吃掉一行（按"改了一行"处理）。
 *
 * 为什么不用 LCS：真正需要 LCS 的是"两份文件被大规模重排过"的场景，
 * 而这里要解决的问题是**看一个文件改了什么**。LCS 表既费内存（几千行时
 * 几十 MB），回溯里"往哪边走"那一步又极容易写错 —— 实测写错一次就报成
 * "整段都改了"（公共前缀全被吃进差异块），而且错得很难看出来。
 * 前瞻法每一步都能对着两行文字讲清楚，出问题时也好查。
 *
 * 代价：一份文件里同一段被搬来搬去时，报出来的差异块会比 LCS 大一些。
 * 那种场景属于"用专门的 diff 工具"，不是这里的活。
 */
constexpr int kLookahead = 64;

QVector<Block> diffBlocks(const QStringList &a, const QStringList &b) {
    const int m = a.size();
    const int n = b.size();
    QVector<Block> blocks;

    int i = 0;
    int j = 0;
    while (i < m || j < n) {
        /* 两边都还有、而且这一行一样：相同行，往前走 */
        if (i < m && j < n && a.at(i) == b.at(j)) {
            ++i;
            ++j;
            continue;
        }

        const int si = i;
        const int sj = j;
        int dx = -1;   /* 左边多出来的行数 */
        int dy = -1;   /* 右边多出来的行数 */

        /*
         * 前瞻：找一个"跳过去之后两边又能对上"的位置。
         * d = 0 那一对是 (i+1, j+1)（也就是两边各吃一行 = "改了一行"），
         * 所以找到的 dx/dy 不会同时为 0 —— 一定至少有一边真的动了。
         */
        for (int d = 0; d <= kLookahead && dx < 0; ++d) {
            for (int x = 0; x <= d && dx < 0; ++x) {
                const int y = d - x;
                const int pi = i + x;
                const int pj = j + y;
                if (pi >= m && pj >= n) {
                    /* 两边都正好走完：这一块把剩下的全吃掉 */
                    dx = x;
                    dy = y;
                    break;
                }
                if (pi < m && pj < n && a.at(pi) == b.at(pj)) {
                    dx = x;
                    dy = y;
                    break;
                }
            }
        }
        if (dx < 0) {
            /* 前瞻窗口里都找不到对齐点：两边各吃一行（当成"改了一行"） */
            dx = (i < m) ? 1 : 0;
            dy = (j < n) ? 1 : 0;
        }

        Block blk;
        blk.leftStart = si;
        blk.leftCount = dx;
        blk.rightStart = sj;
        blk.rightCount = dy;
        blocks.append(blk);

        i = si + dx;
        j = sj + dy;
    }
    return blocks;
}

/*
 * 按两个忽略开关把一行折成"比较用的形状"。
 *
 * 只折比较用的那一份，**显示还是原文** —— 忽略空白不等于把用户文件里的缩进
 * 删掉，界面上该看见还得看见。
 */
QStringList normalizeLines(const QStringList &lines, bool ignoreCase, bool ignoreWs) {
    if (!ignoreCase && !ignoreWs)
        return lines;
    QStringList out;
    out.reserve(lines.size());
    for (const QString &l : lines) {
        QString s = ignoreWs ? l.trimmed() : l;
        if (ignoreCase)
            s = s.toLower();
        out.append(s);
    }
    return out;
}

/*
 * 一行里"到底哪几个字变了"（BC 那一层深色的字级高亮）。
 *
 * 直接把行级那套前瞻算法套在**单字符**的序列上 —— 一行本来就短，
 * 而且这样两边各自拿到自己那一段的区间，左右两栏都能标到正确的字上。
 *
 * 超过 kCharCap 的行不这么算：逐字符前瞻最坏是 O(行长 × 64)，一份压缩过的
 * JS（整行几千字）会把对比卡住。退成"掐公共前后缀，中间整段算一处"，
 * 那种行本来也看不出哪个字变了。
 */
constexpr int kCharCap = 2000;

QVariantList charRunsOf(const QString &x, const QString &y, bool takeLeft) {
    const QString &self = takeLeft ? x : y;
    QVariantList out;
    if (x == y || self.isEmpty())
        return out;

    /* 长行只掐公共前后缀（见上面那段） */
    if (qMax(x.size(), y.size()) > kCharCap) {
        int p = 0;
        while (p < x.size() && p < y.size() && x.at(p) == y.at(p))
            ++p;
        int sx = x.size() - 1;
        int sy = y.size() - 1;
        while (sx > p && sy > p && x.at(sx) == y.at(sy)) {
            --sx;
            --sy;
        }
        const int end = takeLeft ? sx : sy;
        if (end >= p) {
            out.append(p);
            out.append(end - p + 1);
        }
        return out;
    }

    QStringList cx;
    QStringList cy;
    cx.reserve(x.size());
    cy.reserve(y.size());
    for (const QChar &c : x)
        cx.append(QString(c));
    for (const QChar &c : y)
        cy.append(QString(c));
    for (const Block &b : diffBlocks(cx, cy)) {
        const int start = takeLeft ? b.leftStart : b.rightStart;
        const int count = takeLeft ? b.leftCount : b.rightCount;
        if (count > 0) {
            out.append(start);
            out.append(count);
        }
    }
    return out;
}


}  // namespace

DiffEngine::DiffEngine(QObject *parent) : QObject(parent) {}

void DiffEngine::clear() {
    m_rows.clear();
    m_changes.clear();
    m_hasResult = false;
    m_lastError.clear();
    m_summary.clear();
    m_delCount = m_addCount = m_modCount = 0;
    m_leftLines = m_rightLines = 0;
    m_leadGapLeft = m_leadGapRight = 0;
    m_leftText.clear();
    m_rightText.clear();
    m_leftPath.clear();
    m_rightPath.clear();
    emit compared();
}

QString DiffEngine::readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_lastError = tr("打不开 %1").arg(QFileInfo(path).fileName());
        return QString();
    }
    const QByteArray bytes = f.readAll();
    /*
     * 编码：UTF-8 解不出来就按本地编码（中文 Windows 上是 GBK）再试一次。
     * 和编辑器打开文件用的是同一套判断（EditorViewItem::detectEncoding），
     * 这里写简版 —— 对比功能只要能看出"哪几行不一样"就够，不需要和编辑器
     * 逐字节一致。
     */
    QString text = QString::fromUtf8(bytes);
    if (text.contains(QChar(0xFFFD))) {
        QStringDecoder dec("GB18030");
        text = dec(bytes);
    }
    /* UTF-8 BOM */
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    return text;
}

void DiffEngine::compare(const QString &leftText, const QString &rightText,
                         const QString &leftPath, const QString &rightPath) {
    m_rows.clear();
    m_changes.clear();
    m_lastError.clear();
    m_delCount = m_addCount = m_modCount = 0;
    m_leadGapLeft = m_leadGapRight = 0;

    /* 存下来：两个忽略开关改了要按新口径重算，见 setIgnoreCase */
    m_leftText = leftText;
    m_rightText = rightText;
    m_leftPath = leftPath;
    m_rightPath = rightPath;

    const QStringList a = splitLines(leftText);
    const QStringList b = splitLines(rightText);
    m_leftLines = a.size();
    m_rightLines = b.size();

    /*
     * 比较用**归一化那一份**（忽略大小写 / 首尾空白），显示用原文那一份。
     * 两个开关都关着时 normalizeLines 直接返回原表，不复制。
     */
    const QStringList na = normalizeLines(a, m_ignoreCase, m_ignoreWhitespace);
    const QStringList nb = normalizeLines(b, m_ignoreCase, m_ignoreWhitespace);

    m_leftTitle = leftPath.isEmpty() ? tr("左边（未命名）") : QFileInfo(leftPath).fileName();
    m_rightTitle = rightPath.isEmpty() ? tr("右边（未命名）") : QFileInfo(rightPath).fileName();

    if (na == nb) {
        /* 一模一样（按当前口径）：直接说清楚，别让用户对着一堆绿行自己找结论 */
        m_hasResult = true;
        /* 还是把行列出来（用户可能想核对），但全部标成 same */
        for (int i = 0; i < a.size(); ++i) {
            QVariantMap row;
            row.insert(QStringLiteral("no"), i + 1);
            row.insert(QStringLiteral("left"), a.at(i));
            row.insert(QStringLiteral("right"), b.value(i));
            row.insert(QStringLiteral("leftNo"), i + 1);
            row.insert(QStringLiteral("rightNo"), i + 1);
            row.insert(QStringLiteral("kind"), QStringLiteral("same"));
            m_rows.append(row);
        }
        finishRows();
        emit compared();
        return;
    }

    const QVector<Block> blocks = diffBlocks(na, nb);

    int ai = 0;
    int bi = 0;
    int no = 0;

    auto appendSame = [&](int count) {
        for (int k = 0; k < count; ++k) {
            QVariantMap row;
            row.insert(QStringLiteral("no"), ++no);
            row.insert(QStringLiteral("left"), a.at(ai + k));
            row.insert(QStringLiteral("right"), b.at(bi + k));
            row.insert(QStringLiteral("leftNo"), ai + k + 1);
            row.insert(QStringLiteral("rightNo"), bi + k + 1);
            row.insert(QStringLiteral("kind"), QStringLiteral("same"));
            m_rows.append(row);
        }
        ai += count;
        bi += count;
    };

    for (const Block &blk : blocks) {
        /*
         * 块前面那些相同的行。
         *
         * 用**左边**的差值（不是两边的 min）：块在左边是从 blk.leftStart 开始的，
         * 在右边是从 blk.rightStart 开始的 —— 前面已经出现过增删块时，这两个
         * 起点会相差好几行。以前这里取 min(两个差值) 当"相同行数"，差开之后
         * 算出来的行数偏大，循环里 a.at(ai + k) 直接越界，报出来的是一堆
         * 错位的 same（实测：`a\nb\nc\nd` 对 `a\nb\nX\nc\nd` 报成
         * same|same|same|same|add —— 插进去的 X 被当成相同行，真正插的那一行
         * 反倒跑到末尾去了）。
         */
        appendSame(blk.leftStart - ai);

        /*
         * 块里：左边右边**配对**走。
         *
         * 拿左边第 li 行和右边第 rj 行比相似度：
         *   够像（kSimilarEnough）-> 配成一对，报 mod（"这一行改了"）；
         *   不像 -> 谁剩得多谁先出，左边出的是 del（右边补空行）、
         *          右边出的是 add（左边补空行）。
         *
         * 不能"按下标一一配对"（i 对 i）：左右行数不等时那个配法是错的。
         * 实测：左边 2 行、右边 1 行（右边这几行整个是插进去的时候），
         * 按下标配会把"左边第 2 行"和"右边第 1 行"配起来报成 mod，
         * 而真正插进去的那一行反而被吞掉 —— 界面上看着像"改了一行"，
         * 其实文件里是**多了一行**。
         */
        int li = blk.leftStart;
        int rj = blk.rightStart;
        const int lEnd = blk.leftStart + blk.leftCount;
        const int rEnd = blk.rightStart + blk.rightCount;
        while (li < lEnd || rj < rEnd) {
            const bool hasLeft = li < lEnd;
            const bool hasRight = rj < rEnd;

            if (hasLeft && hasRight
                && similarity(na.at(li), nb.at(rj)) >= kSimilarEnough) {
                QVariantMap row;
                row.insert(QStringLiteral("no"), ++no);
                row.insert(QStringLiteral("left"), a.at(li));
                row.insert(QStringLiteral("right"), b.at(rj));
                row.insert(QStringLiteral("leftNo"), li + 1);
                row.insert(QStringLiteral("rightNo"), rj + 1);
                row.insert(QStringLiteral("kind"), QStringLiteral("mod"));
                /*
                 * 字级差异按**原文**算，不按归一化那一份：忽略大小写时"只有大小写
                 * 不同"的两行会被判成相同行，压根走不到这里；真走到这里的行，用户
                 * 要看见的就是原文里哪几个字不一样。
                 */
                row.insert(QStringLiteral("leftWords"), charRunsOf(a.at(li), b.at(rj), true));
                row.insert(QStringLiteral("rightWords"), charRunsOf(a.at(li), b.at(rj), false));
                ++m_modCount;
                m_rows.append(row);
                ++li;
                ++rj;
                continue;
            }

            /* 谁剩得多谁先出（剩下的行数一样时先出左边） */
            const int leftRemain = lEnd - li;
            const int rightRemain = rEnd - rj;
            const bool takeLeft = !hasRight || (hasLeft && leftRemain >= rightRemain);

            QVariantMap row;
            row.insert(QStringLiteral("no"), ++no);
            if (takeLeft) {
                row.insert(QStringLiteral("left"), a.at(li));
                row.insert(QStringLiteral("right"), QString());
                row.insert(QStringLiteral("leftNo"), li + 1);
                row.insert(QStringLiteral("rightNo"), 0);
                row.insert(QStringLiteral("kind"), QStringLiteral("del"));
                ++m_delCount;
                ++li;
            } else {
                row.insert(QStringLiteral("left"), QString());
                row.insert(QStringLiteral("right"), b.at(rj));
                row.insert(QStringLiteral("leftNo"), 0);
                row.insert(QStringLiteral("rightNo"), rj + 1);
                row.insert(QStringLiteral("kind"), QStringLiteral("add"));
                ++m_addCount;
                ++rj;
            }
            m_rows.append(row);
        }

        ai = blk.leftStart + blk.leftCount;
        bi = blk.rightStart + blk.rightCount;
    }

    /* 尾巴上剩下的相同行（走到这里两边剩下的行数必然一样，取左边那个） */
    if (ai < a.size() && bi < b.size())
        appendSame(a.size() - ai);

    while (ai < a.size()) {
        QVariantMap row;
        row.insert(QStringLiteral("no"), ++no);
        row.insert(QStringLiteral("left"), a.at(ai));
        row.insert(QStringLiteral("right"), QString());
        row.insert(QStringLiteral("leftNo"), ++ai);
        row.insert(QStringLiteral("rightNo"), 0);
        row.insert(QStringLiteral("kind"), QStringLiteral("del"));
        ++m_delCount;
        m_rows.append(row);
    }
    while (bi < b.size()) {
        QVariantMap row;
        row.insert(QStringLiteral("no"), ++no);
        row.insert(QStringLiteral("left"), QString());
        row.insert(QStringLiteral("right"), b.at(bi));
        row.insert(QStringLiteral("leftNo"), 0);
        row.insert(QStringLiteral("rightNo"), ++bi);
        row.insert(QStringLiteral("kind"), QStringLiteral("add"));
        ++m_addCount;
        m_rows.append(row);
    }

    finishRows();
    emit compared();
}

/*
 * 行表建好之后补上两样东西：每一行下面要空几行（leftGap / rightGap），
 * 和差异块清单（m_changes，给"上一个 / 下一个差异"和右边那条导航条）。
 *
 * gap 为什么挂在**上一行**：Scintilla 的行注释只能挂在行下面，没有"首行之上
 * 留空"这个口子。所以一段空行只能挂到它上面那条真行去；挂在最开头、上面没有
 * 真行可挂的那些，记进 leadGapLeft / leadGapRight，由界面写一句话说明。
 */
void DiffEngine::finishRows() {
    auto setRow = [&](int index, const QString &key, const QVariant &value) {
        QVariantMap row = m_rows.at(index).toMap();
        row.insert(key, value);
        m_rows[index] = row;
    };
    auto rowKind = [&](int index) {
        return m_rows.at(index).toMap().value(QStringLiteral("kind")).toString();
    };
    auto sideLine = [&](int index, bool left) {
        return m_rows.at(index).toMap()
            .value(left ? QStringLiteral("leftNo") : QStringLiteral("rightNo")).toInt();
    };

    for (const bool left : { true, false }) {
        int anchor = -1;      /* 最近一条真行在行表里的位置 */
        int pending = 0;      /* 它后面攒了多少行空行 */
        for (int i = 0; i < m_rows.size(); ++i) {
            if (sideLine(i, left) > 0) {
                if (pending > 0 && anchor >= 0)
                    setRow(anchor, left ? QStringLiteral("leftGap")
                                        : QStringLiteral("rightGap"), pending);
                pending = 0;
                anchor = i;
            } else {
                ++pending;
                if (anchor < 0) {
                    /* 文件最开头就缺行：没地方挂，单独记一笔给界面提示 */
                    if (left)
                        ++m_leadGapLeft;
                    else
                        ++m_leadGapRight;
                }
            }
        }
        /* 尾巴上缺的行也挂在最后一条真行下面（挂在末尾，视觉上就是"这一侧到此为止"） */
        if (pending > 0 && anchor >= 0)
            setRow(anchor, left ? QStringLiteral("leftGap") : QStringLiteral("rightGap"), pending);
    }

    /* 差异块：连续的"不是 same"合成一块 */
    int i = 0;
    while (i < m_rows.size()) {
        if (rowKind(i) == QLatin1String("same")) {
            ++i;
            continue;
        }
        const int start = i;
        bool sawDel = false;
        bool sawAdd = false;
        bool sawMod = false;
        int leftFrom = 0;
        int leftTo = 0;
        int rightFrom = 0;
        int rightTo = 0;
        while (i < m_rows.size() && rowKind(i) != QLatin1String("same")) {
            const QVariantMap row = m_rows.at(i).toMap();
            const QString kind = row.value(QStringLiteral("kind")).toString();
            sawDel = sawDel || kind == QLatin1String("del");
            sawAdd = sawAdd || kind == QLatin1String("add");
            sawMod = sawMod || kind == QLatin1String("mod");
            const int ln = row.value(QStringLiteral("leftNo")).toInt();
            const int rn = row.value(QStringLiteral("rightNo")).toInt();
            if (ln > 0) {
                if (leftFrom == 0)
                    leftFrom = ln;
                leftTo = ln;
            }
            if (rn > 0) {
                if (rightFrom == 0)
                    rightFrom = rn;
                rightTo = rn;
            }
            ++i;
        }
        QVariantMap change;
        change.insert(QStringLiteral("row"), start);
        /* 既有删又有加、或者成对改的，都算"改了"—— 用户找的就是这一处 */
        change.insert(QStringLiteral("kind"),
                      sawMod || (sawDel && sawAdd) ? QStringLiteral("mod")
                                                   : (sawDel ? QStringLiteral("del")
                                                             : QStringLiteral("add")));
        change.insert(QStringLiteral("leftStart"), qMax(0, leftFrom - 1));
        change.insert(QStringLiteral("leftCount"), qMax(0, leftTo - leftFrom + 1));
        change.insert(QStringLiteral("rightStart"), qMax(0, rightFrom - 1));
        change.insert(QStringLiteral("rightCount"), qMax(0, rightTo - rightFrom + 1));
        m_changes.append(change);
    }

    const int blocks = m_changes.size();
    m_summary = blocks == 0
                    ? (m_ignoreCase || m_ignoreWhitespace ? tr("两份内容相同（按当前忽略规则）")
                                                          : tr("两份内容完全相同"))
                    : tr("左 %1 行 / 右 %2 行：%3 处差异（改了 %4 行、左边多 %5 行、右边多 %6 行）")
                          .arg(m_leftLines)
                          .arg(m_rightLines)
                          .arg(blocks)
                          .arg(m_modCount)
                          .arg(m_delCount)
                          .arg(m_addCount);
    m_hasResult = true;
}

void DiffEngine::setIgnoreCase(bool on) {
    if (m_ignoreCase == on)
        return;
    m_ignoreCase = on;
    /* 没有历史可重算（还没对比过）就只是把开关存下来 */
    if (m_hasResult)
        compare(m_leftText, m_rightText, m_leftPath, m_rightPath);
    else
        emit compared();
}

void DiffEngine::setIgnoreWhitespace(bool on) {
    if (m_ignoreWhitespace == on)
        return;
    m_ignoreWhitespace = on;
    if (m_hasResult)
        compare(m_leftText, m_rightText, m_leftPath, m_rightPath);
    else
        emit compared();
}

QString DiffEngine::unifiedDiff() const {
    if (!m_hasResult)
        return QString();

    /*
     * 统一格式（--- / +++ / @@ -a,b +c,d @@）：能把结果带走的那一种形式，
     * 存成 .patch 之后 git apply / patch -p0 都认。
     */
    QString out;
    out += QStringLiteral("--- %1\n").arg(m_leftTitle);
    out += QStringLiteral("+++ %1\n").arg(m_rightTitle);

    /*
     * 按 hunk 归组：连续的差异行（中间隔 3 行以内的相同行也算同一块，
     * 和 git 的默认上下文一样）。
     */
    constexpr int kContext = 3;
    int i = 0;
    const int total = m_rows.size();
    while (i < total) {
        if (m_rows.at(i).toMap().value(QStringLiteral("kind")).toString()
            == QLatin1String("same")) {
            ++i;
            continue;
        }
        /* 找到一块：往前留 kContext 行上下文 */
        int start = qMax(0, i - kContext);
        int end = i;
        while (end < total) {
            const QString kind =
                m_rows.at(end).toMap().value(QStringLiteral("kind")).toString();
            if (kind != QLatin1String("same")) {
                end = qMin(total, end + kContext);
                continue;
            }
            /* 相同行：看看后面 kContext 行内还有没有差异，有就并进这一块 */
            bool more = false;
            for (int k = end; k < qMin(total, end + kContext * 2 + 1); ++k) {
                if (m_rows.at(k).toMap().value(QStringLiteral("kind")).toString()
                    != QLatin1String("same")) {
                    more = true;
                    break;
                }
            }
            if (!more)
                break;
            ++end;
        }
        end = qMin(total, end);

        int leftStart = 0;
        int rightStart = 0;
        int leftCount = 0;
        int rightCount = 0;
        QString body;
        for (int k = start; k < end; ++k) {
            const QVariantMap row = m_rows.at(k).toMap();
            const QString kind = row.value(QStringLiteral("kind")).toString();
            const int ln = row.value(QStringLiteral("leftNo")).toInt();
            const int rn = row.value(QStringLiteral("rightNo")).toInt();
            if (leftStart == 0 && ln > 0)
                leftStart = ln;
            if (rightStart == 0 && rn > 0)
                rightStart = rn;
            if (ln > 0)
                ++leftCount;
            if (rn > 0)
                ++rightCount;
            if (kind == QLatin1String("same"))
                body += QLatin1Char(' ') + row.value(QStringLiteral("left")).toString()
                        + QLatin1Char('\n');
            else if (kind == QLatin1String("mod")) {
                body += QLatin1Char('-') + row.value(QStringLiteral("left")).toString()
                        + QLatin1Char('\n');
                body += QLatin1Char('+') + row.value(QStringLiteral("right")).toString()
                        + QLatin1Char('\n');
            } else if (kind == QLatin1String("del")) {
                body += QLatin1Char('-') + row.value(QStringLiteral("left")).toString()
                        + QLatin1Char('\n');
            } else {
                body += QLatin1Char('+') + row.value(QStringLiteral("right")).toString()
                        + QLatin1Char('\n');
            }
        }
        out += QStringLiteral("@@ -%1,%2 +%3,%4 @@\n")
                   .arg(qMax(1, leftStart))
                   .arg(leftCount)
                   .arg(qMax(1, rightStart))
                   .arg(rightCount);
        out += body;
        i = end;
    }
    return out;
}

