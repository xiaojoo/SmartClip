#include "DocImport.h"

#include "ClipboardStore.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QThreadPool>

/*
 * 设置键（都挂在 doc/ 下，和别的界面设置同一个 QSettings 目录）
 */
namespace {
constexpr const char *kKeyBase = "doc/";
}

DocImport::DocImport(ClipboardStore *store, QObject *parent)
    : QObject(parent), m_store(store) {
    QSettings settings;
    auto read = [&settings](const char *key) {
        return settings.value(QString::fromLatin1(kKeyBase) + QLatin1String(key));
    };

    /*
     * 默认那条命令：python + 随包脚本（RapidDoc）。
     *
     * 默认填好而不是留空 —— 留空的话设置面板里是一行空白，用户不知道"这儿该
     * 填什么、要装什么"。填好之后他能看见那条命令，也知道要去 pip install。
     * （和 LlmClient 里 pinOcrRunner 的处理一致。）
     */
    m_pythonPath = read("pythonPath").toString();
    const QString savedRunner = read("runner").toString();
    if (savedRunner.trimmed().isEmpty())
        /* 没存过：用"自动找 python"那条当默认 */
        m_runner = DocConvert::defaultRunnerCommand(QString(), m_pythonPath);
    else
        m_runner = savedRunner;

    /*
     * 存过的那条命令里如果还指着"当时自动找到的那个 python"，而用户后来把
     * 解释器改到别处了，就把命令里那一段换掉。
     *
     * 为什么只换"自动找到的那一个"：用户手写过的命令（哪怕手写的就是
     * "python xxx.py"）不能动 —— 那是他自己的选择，改了就是擅自改配置。
     * 判据是"和我们不带 pythonPath 时的默认完全一致"，一致才说明它没被改过。
     */
    if (!savedRunner.trimmed().isEmpty() && !m_pythonPath.trimmed().isEmpty()
        && savedRunner == DocConvert::defaultRunnerCommand(QString(), QString())) {
        m_runner = DocConvert::defaultRunnerCommand(QString(), m_pythonPath);
    }

    m_tier = read("tier").toString();
    if (m_tier.trimmed().isEmpty())
        m_tier = QStringLiteral("balanced");

    m_openWhenDone = read("openWhenDone").isValid() ? read("openWhenDone").toBool() : true;
    m_keepSource = read("keepSource").isValid() ? read("keepSource").toBool() : false;

    /*
     * 自检里主窗口是**故意不显示**的（见 main.cpp 那几个自检分支），而"主窗口
     * 不可见"在正常使用中等于"收进托盘了" —— 卡片会被那条守卫立刻收掉，几何
     * 一条都量不到。所以自检跑起来时把 windowUsable 钉成 true。
     *
     * 用环境变量而不是给自检加一个参数：这条自检是从 main.cpp 的 QTimer 里起的，
     * 那边拿不到"是不是自检"以外的信息，而环境变量是现成的、不必再穿一层。
     */
    if (qEnvironmentVariableIsSet("SMARTCLIP_DOC_SELFTEST"))
        m_windowUsable = true;
}

QString DocImport::runner() const { return m_runner; }
void DocImport::setRunner(const QString &value) {
    const QString clean = value.trimmed();
    if (m_runner == clean)
        return;
    m_runner = clean;
    persist(QStringLiteral("runner"), clean);
    emit settingsChanged();
}

QString DocImport::tier() const { return m_tier; }
void DocImport::setTier(const QString &value) {
    /*
     * 只认这三个词。
     *
     * 空串会落到脚本自己的默认档 —— 但这里不让它变空：设置界面上是三个按钮，
     * 总得有一个是亮的（和 pinOcrEngine 那边一样，非法值一律折回一个合法值）。
     */
    const QString clean = (value == QLatin1String("fast") || value == QLatin1String("best"))
                              ? value
                              : QStringLiteral("balanced");
    if (m_tier == clean)
        return;
    m_tier = clean;
    persist(QStringLiteral("tier"), clean);
    emit settingsChanged();
}

