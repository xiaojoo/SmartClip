#include "ClipboardStore.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

ClipboardStore::ClipboardStore(QObject *parent) : QObject(parent) {}

QString ClipboardStore::dataDir() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

bool ClipboardStore::open() {
    QDir().mkpath(dataDir() + "/images");
    m_db = QSqlDatabase::addDatabase("QSQLITE");
    m_db.setDatabaseName(dataDir() + "/smartclip.db");
    if (!m_db.open()) return false;
    QSqlQuery query;
    return query.exec("CREATE TABLE IF NOT EXISTS clipboard_items ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, type TEXT NOT NULL, title TEXT NOT NULL, "
                      "content TEXT NOT NULL, created_at TEXT NOT NULL, hash TEXT UNIQUE)");
}

QString ClipboardStore::titleFor(const QString &text) const {
    QString title = text.simplified();
    return title.left(80).isEmpty() ? tr("Untitled") : title.left(80);
}

bool ClipboardStore::addText(const QString &text) {
    const QString clean = text.trimmed();
    if (clean.isEmpty()) return false;
    QSqlQuery query;
    query.prepare("INSERT OR IGNORE INTO clipboard_items(type, title, content, created_at, hash) VALUES ('text', ?, ?, ?, ?)");
    query.addBindValue(titleFor(clean));
    query.addBindValue(clean);
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    query.addBindValue(QString::number(qHash(clean)));
    if (!query.exec() || query.numRowsAffected() == 0) return false;
    emit changed();
    return true;
}

bool ClipboardStore::addImage(const QImage &image) {
    if (image.isNull()) return false;
    const QString relative = "images/" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmsszzz") + ".png";
    const QString path = dataDir() + "/" + relative;
    if (!image.save(path, "PNG")) return false;
    QSqlQuery query;
    query.prepare("INSERT INTO clipboard_items(type, title, content, created_at) VALUES ('image', ?, ?, ?)");
    query.addBindValue(tr("Image %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm")));
    query.addBindValue(path);
    query.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    if (!query.exec()) return false;
    emit changed();
    return true;
}

QString ClipboardStore::previewFor(const QString &text) {
    /*
     * 列表摘要：压成单行 + 限长。
     *
     * 顺序很讲究：先截断再 simplified()。
     * 反过来的话，一条 66 万字符的正文要先被 simplified() 全扫一遍，
     * 虽然只是几十毫秒，但这是每一条都要付的成本。
     * 先截到 400 字符再压，成本就与正文长度无关了。
     */
    constexpr int kProbe = 400;
    constexpr int kPreview = 120;

    QString head = text.left(kProbe).simplified();
    if (head.size() > kPreview)
        return head.left(kPreview) + QStringLiteral("…");
    return head;
}

QVariantList ClipboardStore::items(const QString &queryText) const {
    QVariantList result;
    QSqlQuery query;
    query.prepare("SELECT id, type, title, content, created_at FROM clipboard_items "
                  "WHERE title LIKE ? OR content LIKE ? ORDER BY id DESC LIMIT 200");
    const QString pattern = '%' + queryText + '%';
    query.addBindValue(pattern);
    query.addBindValue(pattern);
    if (!query.exec()) return result;
    while (query.next()) {
        const QString type = query.value(1).toString();
        const QString raw = query.value(3).toString();

        QVariantMap item;
        item["id"] = query.value(0);
        item["type"] = type;
        item["title"] = query.value(2);
        /*
         * 只有文本条目给摘要；图片条目的 content 是文件路径，本来就很短。
         * 正文本身一律不放进 QML —— 见 previewFor() 的说明。
         */
        item["content"] = (type == QStringLiteral("text")) ? previewFor(raw) : raw;
        item["contentLength"] = raw.size();
        item["createdAt"] = query.value(4);
        result.append(item);
    }
    return result;
}

QString ClipboardStore::contentOf(qint64 id) const {
    QSqlQuery query;
    query.prepare("SELECT content FROM clipboard_items WHERE id = ?");
    query.addBindValue(id);
    if (!query.exec() || !query.next()) return QString();
    return query.value(0).toString();
}

void ClipboardStore::copyItem(qint64 id) const {
    QSqlQuery query;
    query.prepare("SELECT type, content FROM clipboard_items WHERE id = ?");
    query.addBindValue(id);
    if (!query.exec() || !query.next()) return;

    const QString type = query.value(0).toString();
    const QString content = query.value(1).toString();

    /*
     * 超大内容不往系统剪贴板里塞。
     *
     * 用户点左侧列表是"看一眼"，不是"要复制"（真要复制有右键菜单的
     * 复制全文/复制此行）。库里有一条 668K 字符的条目，
     * 每次点开都把它写进系统剪贴板，那一趟是纯开销。
     * 阈值取得比常见剪贴板内容大得多，正常复制完全不受影响。
     */
    constexpr int kAutoCopyLimit = 256 * 1024;
    if (content.size() > kAutoCopyLimit && type != QStringLiteral("image"))
        return;

    m_skipNextCapture = true;
    if (type == QStringLiteral("image")) QGuiApplication::clipboard()->setImage(QImage(content));
    else QGuiApplication::clipboard()->setText(content);
}

void ClipboardStore::copyText(const QString &text) const {
    if (text.isEmpty()) return;
    m_skipNextCapture = true;
    QGuiApplication::clipboard()->setText(text);
}

bool ClipboardStore::takeSkipNextCapture() {
    const bool skip = m_skipNextCapture;
    m_skipNextCapture = false;
    return skip;
}
