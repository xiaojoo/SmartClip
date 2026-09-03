#include "ClipboardManager.h"
#include "ClipboardStore.h"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSystemTrayIcon>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setOrganizationName("SmartClip");
    app.setApplicationName("SmartClip");

    ClipboardStore store;
    if (!store.open()) return 1;
    ClipboardManager clipboard(&store);
    clipboard.start();

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("clipboardStore", &store);
    engine.loadFromModule("SmartClip", "Main");
    if (engine.rootObjects().isEmpty()) return 1;

    QSystemTrayIcon tray(QIcon::fromTheme("edit-paste"), &app);
    tray.setToolTip("SmartClip");
    tray.show();
    return app.exec();
}