QString DocImport::pythonPath() const { return m_pythonPath; }
void DocImport::setPythonPath(const QString &value) {
    const QString clean = value.trimmed();
    if (m_pythonPath == clean)
        return;
    m_pythonPath = clean;
    persist(QStringLiteral("pythonPath"), clean);

    /*
     * 把"还没被手改过"的默认命令重新拼一遍 —— 不然用户选了解释器，那条命令里
     * 还指着老的那个 python，改了跟没改一样（这个坑真踩过：依赖装在 venv 里，
     * 命令找的是系统 python，报 "No module named 'rapid_doc'"）。
     *
     * 判据和构造里那条一样：命令等于"不带 pythonPath 的默认"才认为它没被动过。
     */
    if (!clean.isEmpty()
        && m_runner == DocConvert::defaultRunnerCommand(QString(), QString())) {
        m_runner = DocConvert::defaultRunnerCommand(QString(), clean);
        persist(QStringLiteral("runner"), m_runner);
    }
    emit settingsChanged();
}

bool DocImport::openWhenDone() const { return m_openWhenDone; }

void DocImport::setOpenWhenDone(bool on) {
    if (m_openWhenDone == on)
        return;
    m_openWhenDone = on;
    persist(QStringLiteral("openWhenDone"), on);
    emit settingsChanged();
}

bool DocImport::keepSource() const { return m_keepSource; }
void DocImport::setKeepSource(bool on) {
    if (m_keepSource == on)
        return;
    m_keepSource = on;
    persist(QStringLiteral("keepSource"), on);
    emit settingsChanged();
}

void DocImport::persist(const QString &key, const QVariant &value) {
    QSettings().setValue(QString::fromLatin1(kKeyBase) + key, value);
}

QString DocImport::runnerProblem() const {
    return DocConvert::runnerProblem(m_runner);
}

QString DocImport::problemFor(const QString &path) const {
    return DocConvert::unsupportedReason(path);
}

QString DocImport::fileFilter() const {
    return DocConvert::fileFilter();
}

QString DocImport::commandForScript(const QString &script) const {
    const QString name = script.trimmed();
    if (name.isEmpty())
        return DocConvert::defaultRunnerCommand(m_tier, m_pythonPath);

    const QString path = DocConvert::runnerScriptPath(name);
    if (path.isEmpty())
        return QString();

    /*
     * 拼法照抄 DocConvert::defaultRunnerCommand（同一套引号 / 解释器选择的规矩），
     * 只是脚本路径换成传进来的这个。为什么不直接把那个函数的脚本名参数化：
     * 它固定用 rapid 那个当默认命令，是别处（默认设置）依赖的行为。
     */
    auto quote = [](const QString &value) {
        return QLatin1Char('"') + value + QLatin1Char('"');
    };
    if (!m_pythonPath.trimmed().isEmpty())
        return quote(m_pythonPath.trimmed()) + QLatin1Char(' ') + quote(path);

    QString exe = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (!exe.isEmpty())
        return quote(exe) + QLatin1Char(' ') + quote(path);
    exe = QStandardPaths::findExecutable(QStringLiteral("py"));
    if (!exe.isEmpty())
        return quote(exe) + QStringLiteral(" -3 ") + quote(path);
    return QStringLiteral("python ") + quote(path);
}

QStringList DocImport::shippedScripts() const {
    return DocConvert::shippedScripts();
}

QString DocImport::kindFor(const QString &path) const {
    return DocConvert::kindFor(path);
}

void DocImport::setWindowUsable(bool on) {
    if (m_windowUsable == on)
        return;
    m_windowUsable = on;
    emit windowUsableChanged();
}

QPoint DocImport::expectedCardPos(int hostX, int hostY, int hostW, int hostH,
                                  int cardW, int cardH) const {    /*
     * 和 DocCard.qml 的 reposition() 同一个算式（那边是"唯一真相"，这里是为了
     * 自检能在 C++ 参照系里比一次）。两处都改才算改对 —— 改了那边忘了这边，
     * 自检会红，正是要的效果。
     */
    return QPoint(hostX + hostW - cardW - cardMargin(),
                  hostY + hostH - cardH - cardBottomGap());
}

