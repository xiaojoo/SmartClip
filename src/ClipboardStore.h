#pragma once

#include <QObject>
#include <QVariantList>
#include <QSqlDatabase>

class QImage;

class ClipboardStore final : public QObject {
    Q_OBJECT
public:
    explicit ClipboardStore(QObject *parent = nullptr);
    bool open();
    bool addText(const QString &text);
    bool addImage(const QImage &image);
    Q_INVOKABLE QVariantList items(const QString &query = {}) const;
    Q_INVOKABLE void copyItem(qint64 id) const;
    bool takeSkipNextCapture();

signals:
    void changed();

private:
    QString dataDir() const;
    QString titleFor(const QString &text) const;
    QSqlDatabase m_db;
    mutable bool m_skipNextCapture = false;
};
