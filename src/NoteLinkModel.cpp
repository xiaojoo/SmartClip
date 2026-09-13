#include "NoteLinkModel.h"

#include "NoteThumbs.h"

#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace {

/*
 * 正文里的链接长这样：http://x、https://x、www.x。
 *
 * 为什么只认这三种：便签是随手记的地方，"看起来像域名"的东西太多
 * （文件名 window.cpp、版本号 1.2.3、邮箱 a@b.com 里的 b.com…），
 * 全都当链接渲染出一堆缩略图卡片比漏掉几条更烦人。带 scheme 和 www. 的
 * 写法没有歧义，正是用户想要的"这是个链接"。
 */
const QRegularExpression &urlPattern() {
    static const QRegularExpression re(
        QStringLiteral("(?:https?://|www\\.)[^\\s<>\"'`\\[\\]{}|\\\\^]+"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

/* 句子结尾的标点不算地址的一部分（正文里写成"…详见 https://x.com/a。" 很常见） */
bool isTrailingPunctuation(QChar c) {
    static const QString marks = QStringLiteral(".,;:!?)]}>\"'。，、；：！？）】》」』…·");
    return marks.contains(c);
}

/* 地址结尾的这几样要剪掉，但剪到一半就剩不下东西了就说明这条不算 */
QString trimUrl(QString text) {
    while (!text.isEmpty() && isTrailingPunctuation(text.back()))
        text.chop(1);
    return text;
}

QString normalize(const QString &raw) {
    if (raw.startsWith(QLatin1String("www."), Qt::CaseInsensitive))
        return QStringLiteral("https://") + raw;
    return raw;
}

}  // namespace

NoteLinkModel::NoteLinkModel(NoteThumbs *thumbs, QObject *parent)
    : QAbstractListModel(parent), m_thumbs(thumbs) {
    if (m_thumbs) {
        connect(m_thumbs, &NoteThumbs::thumbnailReady, this, &NoteLinkModel::onThumbnailReady);
    }
}

NoteLinkModel::~NoteLinkModel() = default;

int NoteLinkModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return int(m_rows.size());
}

QVariant NoteLinkModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return QVariant();

    const Row &row = m_rows.at(index.row());
    switch (role) {
    case UrlRole:      return row.link.url;
    case TitleRole:    return row.title.isEmpty() ? row.link.host : row.title;
    case HostRole:     return row.link.host;
    case LineRole:     return row.link.line;
    case ColumnRole:   return row.link.column;
    case LengthRole:   return row.link.length;
    case ReadyRole:    return row.ready;
    case ThumbVersionRole: return row.version;
    case ThumbSourceRole:
        return QStringLiteral("image://stickythumb/%1?v=%2")
            .arg(row.cacheKey)
            .arg(row.version);
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> NoteLinkModel::roleNames() const {
    return {
        {UrlRole, "url"},
        {TitleRole, "title"},
        {HostRole, "host"},
        {LineRole, "line"},
        {ColumnRole, "column"},
        {LengthRole, "length"},
        {ReadyRole, "ready"},
        {ThumbVersionRole, "thumbVersion"},
        {ThumbSourceRole, "thumbSource"},
    };
}

QList<NoteLink> NoteLinkModel::parse(const QString &text, const NoteThumbs *thumbs) {
    QList<NoteLink> found;
    if (text.isEmpty())
        return found;

    QSet<QString> seen;
    /*
     * 行号 / 列号自己数。
     *
     * 不用 QRegularExpressionMatch::capturedStart() 换算行号：便签正文很短
     * （几十行以内），一次线性扫描比"从上一个匹配位置数到这一个"更简单，
     * 也不受 \r\n 这类换行的影响（数 \n 就够）。
     */
    int line = 0;
    int lineStart = 0;

    auto it = urlPattern().globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const int start = match.capturedStart();
        const QString raw = trimUrl(match.captured());
        if (raw.isEmpty())
            continue;

        /* 补上扫描到这一点经过的换行 */
        for (int i = lineStart; i < start; ++i) {
            if (text.at(i) == QLatin1Char('\n')) {
                ++line;
                lineStart = i + 1;
            }
        }

        const QString url = normalize(raw);
        const QUrl parsed(url);
        /*
         * 域名里得有个点（localhost / 单段主机名不当链接），长度也拦一道
         * ——"http://" 后面跟一个字符这种不是链接。
         */
        const QString host = parsed.host();
        if (host.isEmpty() || !host.contains(QLatin1Char('.')) || url.size() < 12)
            continue;
        if (seen.contains(url))
            continue;
        seen.insert(url);

        NoteLink link;
        link.url = url;
        link.raw = raw;
        link.host = host.startsWith(QLatin1String("www.")) ? host.mid(4) : host;
        link.line = line;
        link.column = start - lineStart;
        link.length = raw.size();
        /* 标题：缓存里有就用（见 NoteThumbs），没有先用域名顶上 */
        link.title = thumbs ? thumbs->titleFor(url) : QString();
        found.append(link);

        /* 一张便签最多摆这么多张卡片：再多就不是"便签"了，也拖慢卡片列表 */
        if (found.size() >= 12)
            break;
    }
    return found;
}

void NoteLinkModel::setText(const QString &text) {
    rebuild(parse(text, m_thumbs));
}

/*
 * 正文重抽之后和现在这份做差分。
 *
 * 关键：**按 URL 复用**。便签里最常见的一次改动是"在链接后面补一句说明"，
 * 这时候链接那一行的 row 原地不动（缩略图、标题都不用重来），只有位置
 * （line / column）变一下 —— 所以这里逐行比较，只有真变了才发 dataChanged。
 */
void NoteLinkModel::rebuild(const QList<NoteLink> &links) {
    /* 先把"现在这份"按 URL 收成一张表 */
    QHash<QString, Row> old;
    for (const Row &row : std::as_const(m_rows))
        old.insert(row.link.url, row);

    QList<Row> next;
    next.reserve(links.size());

    /*
     * 列表形状变了（多 / 少一条链接）就用 beginResetModel 整块换。
     *
     * 为什么不逐行 insert/remove：QML 那边 ListView 的行高是按内容算的，
     * 逐行增删要连着发一串 begin/endInsertRows，路径长、容易在动画中途
     * 对不上；而且链接卡片数量本来就只有几条，整块换的开销可以忽略。
     * 缩略图状态在 Row 里带着走（下面的 old 表），所以不会重新联网。
     */
    const bool sameShape = (links.size() == m_rows.size());

    if (sameShape) {
        /* 形状没变：逐行更新，位置变了的那几条发 dataChanged */
        for (int i = 0; i < links.size(); ++i) {
            const NoteLink &link = links.at(i);
            Row row = old.value(link.url);
            row.link = link;
            if (row.title.isEmpty() && m_thumbs)
                row.title = m_thumbs->titleFor(link.url);
            if (row.cacheKey.isEmpty())
                row.cacheKey = cacheKeyFor(link.url);
            next.append(row);
        }
        m_rows = next;
        if (!m_rows.isEmpty())
            emit dataChanged(index(0), index(int(m_rows.size()) - 1));
        /*
         * 行数没变，但这一趟可能是"上一条链接没了、下一条接上"—— 卡片上的
         * count 绑定（QML 那栏显不显示）也要重算一次。
         */
        emit countChanged();
    } else {
        beginResetModel();
        for (const NoteLink &link : links) {
            Row row = old.value(link.url);
            row.link = link;
            if (row.title.isEmpty() && m_thumbs)
                row.title = m_thumbs->titleFor(link.url);
            if (row.cacheKey.isEmpty())
                row.cacheKey = cacheKeyFor(link.url);
            next.append(row);
        }
        m_rows = next;
        endResetModel();
        emit countChanged();
    }

    /* 新出现的链接去要缩略图（已有的 request() 会立刻回一个 ready） */
    if (m_thumbs) {
        for (const Row &row : std::as_const(m_rows))
            m_thumbs->request(row.link.url);
    }

    recount();
}

QString NoteLinkModel::cacheKeyFor(const QString &url) {
    return NoteThumbs::cacheKeyFor(url);
}

int NoteLinkModel::indexOfUrl(const QString &url) const {
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).link.url == url)
            return i;
    }
    return -1;
}

