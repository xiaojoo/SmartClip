#pragma once

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QRect>
#include <QString>

class NoteLinkModel;
class NoteThumbs;

/*
 * 一条便签（数据 + 落盘）。
 *
 * 一个对象 = 桌面上一块便签的全部状态，同时**就是 QML 那份数据源**：
 * qml/notes/StickyNoteWindow.qml 直接绑在它的属性上（正文 / 底色 / 链接清单），
 * 编辑区里敲的字通过 setText() 回到这里，再由这里统一落盘（见 save()）。
 *
 * 为什么数据不放在 QML 里：便签窗口关掉（hide()）之后 QML 那棵树还在，
 * 但"下次启动要恢复哪些便签、摆在哪、什么底色"这件事得有个 C++ 侧的身份 ——
 * 就是这里的 id。窗口是照着这条数据建出来的，一份数据一条便签，不对应窗口
 * 的生死（隐藏了数据还在，见 StickyNotes::showAll）。
 *
 * 配色（bg 是便签纸的颜色）：文字 / 次要文字 / 边框都由 bg 算出来，
 * 见 textColor() 那几条 —— 便签纸有深有浅（柠檬黄到石墨黑），写死一个
 * 前景色在浅色纸上就看不见了。
 */
class StickyNote final : public QObject {
    Q_OBJECT

    /* 身份（UUID）。窗口 / 托盘菜单 / notes.json 之间靠它对上号 */
    Q_PROPERTY(QString id READ id CONSTANT)

    /* 正文（纯文本）。编辑区里每敲一下都会 setText() 回来 */
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(QString plainText READ plainText NOTIFY textChanged)

    /* 便签纸底色，形如 "#ffe9a8" */
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)

    /*
     * 由底色推出来的三档前景色（QML 里直接绑，不需要调函数）。
     *
     * 必须是**属性**而不是 Q_INVOKABLE：QML 里读 `note.textColor` 时，
     * 如果它是个方法，拿到的是那个函数对象本身（绑到 color 上就报
     * "Unable to assign a function to a property of any type other than var"
     * —— 自检的 stderr 里刷了几十条），得写成 `note.textColor()` 才是调用，
     * 而那样又拿不到 NOTIFY（换底色时界面不会跟着变）。
     */
    Q_PROPERTY(QColor textColor READ textColor NOTIFY colorChanged)
    Q_PROPERTY(QColor mutedTextColor READ mutedTextColor NOTIFY colorChanged)
    Q_PROPERTY(QColor shadeColor READ shadeColor NOTIFY colorChanged)
    /*
     * 这张便签纸是不是深色的（亮度低于阈值）。
     *
     * QML 侧要按它挑"压在纸上的黑"还是"压在纸上的白"（描边 / 分隔线那几处
     * 用透明度画在纸上，深浅纸上要的不是同一个色）。在 C++ 侧算一次，
     * 界面就不用再写一遍亮度公式、更不用去比 "#1e2024" 这种字面色值。
     */
    Q_PROPERTY(bool darkPaper READ darkPaper NOTIFY colorChanged)

    /* 正文里抽出来的链接（缩略图卡片那条，见 NoteLinkModel） */
    Q_PROPERTY(QObject *links READ links CONSTANT)

    /* 便签是不是摆在桌面上（关掉 = 藏起来，数据还在） */
    Q_PROPERTY(bool visible READ visible WRITE setVisible NOTIFY visibleChanged)

    /*
     * 便签纸的不透明度（0.35 ~ 1.0）。
     *
     * Windows 便签那个菜单里有 Opacity —— 半透明的便签能盖在文档上看，
     * 又不完全挡住底下的东西。档位见 opacityPercent / setOpacityPercent。
     */
    Q_PROPERTY(qreal opacity READ opacity WRITE setOpacity NOTIFY opacityChanged)
    Q_PROPERTY(int opacityPercent READ opacityPercent NOTIFY opacityChanged)

    /* 这条便签在这块屏幕上的位置和尺寸（逻辑像素） */
    Q_PROPERTY(QRect geometry READ geometry NOTIFY geometryChanged)

    /*
     * 这条便签属于哪一摞"组合"（空串 = 没组合，单独一块）。
     *
     * 组合就是 Windows 便签那种**归到一起**：把一块便签的头部拖到另一块身上
     * 松手，几块就成一摞（见 StickyNotes::dropNoteOn）。归到一摞之后整摞一起
     * 搬、一起收；摆成"露头 + 依次错开"的样子是**排列**那件事，见
     * StickyNotes::applyGroupLayout。
     *
     * 为什么存成一个 id 而不是"一堆指针"：组合要跨进程重启活下来。几块便签
     * 在 notes.json 里各自记一句"我属于哪一摞"，恢复时按这个 id 重新凑成一摞
     * —— 谁在最上面由 notes.json 里 groups 那一段（哪块是露头的）决定。
     *
     * 组 id 本身只是个自增的短串（"g1"、"g2"…），不落盘也别的地方引用它，
     * 所以不需要 UUID 那么长的东西。
     */
    Q_PROPERTY(QString groupId READ groupId WRITE setGroupId NOTIFY groupIdChanged)

