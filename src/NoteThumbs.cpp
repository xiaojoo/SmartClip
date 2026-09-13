#include "NoteThumbs.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQuickImageProvider>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QtGlobal>

#include <utility>

namespace {

/* 缓存目录名（AppDataLocation 下面那一个） */
constexpr char kCacheDirName[] = "thumbnails";
/* 缓存索引文件名 */
constexpr char kIndexName[] = "index.json";

/* 拉网页时只取前面这一段：og:image / <title> 都在 <head> 里，正文不用下 */
constexpr qint64 kHeadBytes = 96 * 1024;
/* 缩略图本体最大收多少（超过就不接了，别为一张预览图吃掉几十兆内存） */
constexpr int kImageBytes = 6 * 1024 * 1024;
/* 卡片上那张图的尺寸上限（QML 侧再按 dpr 放大，这里留够像素） */
constexpr int kThumbW = 480;
constexpr int kThumbH = 360;
/* 单次网络请求的超时 */
constexpr int kTimeoutMs = 12000;
/* mShots 头几次会返回"正在生成"的占位图，隔一段时间重试（见 .cpp 的说明） */
constexpr int kShotsRetry = 3;
constexpr int kShotsRetryDelayMs = 2500;
/* 取不到图的链接：这么久之内不再试（不然每敲一个字就重排一次队） */
constexpr qint64 kFailTtlMs = 10 * 60 * 1000;
/* 内存里最多留几张图 */
constexpr int kMemLimit = 48;

/*
 * 一次正常浏览器的 User-Agent。
 *
 * 不设的话 Qt 默认那句 "Mozilla/5.0" 会被不少站点直接拒（403），
 * og:image 也就取不到了。
 */
QByteArray browserUserAgent() {
    return QByteArrayLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                             "AppleWebKit/537.36 (KHTML, like Gecko) "
                             "Chrome/126.0 Safari/537.36 SmartClip/0.1");
}

QString hostOf(const QString &url) {
    const QString host = QUrl(url).host();
    return host.startsWith(QLatin1String("www.")) ? host.mid(4) : host;
}

/* 这个 Content-Type 像不像一张能解码的图（SVG 不算：QImage 解不了） */
bool looksLikeImage(const QString &contentType) {
    const QString type = contentType.toLower();
    if (type.startsWith(QLatin1String("image/svg")))
        return false;
    return type.startsWith(QLatin1String("image/"));
}

}  // namespace

/*
 * image://stickythumb/<cacheKey> 的实现。
 *
 * 挂在 NoteThumbs.cpp 里、而不是单开一对文件：它就一个 requestImage，
 * 而且必须和 NoteThumbs 的缓存读写绑在一起（见 imageForCacheKey）。
 * 多继承这里也说明了为什么：QQuickImageProvider 不是 QObject，
 * 而引擎接管所有权、需要父子关系的时候又得是个 QObject。
 */
namespace {

class ThumbProvider final : public QQuickImageProvider {
public:
    explicit ThumbProvider(NoteThumbs *owner)
        : QQuickImageProvider(QQuickImageProvider::Image), m_owner(owner) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requested) override {
        /* id 形如 "<md5>" 或 "<md5>?v=3"（查询串是 QML 侧用来破缓存的） */
        QString key = id;
        const int mark = key.indexOf(QLatin1Char('?'));
        if (mark >= 0)
            key = key.left(mark);

        QImage image = m_owner ? m_owner->imageForCacheKey(key) : QImage();
        if (size)
            *size = image.size();
        if (requested.isValid() && requested.width() > 0 && !image.isNull())
            image = image.scaled(requested, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        return image;
    }

private:
    NoteThumbs *m_owner = nullptr;
};

}  // namespace

NoteThumbs::NoteThumbs(QObject *parent) : QObject(parent) {
    /*
     * 网络管理器不挂父子关系：它的生死由 shutdown() 管（见那里为什么必须
     * 在事件循环还活着的时候拆掉它）。挂给 this 的话会拖到 ~QObject 才拆，
     * 那时 QApplication 已经开始收尾，在飞的 reply 一收尾就踩空。
     */
    m_net = new QNetworkAccessManager;
    m_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QLatin1Char('/') + QLatin1String(kCacheDirName);
    /* 目录建出来：缓存读写都指望它在（见 setCacheDir 的说明） */
    QDir().mkpath(m_dir);
    load();
}

