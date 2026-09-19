/*
 * M1 spike —— 只回答一个问题：**GPU 呈现的 QML 层能不能挂在现在这个 QWidget 宿主里面**。
 *
 * 背景（都是这台机器上量出来的）：
 *   * SmartClip 现在是 QWidget 宿主 + QQuickWidget。QQuickWidget 把 QML 渲进 FBO 再
 *     **读回** raster backing store，最后 BitBlt 上屏 —— 4K 整窗一次 38~49ms，
 *     最大化那一下屏幕上就是 2~4 帧全黑（144fps 实测，见 build\new_01.png）。
 *   * 纯 QML（QQuickWindow / swap chain 呈现）的窗口在同样条件下最大化：**没有黑帧**
 *     （build\gp_02.png，那一帧已经是完整渲染好的最大化画面）。
 *   * 但正文是 QScintilla（QWidget），必须待在 QWidget 层级里 —— 这正是当年
 *     顶层从 QQuickWindow 换成 QWidget 的原因（src/EditorViewItem.cpp:416-418）。
 *
 * 所以 ③ 的可行与否取决于一件事：能不能把 QML 层换成**自己带 swap chain 的原生子窗**
 * （QQuickView + QWidget::createWindowContainer），和 QScintilla 那个原生子窗并列挂在
 * 同一个 QWidget 宿主下面。这个 spike 就是验它，四件事：
 *   1. 挂不挂得上（容器有没有真的变成宿主 HWND 的子窗口）；
 *   2. 最大化那一下还露不露黑（用 build\blackscan.ps1 数）；
 *   3. 鼠标 / 键盘进不进得去 QML 层和编辑层；
 *   4. 两层各自换尺寸时会不会互相盖住、留缝。
 *
 * 不进产品：CMake 里单独一个 exe 目标（gpu-spike），删掉那几行就没有了。
 * 跑法：build\run-gpu-spike.bat
 */
#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QQuickView>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QWidget>

/* QML 层的位置（宿主坐标）：左边一条"树" + 中间一大块"正文占位" */
static const int kLayerX = 0;
static const int kLayerY = 0;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("gpu-spike"));

    /* ---------------- 宿主：和 SmartClip 一样的无边框 + 不透明 + 深色 ---------------- */
    QWidget host;
    host.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    host.setWindowTitle(QStringLiteral("M1 GPU spike"));
    host.setStyleSheet(QStringLiteral("background:#313335;"));
    host.resize(1460, 900);
    host.move(1600, 816);
    host.show();

    /* ---------------- ① GPU 内容层：QQuickView 塞进 createWindowContainer ---------------- */
    QQuickView *qml = new QQuickView();
    qml->setColor(QColor(0x1e, 0x1f, 0x22));
    qml->setResizeMode(QQuickView::SizeRootObjectToView);
    const QString qmlPath = QStringLiteral("H:/steward/spike/gpu-layer.qml");
    if (!QFile::exists(qmlPath)) {
        qWarning() << "找不到 QML：" << qmlPath;
        return 2;
    }
    qml->setSource(QUrl::fromLocalFile(qmlPath));
    if (qml->status() != QQuickView::Ready) {
        qWarning() << "QML 加载失败：" << qml->status();
        return 3;
    }

    /* QQuickView 本身就是一块 QWindow —— 交给 createWindowContainer 塞进宿主 */
    QWidget *layer = QWidget::createWindowContainer(qml, &host);
    layer->setFocusPolicy(Qt::TabFocus);
    layer->setAutoFillBackground(false);
    layer->move(kLayerX, kLayerY);
    layer->resize(1460, 900);
    layer->show();

    /* ---------------- ② 编辑层：一个原生 QWidget 子窗（QScintilla 的替身） ---------------- */
    auto *edit = new QPlainTextEdit(&host);
    edit->setPlainText(QStringLiteral(
        "这一层是原生 QWidget 子窗（QScintilla 的替身）。\n"
        "它和上面那层 GPU 内容层是**并列的两块 HWND**。\n"
        "打字试试：键盘焦点进得来吗？"));
    edit->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background:#25262a;color:#d6d6d6;border:1px solid #4b4d4f;"
        "font:14px Consolas;}"));
    edit->setGeometry(300, 60, 1140, 800);
    edit->show();

    /* ---------------- ③ 每 3 秒自己最大化 / 还原，方便一次录像量到几个来回 ---------------- */
    auto *pump = new QTimer(&host);
    QObject::connect(pump, &QTimer::timeout, &host, [&host, layer, edit]() {
        const bool maxed = host.isMaximized();
        if (maxed) {
            host.showNormal();
            host.setGeometry(1600, 816, 1460, 900);
        } else {
            host.showMaximized();
        }
        layer->setGeometry(kLayerX, kLayerY, host.width(), host.height());
        edit->setGeometry(300, 60, qMax(200, host.width() - 320), qMax(200, host.height() - 120));
        qDebug() << "spike 循环：" << (maxed ? "还原" : "最大化") << host.width() << "x" << host.height()
                 << " 层" << layer->geometry() << " 编辑" << edit->geometry();
    });
    pump->setInterval(3000);
    pump->start();

    return app.exec();
}