void DocImport::publishCardGeometry(int x, int y) {
    m_lastCardPos = QPoint(x, y);
    emit cardGeometryChanged(x, y);
}

void DocImport::clearResult() {
    if (m_created.isEmpty() && m_error.isEmpty())
        return;
    m_created.clear();
    m_error.clear();
    emit createdChanged();
    emit stateChanged();
}

void DocImport::openLast() {
    if (m_created.isEmpty())
        return;
    emit openRequested(m_created.constLast());
}

void DocImport::setStatus(const QString &text) {
    if (m_status == text)
        return;
    m_status = text;
    emit stateChanged();
}

void DocImport::enqueue(const QStringList &paths) {
    if (paths.isEmpty())
        return;

    /*
     * 收进来的同时就挑一遍：
     *   * 目录直接跳过（用户拖了一个文件夹进来，这里不递归 —— 一拖一个几万文件
     *     的目录就开始跑，那不是他要的）；
     *   * 认不了的格式不入队，但**记下来**，入队完统一报一句人话。
     *
     * 不在这儿直接弹错误框：一次拖五个文件、三个不认，弹三次框很烦。
     */
    QStringList rejected;
    QStringList accepted;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            rejected << info.fileName();
            continue;
        }
        const QString problem = DocConvert::unsupportedReason(path);
        if (!problem.isEmpty()) {
            rejected << info.fileName();
            continue;
        }
        accepted << info.absoluteFilePath();
    }

    if (!rejected.isEmpty()) {
        const int shown = qMin(3, int(rejected.size()));
        QStringList names = rejected.mid(0, shown);
        QString text = QStringLiteral("有 %1 个文件认不了：%2")
                           .arg(rejected.size())
                           .arg(names.join(QStringLiteral("、")));
        if (rejected.size() > shown)
            text += QStringLiteral("…");
        m_error = text;
        emit stateChanged();
    }

    if (accepted.isEmpty()) {
        if (m_queue.isEmpty())
            setStatus(QStringLiteral("没有可以识别的文件"));
        return;
    }

    /*
     * 命令不可用时直接报，不排队：排了也是每个文件跑一遍同样的报错，
     * 而"python 没装"这件事跟文件无关。
     */
    const QString runnerIssue = runnerProblem();
    if (!runnerIssue.isEmpty()) {
        m_error = runnerIssue;
        setStatus(QStringLiteral("识别程序没配好"));
        emit stateChanged();
        return;
    }

    m_queue += accepted;
    /* total 是"这一轮一共几个"：已经在跑的时候再拖文件进来，加上去 */
    m_total = m_done + m_queue.size();
    m_error.clear();
    emit stateChanged();

    if (!m_task)
        pump();
}

void DocImport::pump() {
    if (m_queue.isEmpty()) {
        const int succeeded = m_created.size();
        const int failed = m_done - succeeded;
        m_running = false;
        m_total = 0;
        m_done = 0;
        setStatus(succeeded > 0
                      ? QStringLiteral("识别完成：%1 份笔记").arg(succeeded)
                      : QStringLiteral("识别完成"));
        emit stateChanged();
        emit finished(succeeded, failed);
        return;
    }

    m_current = m_queue.takeFirst();
    m_running = true;
    setStatus(QStringLiteral("正在识别 %1/%2：%3")
                  .arg(m_done + 1)
                  .arg(m_total)
                  .arg(QFileInfo(m_current).fileName()));

    /*
     * 一条命令行 = 命令 + 档位词。档位接在**最后**（脚本读倒数第三个参数，
     * 见 doc_runner_common.py 的 read_args）—— 路径是脚本运行时才接上去的，
     * 所以这里拼出来的顺序正好对。
     */
    QString command = m_runner.trimmed();
    if (!m_tier.isEmpty())
        command += QLatin1Char(' ') + m_tier;

    /*
     * 跑在线程池里。
     *
     * Task 是 QRunnable（setAutoDelete(true)），跑完由**线程池**删 —— 所以这里
     * 不能自己 delete，也不能 deleteLater（那是给 QObject 生命周期的，两个一起
     * 用就是双重释放）。m_task 是个 QPointer，只用来"取消时找得到它"，任务被删
     * 之后它自己会变空。
     *
     * 连接用 QueuedConnection：finished 是在池线程里 emit 的，而这个对象在主
     * 线程 —— 队列化之后回调跑在主线程上，落盘 / 发信号都不用自己加锁。
     */
    auto *task = new DocConvert::Task(m_current, command, 1800000);
    m_task = task;
    connect(task, &DocConvert::Task::finished, this, &DocImport::onTaskFinished,
            Qt::QueuedConnection);
    QThreadPool::globalInstance()->start(task);
}