NoteThumbs::~NoteThumbs() {
    /*
     * 兜底：正常路径上 StickyNotes::shutdown() 已经收过一遍了（那时事件循环
     * 还在，才是最安全的时机）。这里只是"没人调 shutdown 也不能漏"。
     */
    shutdown();
}

void NoteThumbs::shutdown() {
    /*
     * 在跑的请求：先掐断信号，再 abort，最后删掉状态。
     *
     * 顺序有讲究 —— abort() 会**同步**发一次 finished()，而那个回调
     * （onPage / onImage）会去 read() 这个 reply、还会回删 Fetch 自己。
     * 所以必须先把连接断开，abort 之后那些回调就不会再进来。
     */
    const QList<Fetch *> pending = m_pending.values();
    m_pending.clear();
    for (Fetch *fetch : pending) {
        if (fetch->reply) {
            QNetworkReply *reply = fetch->reply;
            fetch->reply = nullptr;
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
        delete fetch;
    }

    if (m_net) {
        m_net->disconnect(this);
        delete m_net;
        m_net = nullptr;
    }

    flush();
}

QQuickImageProvider *NoteThumbs::imageProvider() {
    if (!m_provider)
        m_provider = new ThumbProvider(this);
    return m_provider;
}

void NoteThumbs::setCacheDir(const QString &dir) {
    if (dir.isEmpty() || dir == m_dir)
        return;
    m_dir = dir;
    /*
     * 目录当场建出来（load / 取图都指望它存在）。指到临时目录（自检）时
     * 它通常还不存在 —— 等到真要存图才 mkpath 的话，中间那几次读取会
     * 悄悄落空，症状是"图存下来了却取不到"。
     */
    QDir().mkpath(m_dir);
    m_titles.clear();
    m_images.clear();
    m_mem.clear();
    m_failed.clear();
    load();
}

QString NoteThumbs::md5(const QString &text) {
    return QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Md5).toHex());
}

QString NoteThumbs::cacheKeyFor(const QString &url) {
    return md5(url);
}

bool NoteThumbs::load() {
    m_titles.clear();
    m_images.clear();
    m_mem.clear();

    const QString path = m_dir + QLatin1Char('/') + QLatin1String(kIndexName);
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return false;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject())
        return false;

    const QJsonObject items = doc.object().value(QStringLiteral("items")).toObject();
    for (auto it = items.begin(); it != items.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        const QString title = entry.value(QStringLiteral("title")).toString();
        const QString image = entry.value(QStringLiteral("image")).toString();
        if (!title.isEmpty())
            m_titles.insert(it.key(), title);
        /*
         * 图那一项要确认文件**真的还在**。
         *
         * 索引里记的是绝对路径，缓存目录被人清掉（或者整个 AppData 被搬走）
         * 之后路径就成了空指针 —— 照收的话 request() 会认为"已经有图了"，
         * 于是既不给界面图、也不再联网去取，卡片永远停在"载入中"。
         */
        if (!image.isEmpty() && QFileInfo::exists(image))
            m_images.insert(it.key(), image);
    }
    return true;
}

bool NoteThumbs::flush() {
    if (m_dir.isEmpty())
        return false;
    if (!QDir().mkpath(m_dir))
        return false;

    QJsonObject items;
    /*
     * 标题和图的 key 集合不一定一样（有网页声明了标题却没有可用图），
     * 取并集写出去。
     */
    for (auto it = m_titles.begin(); it != m_titles.end(); ++it) {
        QJsonObject entry = items.value(it.key()).toObject();
        entry.insert(QStringLiteral("title"), it.value());
        items.insert(it.key(), entry);
    }
    for (auto it = m_images.begin(); it != m_images.end(); ++it) {
        QJsonObject entry = items.value(it.key()).toObject();
        entry.insert(QStringLiteral("image"), it.value());
        items.insert(it.key(), entry);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("items"), items);

    const QString path = m_dir + QLatin1Char('/') + QLatin1String(kIndexName);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();
    return true;
}

QString NoteThumbs::titleFor(const QString &url) const {
    return m_titles.value(url);
}