void NoteLinkModel::onThumbnailReady(const QString &url) {
    const int row = indexOfUrl(url);
    if (row < 0)
        return;

    Row &entry = m_rows[row];
    /*
     * 版本 +1：QML 侧 Image.source 里带着它，值一变 QQuickPixmapCache 就当
     * 新 URL 重新取图（不然会一直用"还没有图"那一次的空缓存）。失败时也 +1
     * —— 卡片要把"加载中"收掉、退回占位块。
     */
    ++entry.version;
    entry.ready = m_thumbs && !m_thumbs->imageForCacheKey(entry.cacheKey).isNull();
    if (m_thumbs) {
        const QString title = m_thumbs->titleFor(url);
        if (!title.isEmpty())
            entry.title = title;
        if (entry.cacheKey.isEmpty())
            entry.cacheKey = cacheKeyFor(url);
    }

    /*
     * 整块列表都发一次 dataChanged，而不是只发这一行。
     *
     * 为什么：Repeater 的委托里 `source: thumbSource` 是**整条链重新求值**
     * 才更新的（模型只发某一行时，那一行之外的委托不会重算，而这里真正要
     * 改的正是"某一条的缩略图地址变了"）。卡片一屏最多十几张，整块发一次
     * 比按角色精细通知更省事也更可靠 —— 见 NoteLinkModel 开头那段说明。
     */
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(int(m_rows.size()) - 1));
    recount();
}

void NoteLinkModel::recount() {
    int pending = 0;
    for (const Row &row : std::as_const(m_rows)) {
        if (!row.ready)
            ++pending;
    }
    if (pending != m_pending) {
        m_pending = pending;
        emit pendingChanged();
    }
}
