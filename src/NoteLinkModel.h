#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QString>

class NoteThumbs;

/*
 * 一条被识别出来的链接。
 *
 * line / column 是它在正文里的位置：点链接卡片上那个"在正文里定位"会把编辑区
 * 滚到那一行并选中它（见 qml/notes/StickyNoteWindow.qml）。留这个口子而不是
 * 把卡片做得更花哨，是因为便签的正文才是内容本身，卡片只是它的一个预览。
 */
struct NoteLink {
    QString url;      /* 规范化之后的完整地址（补过 scheme） */
    QString raw;      /* 正文里原样那一串（选中正文时用） */
    QString host;     /* 域名，卡片上显示的那一行小字 */
    QString title;    /* 网页标题（缩略图抓到之后才有），没有就退回域名 */
    int line = 0;     /* 从 0 数 */
    int column = 0;   /* 从 0 数 */
    int length = 0;   /* raw 的字符数 */
};

/*
 * 便签正文里那几条链接（QML 的链接卡片列表就绑在它上面）。
 *
 * 为什么单独一个模型：正文每敲一个字都要重新抽一遍链接，而卡片是按 URL
 * 复用的（同一份便签里链接常常是"补一句说明、链接不动"）—— 用模型按 URL
 * 做差分，已抓到的缩略图不用重抓，卡片也不会整条重建（重建会闪一下）。
 *
 * 缩略图：每条链接向 NoteThumbs 要一次图，拿到之后
 * thumbVersion 那个 role +1 —— QML 侧 Image.source 里带着它当查询串，
 * 值一变 QQuickPixmapCache 就当新 URL 重新取（不然会一直用"还没图"那次
 * 的缓存，永远显示占位块）。这套和截图那边 image://shot/<serial> 是同一个道理。
 */
class NoteLinkModel final : public QAbstractListModel {
    Q_OBJECT

    /* 卡片列表要不要显示（正文里一条链接都没有时 QML 把那一栏收掉） */
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    /* 还没抓到缩略图的条数（界面可以显示"缩略图加载中"） */
    Q_PROPERTY(int pending READ pending NOTIFY pendingChanged)
    /*
     * LineRole 的数值。
     *
     * QML 里够不到 C++ 的 enum（这个类是用上下文属性 / 属性暴露给 QML 的，
     * 没走 qmlRegisterType），而"把正文滚到第几条链接那一行"要按 role 取值。
     * 与其在 QML 里写死一个 260 这种魔数，不如从这里读 —— 枚举改了它跟着改。
     */
    Q_PROPERTY(int lineRole READ lineRole CONSTANT)
    /*
     * TitleRole / UrlRole 的数值。同上：链接卡片上那几个字（标题 / 地址）要按
     * role 取值，QML 这边够不到 C++ 的 enum。
     * （菜单里那栏"正文里的链接"已经删了，这两个 role 现在只给卡片用。）
     */
    Q_PROPERTY(int titleRole READ titleRole CONSTANT)
    Q_PROPERTY(int urlRole READ urlRole CONSTANT)

public:
    enum Roles {
        UrlRole = Qt::UserRole + 1,
        TitleRole,
        HostRole,
        LineRole,
        ColumnRole,
        LengthRole,
        /* image://stickythumb/<cacheKey>?v=<thumbVersion>：QML 直接拿去当 source */
        ThumbSourceRole,
        ThumbVersionRole,
        ReadyRole,      /* 缩略图取到了没（没取到先画占位块） */
    };
    Q_ENUM(Roles)

    explicit NoteLinkModel(NoteThumbs *thumbs, QObject *parent = nullptr);
    ~NoteLinkModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /*
     * 正文变了：重新抽链接并和现在这份做差分。
     *
     * 只有真的变了才发信号（QML 的 TextEdit.onTextChanged 每一击都来一次），
     * 而且保留没变的那些行的缩略图状态 —— 见这个类开头的说明。
     */
    void setText(const QString &text);

    /*
     * 从正文里抽出全部链接（静态是为了自检能单独调它，不用建模型）。
     *
     * thumbs 只用来给每条链接的 title 填上已经缓存下来的网页标题（可为空）。
     *
     * 认三种写法：http://…、https://…、www.…；结尾的标点（。，、）】等）
     * 不算地址的一部分，会被剪掉 —— 正文里"详见 https://x.com/a。" 这种情况
     * 很常见，带着句号去抓页面是抓不到的。
     */
    static QList<NoteLink> parse(const QString &text, const NoteThumbs *thumbs = nullptr);

    int pending() const { return m_pending; }
    int lineRole() const { return LineRole; }
    int titleRole() const { return TitleRole; }
    int urlRole() const { return UrlRole; }

    /* 缩略图抓好了：刷新对应那一条（NoteThumbs::thumbnailReady 转过来） */
    void onThumbnailReady(const QString &url);

signals:
    void countChanged();
    void pendingChanged();

private:
    struct Row {
        NoteLink link;
        QString cacheKey;
        QString title;
        int version = 0;
        bool ready = false;
    };

    void rebuild(const QList<NoteLink> &links);
    static QString cacheKeyFor(const QString &url);
    void recount();
    int indexOfUrl(const QString &url) const;

    NoteThumbs *m_thumbs = nullptr;
    QList<Row> m_rows;
    int m_pending = 0;
};
