#include "ClipboardManager.h"
#include "ClipboardStore.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>

ClipboardManager::ClipboardManager(ClipboardStore *store, QObject *parent) : QObject(parent), m_store(store) {}

void ClipboardManager::start() {
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ClipboardManager::capture);
}

void ClipboardManager::capture() {
    if (m_store->takeSkipNextCapture()) return;
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (data->hasImage()) m_store->addImage(qvariant_cast<QImage>(data->imageData()));
    else if (data->hasText()) m_store->addText(data->text());
}
