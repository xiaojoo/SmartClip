#include "StickyNoteStore.h"

#include "NoteLinkModel.h"
#include "NoteThumbs.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

namespace {

/* 落盘文件名（见 StickyNoteStore 开头的说明） */
constexpr char kNotesFile[] = "notes.json";
/* 编辑 / 拖动之后多久真正写盘（见 StickyNoteStore::scheduleSave） */
constexpr int kSaveDelayMs = 500;

/*
 * 便签纸的配色：**48 格**，8 列 × 6 行（面板按这个排）。
 *
 * 前四行是浅色系（经典便签黄 / 粉 / 绿 / 蓝 / 紫 一路铺开），后两行是深色纸 ——
 * 前后景对比度由 StickyNote::textColor() 按亮度自动配（见 .cpp 底部），
 * 所以深纸上写的是浅字，不用为深色单独一套样式。
 *
 * 第一格**必须是便签黄**：新建便签的默认底色就是它，自检也钉着这一条。
 *
 * 为什么从 8 个扩到 48 个：用户要的是取色框里那种"Basic colors"那么大一片色板
 * （原来 8 个色排成 6 列，第二行只两格，看着不像块色板）。
 */
const QStringList kPalette = {
    /* 第 1 行：便签黄领头的浅暖色 */
    QStringLiteral("#ffe9a8"), QStringLiteral("#fff3c4"), QStringLiteral("#ffe0b2"),
    QStringLiteral("#ffd3d8"), QStringLiteral("#f7c8e0"), QStringLiteral("#e5d4f7"),
    QStringLiteral("#d9d6f7"), QStringLiteral("#c9b6f7"),
    /* 第 2 行：浅绿 / 浅蓝 / 浅青 */
    QStringLiteral("#f0f7c4"), QStringLiteral("#ddf2b8"), QStringLiteral("#c9ebc0"),
    QStringLiteral("#b8ecd8"), QStringLiteral("#c0eef0"), QStringLiteral("#bfe0f5"),
    QStringLiteral("#c4d8f7"), QStringLiteral("#cfc4f7"),
    /* 第 3 行：中等饱和（暖） */
    QStringLiteral("#ffd166"), QStringLiteral("#ffbe5c"), QStringLiteral("#ffab91"),
    QStringLiteral("#ff9aa2"), QStringLiteral("#f48fb1"), QStringLiteral("#ce93d8"),
    QStringLiteral("#b39ddb"), QStringLiteral("#9fa8da"),
    /* 第 4 行：中等饱和（冷） */
    QStringLiteral("#b7d34a"), QStringLiteral("#a5d6a7"), QStringLiteral("#80cbc4"),
    QStringLiteral("#81d4fa"), QStringLiteral("#90caf9"), QStringLiteral("#a5b4fc"),
    QStringLiteral("#c4b5fd"), QStringLiteral("#e1bee7"),
    /* 第 5 行：深一档（含中性灰） */
    QStringLiteral("#6b5b2e"), QStringLiteral("#7a5c3e"), QStringLiteral("#8d6e63"),
    QStringLiteral("#a1887f"), QStringLiteral("#757575"), QStringLiteral("#616161"),
    QStringLiteral("#546e7a"), QStringLiteral("#4e6e5d"),
    /* 第 6 行：石墨 / 墨绿 / 深紫这些"深色纸" */
    QStringLiteral("#3b4048"), QStringLiteral("#33403a"), QStringLiteral("#3a3348"),
    QStringLiteral("#2f3b46"), QStringLiteral("#432b2b"), QStringLiteral("#2b2b2b"),
    QStringLiteral("#1f1f1f"), QStringLiteral("#101010"),
};

/* 带 alpha 的颜色写进 JSON：QColor::name() 会丢掉透明度，所以自己拼 */
QString colorToJson(const QColor &color) {
    return color.name(QColor::HexArgb);
}

QColor colorFromJson(const QVariant &value, const QColor &fallback) {
    const QColor color(value.toString());
    return color.isValid() ? color : fallback;
}

}  // namespace

/* ===========================================================================
 * StickyNote
 * ======================================================================== */

StickyNote::StickyNote(QObject *parent) : QObject(parent) {
    m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_color = QColor(kPalette.first());
    /* 链接模型挂在便签身上：便签没了它也没了（QML 那边只认便签这一个对象） */
    m_links = new NoteLinkModel(nullptr, this);
}