QImage NoteThumbs::imageForCacheKey(const QString &cacheKey) const {
    if (cacheKey.isEmpty())
        return QImage();

    if (const auto mem = m_mem.constFind(cacheKey); mem != m_mem.constEnd())
        return mem.value();

    /*
     * m_mem 是按 cacheKey 缓的，这里要从 cacheKey 找回文件：遍历 m_images
     * 反查（条目数就是便签里的链接数，几十条以内，不值得再建一张反查表）。
     */
    QString file;
    for (auto it = m_images.constBegin(); it != m_images.constEnd(); ++it) {
        if (cacheKeyFor(it.key()) == cacheKey) {
            file = it.value();
            break;
        }
    }
    if (file.isEmpty())
        return QImage();

    QImage image(file);
    if (image.isNull())
        return QImage();
    m_mem.insert(cacheKey, image);
    while (m_mem.size() > kMemLimit)
        m_mem.erase(m_mem.begin());
    return image;
}

void NoteThumbs::request(const QString &url) {
    if (url.isEmpty())
        return;
    /*
     * 收工之后不再联网（见 shutdown）：那些便签窗口可能还在被界面引用，
     * 它们问一句"有没有图"是正常的，但这时候不该再发请求。
     */
    if (!m_net)
        return;

    /* 已经有图 / 标题了：图缓存到磁盘上就不再联网了 */
    if (m_images.contains(url)) {
        emit thumbnailReady(url);
        return;
    }
    /* 有标题没图：网页声明过标题，但图没取到 —— 也算"结果"（卡片上有字） */
    if (m_titles.contains(url) && !m_pending.contains(url)) {
        emit thumbnailReady(url);
        return;
    }
    if (m_pending.contains(url))
        return;

    /* 最近失败过的先不再试（见 kFailTtlMs） */
    if (const auto bad = m_failed.constFind(url); bad != m_failed.constEnd()) {
        if (QDateTime::currentMSecsSinceEpoch() - bad.value() < kFailTtlMs)
            return;
        m_failed.remove(url);
    }

    startFetch(url);
}