public:
    explicit StickyNote(QObject *parent = nullptr);
    /*
     * thumbs 是缩略图抓取器（可为空：那样链接卡片只有标题没有图，
     * 自检里测链接解析时就是这种用法）。它**不归便签所有**，
     * 一份进程里只有 StickyNotes 建的那一个。
     */
    StickyNote(const QString &id, NoteThumbs *thumbs = nullptr, QObject *parent = nullptr);
    ~StickyNote() override;

    /*
     * 便签纸可选的颜色。第一项是新建时的默认色。
     *
     * 排在前面的几个是经典的便签黄 / 粉 / 绿 / 蓝，后面几个是深色纸
     * —— 由 StickyNote::textColor() 自动配浅色字，两种都能用。
     */
    static const QStringList &palette();

    QString id() const { return m_id; }

    QString text() const { return m_text; }
    /*
     * QML 的 TextEdit 绑在 text 上，用户每敲一下都会写回这里 —— 直接改
     * 属性的写法（不是 setText()），所以它同时是"写入口"。
     */
    void setText(const QString &text);

    /*
     * TextEdit.getText() 那份语义的正文。
     *
     * 现在和 text() 是同一份（正文就是纯文本）；留着这个名字是为了让 QML
     * 侧读"编辑器里的字"时有个明确的入口，以后正文要分段存（富文本 /
     * 待办勾选）时改这里就够，QML 不用动。
     */
    QString plainText() const { return m_text; }

    QColor color() const { return m_color; }
    void setColor(const QColor &color);

    /* 底色对应的前景色（浅纸给深字、深纸给浅字），QML 里直接绑 */
    QColor textColor() const;
    QColor mutedTextColor() const;
    /* 底色压暗一点：头部那条 / 链接条的底色，和便签纸拉开一点点层次 */
    QColor shadeColor() const;
    bool darkPaper() const;

    NoteLinkModel *linkModel() const { return m_links; }
    QObject *links() const;

    bool visible() const { return m_visible; }
    void setVisible(bool visible);

    qreal opacity() const { return m_opacity; }
    void setOpacity(qreal opacity);
    /* 百分比（菜单里显示 "100%" 那种） */
    int opacityPercent() const;
    /* 按某一档百分比设置（菜单里的 100 / 85 / 70 / 55 / 40），越界会夹住 */
    Q_INVOKABLE void setOpacityPercent(int percent);

    QRect geometry() const { return m_geometry; }
    void setGeometry(const QRect &rect);
    /* 拆开给 QML / 窗口用（QRect 在 QML 里改不了单项） */
    int x() const { return m_geometry.x(); }
    int y() const { return m_geometry.y(); }
    int width() const { return m_geometry.width(); }
    int height() const { return m_geometry.height(); }

    QString groupId() const { return m_groupId; }
    void setGroupId(const QString &id);

    /* 在不在某一摞里（界面按它决定菜单那一条是"组合成摞"还是"拆分组合"） */
    Q_INVOKABLE bool grouped() const { return !m_groupId.isEmpty(); }

    /* 0 = 没摆过（新建的便签按这个判断"要找你摆个位置"）；见 StickyNotes::create */
    bool hasGeometry() const { return !m_geometry.isNull(); }

    /* 正文里链接的条数（自检 / 界面提示用） */
    Q_INVOKABLE int linkCount() const;

    /* 在系统浏览器里打开正文里第 index 条链接；越界返回 false */
    Q_INVOKABLE bool openLink(int index) const;
    /* 直接开一个 URL（链接卡片上点了就是它） */
    Q_INVOKABLE bool openUrl(const QString &url) const;

    /* 把便签里的正文复制到系统剪贴板（头部那个"复制"按钮） */
    Q_INVOKABLE void copyTextToClipboard() const;

    /* ---- 落盘 ---- */
    QJsonObject toJson() const;
    static StickyNote *fromJson(const QJsonObject &obj, NoteThumbs *thumbs = nullptr,
                                QObject *parent = nullptr);

signals:
    void textChanged();
    void colorChanged();
    void visibleChanged();
    void opacityChanged();
    void geometryChanged();
    void groupIdChanged();

