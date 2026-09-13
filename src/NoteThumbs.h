#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QQuickImageProvider>
#include <QSet>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QQuickImageProvider;

/*
 * 链接的"网站缩略图"（便签里那条链接卡片上那张小图）。
 *
 * ---------------------------------------------------------------------------
 * 缩略图是怎么来的
 * ---------------------------------------------------------------------------
 * 分两步，第一步不成再走兜底：
 *
 *   1) **网页自己声明的那张图**。拉一次网页（只取前面几十 KB，见
 *      kHeadBytes），从 <meta property="og:image"> / twitter:image 里读出
 *      它自己给的分享图 —— 这是最像"这个网站的缩略图"的一张：站点自己挑的
 *      logo / 封面图，尺寸和构图都是给预览用的。顺手把 og:title / <title>
 *      也读出来（卡片上显示网站名）。
 *   2) **外部截图服务**（WordPress 的 mShots，https://s0.wp.com/mshots/v1/…）。
 *      网页没声明图、或者那张图没下下来时用它按 URL 现截一张。它是公开服务、
 *      不用 key，代价是要等它抓完（头几次请求会返回一张"正在生成"的占位图，
 *      见下面 kShotsRetry 的重试）。
 *
 * 两者都失败就记一条"这条链接没有缩略图"，卡片上退回一个占位块（QML 侧画），
 * 不会一直转圈重试 —— 见 m_failed。
 *
 * ---------------------------------------------------------------------------
 * 为什么整件事要放在 C++ 里
 * ---------------------------------------------------------------------------
 * Qt Quick 的 Image 只能"给个 URL 让它自己去取"，而这里要的是
 * "先取网页 -> 解析出图 -> 再取图 -> 缩放 -> 缓存到磁盘"这一串，还要跨启动
 * 复用（第二次打开同一份便签不该再联网）。QML 里没有网络请求的原语，
 * 所以取图 / 解析 / 缓存都在这里做，QML 只通过
 * image://stickythumb/<cacheKey> 拿现成的图（见 .cpp 里的 provider）。
 *
 * 缓存落在 QStandardPaths::AppDataLocation/thumbnails/：
 *   <md5(url)>.png           图本身
 *   index.json               每条链接的标题 / 有没有图（再启动时先用它把
 *                            卡片填出来，不用等联网）
 */
class NoteThumbs final : public QObject {
    Q_OBJECT

public:
    explicit NoteThumbs(QObject *parent = nullptr);
    ~NoteThumbs() override;

    /* 缓存目录（测试 / 自检可以指到临时目录） */
    QString cacheDir() const { return m_dir; }
    void setCacheDir(const QString &dir);

    /* 读 index.json（文件不在就是空的，不报错） */
    bool load();
    /* 立刻把 index.json 写下去 */
    bool flush();

    /*
     * 收工：掐掉所有在跑的请求、断开网络管理器、落盘。
     *
     * 进程退出时**必须**在事件循环还活着的时候调一次（见 StickyNotes::shutdown
     * 和 main.cpp 末尾）—— 不调的话，那些 QNetworkReply 会活到 QApplication
     * 析构之后，Qt 网络层在收尾时踩到已经拆掉的内部状态，退出那一刻崩
     * （实测：自检 457 项全过，退出码 0xC0000005，且和有没有便签窗口无关
     *  —— 只要便签子系统的网络管理器建过就会复现）。
     *
     * 幂等：调两次没关系。
     */
    void shutdown();

    /*
     * 要一条链接的缩略图。
     *
     * 已经有（内存 / 磁盘缓存）就什么都不做 —— 但还是会发一次
     * thumbnailReady()，因为调用方（NoteLinkModel）可能是刚建出来的，
     * 它还不知道这张图在不在。
     *
     * 正在取 / 已经失败过的也同样返回：不重复排队（见 m_pending / m_failed）。
     */
    void request(const QString &url);

    /*
     * 取图口（image://stickythumb/<cacheKey> 那个 provider 调的）。
     * 没有就返回空图 —— QML 侧显示占位块。
     */
    QImage imageForCacheKey(const QString &cacheKey) const;

    /*
     * 这条链接缓存里的标题（没有就空串）。NoteLinkModel 用它给卡片起标题，
     * 不用等缩略图回来。
     */
    QString titleFor(const QString &url) const;
    /* 这条链接的缓存 key（md5(url)），QML 侧拿它拼 image:// 地址 */
    static QString cacheKeyFor(const QString &url);

    /* 自检用：一共缓存了几条 */
    int cachedCount() const { return m_titles.size(); }

    /*
     * 供 QQmlEngine::addImageProvider 用（image://stickythumb/…）。
     *
     * QQuickImageProvider 不继承 QObject，所以这里是**多继承**的一个内部类：
     * QObject 那半截只是为了有个父子关系（provider 由引擎接管所有权），
     * 真正干活的 requestImage 转回本对象。返回的指针第一次调用时才建，
     * 之后一直复用 —— 引擎会 delete 它，所以不要在别处删。
     */
    QQuickImageProvider *imageProvider();

signals:
    /* 某条链接的缩略图 / 标题有结果了（成功失败都发，失败时 image 为空） */
    void thumbnailReady(const QString &url);

private:
    /* 一次抓取的全部中间状态（网页 -> 图片两跳，见 .cpp 的 startFetch） */
    struct Fetch {
        QString url;          /* 原始链接 */
        QString pageUrl;      /* 真正去拉的地址（补过 scheme） */
        QString imageUrl;     /* 网页声明的那张图 */
        QString title;
        int attempt = 0;      /* mShots 重试到第几次 */
        bool fromShots = false;
        class QNetworkReply *reply = nullptr;
    };

    void startFetch(const QString &url);
    void fetchShots(Fetch *fetch);
    void finish(Fetch *fetch, const QImage &image);
    void fail(Fetch *fetch);
    /* 网页回来了：解析 og:image / og:title，有了就去下图，没有就走 mShots */
    void onPage(Fetch *fetch, const QByteArray &body);
    /* 图回来了：缩放 + 存盘 + 发信号 */
    void onImage(Fetch *fetch, const QByteArray &body);

    static QString md5(const QString &text);
    static QString resolveUrl(const QString &base, const QString &ref);
    /* 从 HTML 头里读 <meta property="og:image" content="…"> 这类声明 */
    static QString metaContent(const QString &html, const QStringList &names);
    static QString pageTitle(const QString &html);

    QString m_dir;
    /*
     * 网络管理器。**唯一持有者**：shutdown() 里显式 delete 它，那时事件循环
     * 还在（见 shutdown 的说明）—— 不能留给 QObject 的父子析构去收尾，
     * 那样它会在 QApplication 拆掉之后才没，那些在飞的 reply 就踩空了。
     */
    QNetworkAccessManager *m_net = nullptr;
    /* 已经排队 / 在跑的（URL -> 状态）。同一份便签反复重抽链接不会重复联网 */
    QHash<QString, Fetch *> m_pending;
    /* 取不到图的（URL -> 什么时候记下的）：一段时间内不再试，见 kFailTtl */
    QHash<QString, qint64> m_failed;
    /* 缓存索引：URL -> { title, image } */
    QHash<QString, QString> m_titles;
    QHash<QString, QString> m_images;
    /* 内存里那份图（缩放过的），省一次读盘 */
    mutable QHash<QString, QImage> m_mem;
    /* image://stickythumb/… 的提供者（第一次要的时候才建，引擎接管所有权） */
    QQuickImageProvider *m_provider = nullptr;
};
