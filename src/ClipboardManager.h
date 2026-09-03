#pragma once

#include <QObject>

class ClipboardStore;

class ClipboardManager final : public QObject {
    Q_OBJECT
public:
    explicit ClipboardManager(ClipboardStore *store, QObject *parent = nullptr);
    void start();
private slots:
    void capture();
private:
    ClipboardStore *m_store;
};