void NoteThumbs::startFetch(const QString &url) {
    auto *fetch = new Fetch;
    fetch->url = url;
    fetch->pageUrl = url;
    m_pending.insert(url, fetch);

    QNetworkRequest req{QUrl(fetch->pageUrl)};
    req.setHeader(QNetworkRequest::UserAgentHeader, browserUserAgent());
    /*
     * 只要开头这段就够解析 <head> 了。服务器不支持 Range 也没关系
     * （会整份发回来，见 onPage 里只截前 kHeadBytes 解析）。
     */
    req.setRawHeader(QByteArrayLiteral("Range"),
                     QByteArrayLiteral("bytes=0-") + QByteArray::number(kHeadBytes));
    req.setRawHeader(QByteArrayLiteral("Accept"),
                     QByteArrayLiteral("text/html,application/xhtml+xml"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(kTimeoutMs);

    fetch->reply = m_net->get(req);
    QNetworkReply *reply = fetch->reply;
    connect(reply, &QNetworkReply::finished, this, [this, fetch]() { onPage(fetch, QByteArray()); });
}

void NoteThumbs::fetchShots(Fetch *fetch) {
    ++fetch->attempt;
    fetch->fromShots = true;

    /*
     * mShots：给个 URL 它返回一张网页截图。
     *
     * w / vpw / vph 这套参数是从它的公开用法里来的：vpw/vph 是"虚拟视口"尺寸，
     * 因为显示宽度只有卡片那么大，视口开太宽文字会小到看不清。第一版请求
     * 它往往返回一张"生成中"的占位图（灰底 + 站名），所以隔几秒重试几次 ——
     * 重试的时候带一个时间戳参数，不然 CDN 会把那张占位图缓存给我们。
     */
    QUrl shots(QStringLiteral("https://s0.wp.com/mshots/v1/"));
    shots.setPath(shots.path() + QString::fromUtf8(QUrl::toPercentEncoding(fetch->url)));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("w"), QString::number(kThumbW));
    query.addQueryItem(QStringLiteral("vpw"), QStringLiteral("768"));
    query.addQueryItem(QStringLiteral("vph"), QStringLiteral("480"));
    if (fetch->attempt > 1)
        query.addQueryItem(QStringLiteral("_r"),
                           QString::number(QDateTime::currentMSecsSinceEpoch()));
    shots.setQuery(query);

    QNetworkRequest req{shots};
    req.setHeader(QNetworkRequest::UserAgentHeader, browserUserAgent());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(kTimeoutMs);

    fetch->reply = m_net->get(req);
    QNetworkReply *reply = fetch->reply;
    connect(reply, &QNetworkReply::finished, this, [this, fetch]() { onImage(fetch, QByteArray()); });
}

void NoteThumbs::onPage(Fetch *fetch, const QByteArray &body) {
    QNetworkReply *reply = fetch->reply;
    fetch->reply = nullptr;
    if (!reply)
        return;

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString contentType =
        reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    const QByteArray data = body.isEmpty() ? reply->read(kHeadBytes) : body;
    const QNetworkReply::NetworkError error = reply->error();
    reply->deleteLater();

    /*
     * 有些站点（图片直链、或者把链接重定向到 CDN 的图）回来的就是一张图：
     * 那就直接当缩略图用，不用再去解析 <head>。
     */
    if (looksLikeImage(contentType)) {
        onImage(fetch, data);
        return;
    }

    if (error != QNetworkReply::NoError || data.isEmpty()
        || (status >= 400 && status != 0)) {
        /* 网页没拉下来：还有 mShots 兜底 */
        fetchShots(fetch);
        return;
    }

    const QString html = QString::fromUtf8(data);
    const QString title = pageTitle(html);
    if (!title.isEmpty()) {
        m_titles.insert(fetch->url, title);
        emit thumbnailReady(fetch->url);
    }

    const QString declared =
        metaContent(html, {QStringLiteral("og:image"), QStringLiteral("og:image:url"),
                           QStringLiteral("twitter:image"), QStringLiteral("twitter:image:src")});
    if (declared.isEmpty()) {
        fetchShots(fetch);
        return;
    }

    fetch->imageUrl = resolveUrl(fetch->pageUrl, declared);
    if (fetch->imageUrl.isEmpty()) {
        fetchShots(fetch);
        return;
    }

    fetch->fromShots = false;
    QNetworkRequest req{QUrl(fetch->imageUrl)};
    req.setHeader(QNetworkRequest::UserAgentHeader, browserUserAgent());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(kTimeoutMs);

    fetch->reply = m_net->get(req);
    QNetworkReply *imageReply = fetch->reply;
    connect(imageReply, &QNetworkReply::finished, this,
            [this, fetch]() { onImage(fetch, QByteArray()); });
}

void NoteThumbs::onImage(Fetch *fetch, const QByteArray &body) {
    QNetworkReply *reply = fetch->reply;
    fetch->reply = nullptr;
    if (!reply)
        return;

    const QNetworkReply::NetworkError error = reply->error();
    const QByteArray data = body.isEmpty() ? reply->read(kImageBytes) : body;
    const QString contentType =
        reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    reply->deleteLater();

    if (error != QNetworkReply::NoError || data.isEmpty()) {
        fail(fetch);
        return;
    }
    /* 网页声明的那张图其实是个 HTML 错误页 / SVG：换 mShots 兜底 */
    if (!fetch->fromShots && !contentType.isEmpty() && !looksLikeImage(contentType)) {
        fetchShots(fetch);
        return;
    }

    QImageReader reader;
    reader.setDecideFormatFromContent(true);
    /*
     * reader 不接管 device 的所有权，所以用 QBuffer 包一层字节数组时要**自己**
     * 管它的生死：栈上建、传地址，读完之后 reader 还活着也没关系
     * （它只在 read() 期间用这个 device）。
     */
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    reader.setDevice(&buffer);
    QImage image = reader.read();
    buffer.close();

    if (image.isNull()) {
        /* mShots 那张"正在生成"的占位图有时候解不出来，重试几次 */
        if (fetch->fromShots && fetch->attempt < kShotsRetry) {
            QTimer::singleShot(kShotsRetryDelayMs, this, [this, fetch]() {
                if (m_pending.contains(fetch->url))
                    fetchShots(fetch);
            });
            return;
        }
        fail(fetch);
        return;
    }

    /*
     * mShots 那张"正在生成"的占位图是一张真图（灰底写着站点名），解得出、
     * 内容也对不上。判断办法很土但有效：它只有几百像素宽。真截图按
     * kThumbW 出，宽度接近 kThumbW。
     */
    if (fetch->fromShots && fetch->attempt < kShotsRetry
        && image.width() < kThumbW / 2) {
        QTimer::singleShot(kShotsRetryDelayMs, this, [this, fetch]() {
            if (m_pending.contains(fetch->url))
                fetchShots(fetch);
        });
        return;
    }

    if (image.width() > kThumbW || image.height() > kThumbH)
        image = image.scaled(kThumbW, kThumbH, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    finish(fetch, image);
}

void NoteThumbs::finish(Fetch *fetch, const QImage &image) {
    const QString path = m_dir + QLatin1Char('/') + cacheKeyFor(fetch->url) + QStringLiteral(".png");
    /*
     * 缓存目录要**先**建出来：image.save() 到不存在的目录只会返回 false,
     * 而这里失败了不会报错、只记一条"这条链接没有缩略图"—— 症状是
     * "联网明明成功了，图却永远是占位块"，很难往回查。
     */
    QDir().mkpath(m_dir);

    const bool saved = image.save(path, "PNG");
    const QString url = fetch->url;

    m_pending.remove(url);
    delete fetch;

    if (saved) {
        m_images.insert(url, path);
        m_mem.insert(cacheKeyFor(url), image);
        if (m_titles.value(url).isEmpty())
            m_titles.insert(url, hostOf(url));
    } else {
        m_failed.insert(url, QDateTime::currentMSecsSinceEpoch());
    }

    flush();
    emit thumbnailReady(url);
}

void NoteThumbs::fail(Fetch *fetch) {
    const QString url = fetch->url;
    m_pending.remove(url);
    delete fetch;
    m_failed.insert(url, QDateTime::currentMSecsSinceEpoch());
    /* 失败也要发一次：卡片那边要把"加载中"收掉，退回占位块 */
    emit thumbnailReady(url);
}

QString NoteThumbs::resolveUrl(const QString &base, const QString &ref) {
    const QUrl url(ref);
    if (url.isValid() && !url.scheme().isEmpty())
        return url.toString();
    const QUrl resolved = QUrl(base).resolved(QUrl(ref));
    if (!resolved.isValid() || resolved.scheme().isEmpty())
        return QString();
    return resolved.toString();
}

/*
 * 从 HTML 里读 <meta …> 的内容。
 *
 * 两个正则：一个管 content 在 name/property 后面，一个管反过来
 * （两种顺序的站点都有）。宽松匹配（属性之间可能有换行）——
 * 反正只在这一段 head 文本里找，不会误伤正文。
 */
QString NoteThumbs::metaContent(const QString &html, const QStringList &names) {
    for (const QString &name : names) {
        const QString quoted = QRegularExpression::escape(name);
        const QRegularExpression after(
            QStringLiteral("<meta[^>]*\\b(?:property|name)\\s*=\\s*[\"']%1[\"'][^>]*?\\bcontent\\s*=\\s*[\"']([^\"']+)[\"']")
                .arg(quoted),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression before(
            QStringLiteral("<meta[^>]*\\bcontent\\s*=\\s*[\"']([^\"']+)[\"'][^>]*?\\b(?:property|name)\\s*=\\s*[\"']%1[\"']")
                .arg(quoted),
            QRegularExpression::CaseInsensitiveOption);

        for (const QRegularExpression &re : {after, before}) {
            const QRegularExpressionMatch match = re.match(html);
            if (match.hasMatch()) {
                const QString value = match.captured(1).trimmed();
                if (!value.isEmpty())
                    return value;
            }
        }
    }
    return QString();
}

QString NoteThumbs::pageTitle(const QString &html) {
    QString title = metaContent(html, {QStringLiteral("og:title"), QStringLiteral("twitter:title")});
    if (title.isEmpty()) {
        const QRegularExpression re(QStringLiteral("<title[^>]*>([^<]*)</title>"),
                                    QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = re.match(html);
        if (match.hasMatch())
            title = match.captured(1).trimmed();
    }
    if (title.size() > 80)
        title = title.left(79) + QChar(0x2026);
    return title;
}
