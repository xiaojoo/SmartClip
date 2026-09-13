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
    /*
     * 第一件事就是问"这次变化是不是我们自己弄出来的"。
     *
     * 编辑器里 Ctrl+C、菜单里的"复制全文"、点左树回填剪贴板……都会走到这里。
     * 不挡掉的话，用户复制出来的东西立刻又被采集一遍 —— 正是要避免的那件事
     * （见 ClipboardStore::markOwnCopy 的说明）。
     */
    if (m_store->takeSkipNextCapture())
        return;

    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (!data)
        return;

    /* 内容由 ClipboardStore 落成文件：文本进当天的 md，图片进 assets/ */
    if (data->hasImage())
        m_store->captureImage(qvariant_cast<QImage>(data->imageData()));
    else if (data->hasText())
        m_store->captureText(data->text());
}