void DocImport::onTaskFinished(const DocConvert::Result &result) {
    /*
     * 任务对象这时候已经被线程池删了（setAutoDelete），所以**不能**去碰
     * sender()；m_task 这个 QPointer 也早就自己变空了，这里只是显式清一下。
     */
    m_task = nullptr;
    m_running = false;

    m_done += 1;

    if (result.ok()) {
        QStringList missing;
        if (m_store) {
            const QString path = m_store->createNote(
                result.title.isEmpty() ? DocConvert::titleFor(m_current) : result.title,
                result.markdown, result.images);
            if (!path.isEmpty())
                m_created << path;
            else
                missing << QStringLiteral("笔记写不进去");
        } else {
            missing << QStringLiteral("没有内容库");
        }
        if (!missing.isEmpty())
            m_error = QStringLiteral("%1：%2")
                          .arg(QFileInfo(m_current).fileName(), missing.join(QStringLiteral("、")));
        if (!m_created.isEmpty())
            emit createdChanged();
    } else {
        /*
         * 被取消不算"失败"，状态那行已经写着"已取消"了。
         * 判据用 DocConvert 那一份（词只在那儿写一次，见 cancelledError）——
         * 原来这里比的是 QLatin1String("已取消")，中文走 Latin-1 永不相等。
         */
        if (!DocConvert::isCancelledError(result.error)) {
            m_error = QStringLiteral("%1：%2")
                          .arg(QFileInfo(m_current).fileName(), result.error);
        }
    }

    m_current.clear();
    emit stateChanged();
    pump();
}

void DocImport::cancel() {
    const bool hadWork = m_task || !m_queue.isEmpty();
    if (m_task)
        m_task->cancel();
    m_queue.clear();
    m_created.clear();
    m_error.clear();
    emit createdChanged();

    if (!hadWork) {
        setStatus(QStringLiteral("没有在识别的文件"));
        return;
    }

    /*
     * total / done 只在**没有任务在跑**的时候清。
     *
     * 有任务在跑时不能清：那个任务回来会走 onTaskFinished，里面要拿 m_done
     * 报"这一轮认了几个"。清了它，回调里那个计数就从头开始，报出来是错的。
     */
    if (!m_task) {
        m_total = 0;
        m_done = 0;
        m_running = false;
        m_current.clear();
    }
    setStatus(QStringLiteral("已取消"));
}

void DocImport::shutdown() {
    if (!m_task)
        return;
    m_task->cancel();
    /*
     * 给 3 秒让子进程被 kill 掉、run() 从等待里出来。
     *
     * **不等它跑完**：脚本可能要几分钟，退出时挂在那儿不能接受。3 秒是"够
     * kill 一个进程"的量级，不是"够跑完一份 PDF"。
     *
     * 这里等的是整个全局线程池（Qt 没给"等某一个任务"的接口），所以贴图那条
     * 路的 OcrTask 如果正在跑也会被一起等 —— 那个是秒级的，无所谓。
     */
    QThreadPool::globalInstance()->waitForDone(3000);
    m_queue.clear();
}