StickyNote::StickyNote(const QString &id, NoteThumbs *thumbs, QObject *parent)
    : QObject(parent) {
    m_id = id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
    m_color = QColor(kPalette.first());
    m_links = new NoteLinkModel(thumbs, this);
}

StickyNote::~StickyNote() = default;

const QStringList &StickyNote::palette() {
    return kPalette;
}

void StickyNote::setText(const QString &text) {
    if (m_text == text)
        return;
    m_text = text;
    if (m_links)
        m_links->setText(text);
    emit textChanged();
}

void StickyNote::setColor(const QColor &color) {
    if (!color.isValid() || m_color == color)
        return;
    m_color = color;
    emit colorChanged();
}

/*
 * 前景色：按便签纸的亮度选深字还是浅字。
 *
 * 不用 WCAG 的对比度公式，用感知亮度（0.299R + 0.587G + 0.114B）就够 ——
 * 阈值设在 150 附近：便签黄(≈226)、浅粉(≈218)、浅绿(≈222) 都走深字，
 * 石墨(≈64)、墨绿(≈61) 都走浅字。
 */
namespace {

int paperLuma(const QColor &color) {
    return qRound(0.299 * color.red() + 0.587 * color.green() + 0.114 * color.blue());
}

}  // namespace

bool StickyNote::darkPaper() const {
    return paperLuma(m_color) <= 150;
}

QColor StickyNote::textColor() const {
    return darkPaper() ? QColor(0xf2, 0xf3, 0xf5) : QColor(0x1e, 0x20, 0x24);
}

QColor StickyNote::mutedTextColor() const {
    QColor base = textColor();
    base.setAlpha(150);
    return base;
}

QColor StickyNote::shadeColor() const {
    /*
     * 头部那条和链接卡片的底色：把便签纸往"字色"的方向压一点。
     *
     * 深纸上压暗、浅纸上也压暗（往黑走）都行 —— 只要和纸本身差一点点层次
     * 就够了，压过头会像另一张纸。
     */
    const int delta = darkPaper() ? 26 : -22;
    return QColor(qBound(0, m_color.red() + delta, 255),
                  qBound(0, m_color.green() + delta, 255),
                  qBound(0, m_color.blue() + delta, 255));
}

QObject *StickyNote::links() const {
    return m_links;
}

void StickyNote::setVisible(bool visible) {
    if (m_visible == visible)
        return;
    m_visible = visible;
    emit visibleChanged();
}

void StickyNote::setOpacity(qreal opacity) {
    const qreal clamped = qBound(0.35, opacity, 1.0);
    if (qFuzzyCompare(m_opacity, clamped))
        return;
    m_opacity = clamped;
    emit opacityChanged();
}

void StickyNote::setGroupId(const QString &id) {
    if (m_groupId == id)
        return;
    m_groupId = id;
    emit groupIdChanged();
}

int StickyNote::opacityPercent() const {
    return qRound(m_opacity * 100.0);
}

void StickyNote::setOpacityPercent(int percent) {
    setOpacity(percent / 100.0);
}

void StickyNote::setGeometry(const QRect &rect) {
    if (m_geometry == rect)
        return;
    m_geometry = rect;
    emit geometryChanged();
}

int StickyNote::linkCount() const {
    return m_links ? m_links->rowCount() : 0;
}

bool StickyNote::openUrl(const QString &url) const {
    const QUrl target(url);
    if (!target.isValid() || target.scheme().isEmpty())
        return false;
    /* 只放 http/https 出去：正文里万一混进别的 scheme，不能凭它拉起别的程序 */
    if (target.scheme() != QLatin1String("http") && target.scheme() != QLatin1String("https"))
        return false;
    return QDesktopServices::openUrl(target);
}

bool StickyNote::openLink(int index) const {
    if (!m_links || index < 0 || index >= m_links->rowCount())
        return false;

    const QModelIndex modelIndex = m_links->index(index);
    const QString url = m_links->data(modelIndex, NoteLinkModel::UrlRole).toString();
    return openUrl(url);
}

void StickyNote::copyTextToClipboard() const {
    if (QClipboard *clip = QGuiApplication::clipboard())
        clip->setText(m_text);
}