private:
    QString m_id;
    QString m_text;
    QColor m_color;
    QRect m_geometry;
    bool m_visible = true;
    /* 便签纸不透明度，默认完全不透明（见 Q_PROPERTY 的说明） */
    qreal m_opacity = 1.0;
    /* 所属组合（空串 = 单块），见 Q_PROPERTY 的说明 */
    QString m_groupId;
    NoteLinkModel *m_links = nullptr;
};

/*
 * 便签清单 + notes.json。
 *
 * 为什么不进剪贴板那个 SQLite 库：两边管的不是一回事 —— 剪贴板的正文是磁盘上的
 * md 文件（见 ClipboardStore），便签是"一小块随手记的文字 + 摆在哪儿 + 什么颜色"，
 * 整份就是一个几百字节的 JSON，读写直接来、坏了删掉重来就行，没必要为它建表。
 *
 * 落盘位置：QStandardPaths::AppDataLocation/notes.json
 * （Windows 上就是 %APPDATA%/SmartClip/SmartClip/notes.json —— 和
 * QSettings 里那些界面设置同一个程序数据目录，用户改剪贴板保存位置不会
 * 把便签一起搬走）。测试 / 自检可以用 setFilePath() 指到别处。
 *
 * 写盘是**防抖**的：编辑区里敲字、拖窗口都会调 save()，但真正落盘在
 * 500ms 之后（见 .cpp 的 flush / scheduleSave）。进程被杀最多丢最后半秒。
 */
class StickyNoteStore final : public QObject {
    Q_OBJECT

    /* 一共几条便签（状态栏 / 自检用） */
    Q_PROPERTY(int count READ count NOTIFY changed)
    /* 几条正摆在桌面上 */
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY changed)

public:
    explicit StickyNoteStore(QObject *parent = nullptr);
    ~StickyNoteStore() override;

    /* 默认落盘位置（AppDataLocation/notes.json），不存在就建目录 */
    static QString defaultFilePath();

    /*
     * 把 notes.json 读进来（文件不在 / 内容坏了都当成"还没有便签"，
     * 不报错也不清盘 —— 坏文件会被改名成 notes.json.bad 留在原地）。
     */
    bool load();
    /* 立刻落盘（不看防抖） */
    bool flush();
    /* 排一次落盘：500ms 内多次调用只写一次 */
    void scheduleSave();

    /* 文件路径（测试可以把整份数据指到临时目录） */
    QString filePath() const { return m_path; }
    void setFilePath(const QString &path) { m_path = path; }

    /*
     * 缩略图抓取器（见 NoteThumbs）。
     *
     * 不放在构造参数里，是因为它是 StickyNotes 建的、而 StickyNotes 又要用
     * 这个 store —— 构造顺序上只能稍后 setThumbs()。**必须**在 load() 之前调，
     * 否则从文件里恢复的那些便签拿不到它（链接卡片就没有缩略图了）。
     */
    void setThumbs(NoteThumbs *thumbs) { m_thumbs = thumbs; }

    /*
     * 新建一条便签。
     *
     * 生成 id、给默认底色、正文为空，**不**摆位置（geometry 空着，
     * 由 StickyNotes 找一块空桌面放它）；建完就排一次落盘。
     */
    StickyNote *create();

    /* 挂一条已经建好的便签进来（fromJson 那条路 / 测试用） */
    void append(StickyNote *note);

    QList<StickyNote *> notes() const { return m_notes; }
    int count() const { return int(m_notes.size()); }
    int visibleCount() const;

    /* 把一条便签从清单里摘掉（它自己 deleteLater） */
    void remove(StickyNote *note);

    /* 便签的正文 / 颜色 / 位置改了：转成一次落盘 */
    void touch();

    /*
     * 一摞便签里"哪一块在最上面"（露头那张纸）。
     *
     * 每条便签自己只记"我属于哪一摞"（StickyNote::groupId）—— 那足够恢复出
     * 一摞有哪几块；而**谁在最上面**是单独一件事（用户点过下面那几张纸、或者
     * 拖过一块上去就变了），所以按组 id 另记一份在这里，跟着 notes.json 一起
     * 落盘：
     *
     *     "groups": { "g1": { "active": "<便签 id>" } }
     *
     * 没记过的组、或者记的那块已经不在组里了，恢复时按便签在文件里的先后
     * （第一块在最上面）—— 老文件里没有这一段，也走这条路。
     */
    QString groupActiveId(const QString &groupId) const;
    void setGroupActiveId(const QString &groupId, const QString &noteId);

signals:
    void changed();

private:
    QList<StickyNote *> m_notes;
    QString m_path;
    NoteThumbs *m_thumbs = nullptr;
    /* 组 id -> 露头那块便签的 id（见 groupActiveId 的说明） */
    QHash<QString, QString> m_groupActive;
    /* 防抖用：真正落盘的定时器，见 scheduleSave */
    class QTimer *m_saveTimer = nullptr;
};
