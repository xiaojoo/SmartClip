#include "ClipboardStore.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
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
        QVariantMap item;
        item["id"] = query.value(0);
        item["type"] = query.value(1);
        item["title"] = query.value(2);
        item["content"] = query.value(3);
        item["createdAt"] = query.value(4);
        result.append(item);
    }
    return result;
}

void ClipboardStore::copyItem(qint64 id) const {
    QSqlQuery query;
    query.prepare("SELECT type, content FROM clipboard_items WHERE id = ?");
    query.addBindValue(id);
    if (!query.exec() || !query.next()) return;
    m_skipNextCapture = true;
    if (query.value(0).toString() == "image") QGuiApplication::clipboard()->setImage(QImage(query.value(1).toString()));
    else QGuiApplication::clipboard()->setText(query.value(1).toString());
}

bool ClipboardStore::takeSkipNextCapture() {
    const bool skip = m_skipNextCapture;
    m_skipNextCapture = false;
    return skip;
}