QJsonObject StickyNote::toJson() const {
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), m_id);
    obj.insert(QStringLiteral("text"), m_text);
    obj.insert(QStringLiteral("color"), colorToJson(m_color));
    obj.insert(QStringLiteral("visible"), m_visible);
    obj.insert(QStringLiteral("opacity"), m_opacity);
    /*
     * 只在真属于某个组合时才写 groupId：空组 id 存进去在 JSON 里是个没意义的
     * 字段（也让"没有这一项的老文件"和"新文件里没组合的便签"长得一样）。
     */
    if (!m_groupId.isEmpty())
        obj.insert(QStringLiteral("groupId"), m_groupId);

    /* 没摆过位置的便签不写 geometry：下次启动还是"找块空桌面放" */
    if (!m_geometry.isNull()) {
        QJsonObject geo;
        geo.insert(QStringLiteral("x"), m_geometry.x());
        geo.insert(QStringLiteral("y"), m_geometry.y());
        geo.insert(QStringLiteral("w"), m_geometry.width());
        geo.insert(QStringLiteral("h"), m_geometry.height());
        obj.insert(QStringLiteral("geometry"), geo);
    }
    return obj;
}

StickyNote *StickyNote::fromJson(const QJsonObject &obj, NoteThumbs *thumbs, QObject *parent) {
    auto *note = new StickyNote(obj.value(QStringLiteral("id")).toString(), thumbs, parent);
    note->m_text = obj.value(QStringLiteral("text")).toString();
    note->m_color = colorFromJson(obj.value(QStringLiteral("color")), QColor(kPalette.first()));
    /* 没有 visible 字段的老文件：当"摆着" */
    note->m_visible = obj.value(QStringLiteral("visible")).toBool(true);
    /* 没有 opacity 字段的老文件：当"完全不透明" */
    note->m_opacity = qBound(0.35, obj.value(QStringLiteral("opacity")).toDouble(1.0), 1.0);
    /* 没有 groupId 字段的老文件：当"没组合"（单独一块便签） */
    note->m_groupId = obj.value(QStringLiteral("groupId")).toString();

    const QJsonObject geo = obj.value(QStringLiteral("geometry")).toObject();
    if (!geo.isEmpty()) {
        note->m_geometry = QRect(geo.value(QStringLiteral("x")).toInt(),
                                 geo.value(QStringLiteral("y")).toInt(),
                                 geo.value(QStringLiteral("w")).toInt(),
                                 geo.value(QStringLiteral("h")).toInt());
    }

    /* 正文里的链接要重新抽一遍（链接清单本身不落盘，它是正文推出来的） */
    if (note->m_links)
        note->m_links->setText(note->m_text);
    return note;
}

/* ===========================================================================
 * StickyNoteStore
 * ======================================================================== */

StickyNoteStore::StickyNoteStore(QObject *parent) : QObject(parent) {
    m_path = defaultFilePath();
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kSaveDelayMs);
    connect(m_saveTimer, &QTimer::timeout, this, [this]() { flush(); });
}

StickyNoteStore::~StickyNoteStore() {
    /* 退出前把最后一次改动落下去（编辑区里敲完字直接退出进程就是这种） */
    if (m_saveTimer && m_saveTimer->isActive())
        m_saveTimer->stop();
    flush();
}

QString StickyNoteStore::defaultFilePath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QLatin1Char('/') + QLatin1String(kNotesFile);
}

bool StickyNoteStore::load() {
    qDeleteAll(m_notes);
    m_notes.clear();
    m_groupActive.clear();

    QFile file(m_path);
    if (!file.exists()) {
        emit changed();
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        emit changed();
        return false;
    }
    const QByteArray raw = file.readAll();
    file.close();

    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        /*
         * 坏文件不当成"没有便签"直接覆盖：改名留一份，用户还能自己捞回来
         * （内容是纯文本的 JSON，正文就在里面）。
         */
        const QString backup = m_path + QStringLiteral(".bad");
        QFile::remove(backup);
        QFile::rename(m_path, backup);
        emit changed();
        return false;
    }

    const QJsonArray items = doc.object().value(QStringLiteral("notes")).toArray();
    for (const QJsonValue &value : items) {
        if (!value.isObject())
            continue;
        StickyNote *note = StickyNote::fromJson(value.toObject(), m_thumbs, nullptr);
        note->setParent(this);
        connect(note, &StickyNote::textChanged, this, &StickyNoteStore::touch);
        connect(note, &StickyNote::colorChanged, this, &StickyNoteStore::touch);
        connect(note, &StickyNote::geometryChanged, this, &StickyNoteStore::touch);
        connect(note, &StickyNote::visibleChanged, this, &StickyNoteStore::touch);
        m_notes.append(note);
    }

    /*
     * 一摞便签"哪一块在最上面"（见 groupActiveId 的说明）。老文件里没有这一段
     * —— 那就空着，恢复时按便签在文件里的先后（第一块在最上面）。
     */
    const QJsonObject groups = doc.object().value(QStringLiteral("groups")).toObject();
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        const QString active = it.value().toObject().value(QStringLiteral("active")).toString();
        if (!active.isEmpty())
            m_groupActive.insert(it.key(), active);
    }

    emit changed();
    return true;
}

bool StickyNoteStore::flush() {
    if (m_path.isEmpty())
        return false;

    if (m_saveTimer && m_saveTimer->isActive())
        m_saveTimer->stop();

    const QFileInfo info(m_path);
    if (!QDir().mkpath(info.absolutePath()))
        return false;

    QJsonArray items;
    for (const StickyNote *note : std::as_const(m_notes))
        items.append(note->toJson());

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("notes"), items);

    /*
     * 一摞便签"哪一块在最上面"（见 groupActiveId 的说明）。顺手把已经不存在
     * 的组清掉：那几块便签都被删 / 都散伙了，留着这段只会让文件越攒越脏。
     */
    QJsonObject groups;
    for (auto it = m_groupActive.constBegin(); it != m_groupActive.constEnd(); ++it) {
        bool stillThere = false;
        for (const StickyNote *note : std::as_const(m_notes)) {
            if (note && note->groupId() == it.key()) {
                stillThere = true;
                break;
            }
        }
        if (!stillThere)
            continue;
        QJsonObject entry;
        entry.insert(QStringLiteral("active"), it.value());
        groups.insert(it.key(), entry);
    }
    if (!groups.isEmpty())
        root.insert(QStringLiteral("groups"), groups);

    /*
     * 先写临时文件再 rename：直接截断重写的话，写到一半断电 / 被杀，
     * 用户的便签就剩半个 JSON（下次启动按坏文件处理，只能从 .bad 里捞）。
     */
    const QString temp = m_path + QStringLiteral(".tmp");
    QFile file(temp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();

    QFile::remove(m_path);
    return QFile::rename(temp, m_path);
}

void StickyNoteStore::scheduleSave() {
    if (m_saveTimer)
        m_saveTimer->start();
}

QString StickyNoteStore::groupActiveId(const QString &groupId) const {
    return m_groupActive.value(groupId);
}

void StickyNoteStore::setGroupActiveId(const QString &groupId, const QString &noteId) {
    if (groupId.isEmpty())
        return;
    if (m_groupActive.value(groupId) == noteId)
        return;
    if (noteId.isEmpty())
        m_groupActive.remove(groupId);
    else
        m_groupActive.insert(groupId, noteId);
    scheduleSave();
}

StickyNote *StickyNoteStore::create() {
    auto *note = new StickyNote(QString(), m_thumbs, this);
    connect(note, &StickyNote::textChanged, this, &StickyNoteStore::touch);
    connect(note, &StickyNote::colorChanged, this, &StickyNoteStore::touch);
    connect(note, &StickyNote::geometryChanged, this, &StickyNoteStore::touch);
    connect(note, &StickyNote::visibleChanged, this, &StickyNoteStore::touch);
    m_notes.append(note);
    emit changed();
    scheduleSave();
    return note;
}

void StickyNoteStore::append(StickyNote *note) {
    if (!note || m_notes.contains(note))
        return;
    note->setParent(this);
    connect(note, &StickyNote::textChanged, this, &StickyNoteStore::touch);
    connect(note, &StickyNote::colorChanged, this, &StickyNoteStore::touch);
    connect(note, &StickyNote::geometryChanged, this, &StickyNoteStore::touch);
    connect(note, &StickyNote::visibleChanged, this, &StickyNoteStore::touch);
    m_notes.append(note);
    emit changed();
}

int StickyNoteStore::visibleCount() const {
    int count = 0;
    for (const StickyNote *note : std::as_const(m_notes)) {
        if (note->visible())
            ++count;
    }
    return count;
}

void StickyNoteStore::remove(StickyNote *note) {
    if (!note || !m_notes.removeOne(note))
        return;
    note->deleteLater();
    emit changed();
    scheduleSave();
}

void StickyNoteStore::touch() {
    emit changed();
    scheduleSave();
}
