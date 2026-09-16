#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

/*
 * 代码格式化（编辑区右键菜单里那条"格式化代码"）。
 *
 * 做成 QML 单例 `Fmt`（见 src/main.cpp 的 qmlRegisterSingletonInstance），
 * 因为界面那侧要问三件事：
 *   * 这份文件**能不能**格式化（菜单条目置灰用）—— supported(language)；
 *   * 到底会用哪个工具（状态栏 / 提示里说清楚）—— engineLabel(language)；
 *   * 真去格式化一份正文 —— format(text, language, filePath)。
 *
 * 为什么必须有"外部命令"这条路：
 *   QScintilla 和 Qt 都**不带**任何格式化器（Scintilla 的 SCI_* 消息里没有
 *   "重排这份代码"这种东西），格式化只能靠本机装了的工具。所以这里的做法是
 *   "认几个常见工具 + 认命令行选项"，没有工具时**明说没有**，而不是自己
 *   写一个半吊子的缩进器蒙混过去 —— 那种东西对 C++ / Python 只会把代码改坏。
 *
 * 内置（不需要装任何东西）的三样：
 *   json  —— QJsonDocument 重排 + 4 空格缩进（最常用，也最安全）
 *   html  —— XML 良构时按标签重排缩进（QXmlStreamReader 走一遍）
 *   text  —— 去行尾空白 / 收敛多余空行 / 文件末尾补一个换行（任何语言都能用）
 *
 * 外部工具（按语言认一份默认命令，可在设置里改，见 toolFor）：
 *   cpp/csharp/java/javascript/typescript/json  ->  clang-format
 *   python                                      ->  black
 *   go                                          ->  gofmt
 *   rust                                        ->  rustfmt
 *   php                                         ->  php-cs-fixer
 *   ruby                                        ->  rubocop
 *
 * 调用约定统一成"把正文写到临时文件、把那个文件路径追加到命令行末尾、
 * 读回 stdout"——这几种工具都支持"就地改文件"或者"打一份到标准输出"，
 * 下面按各自的实际行为分开处理（见 kTools 里的 inPlace）。
 *
 * 为什么用临时文件而不是把正文从 stdin 喂进去、再从 stdout 读回来：
 *   DSH 那类受限环境里"给子进程开管道"会被拒（EPERM），而重定向到文件
 *   到处都能用；顺带一个好处是工具报错时能看到它到底在对哪份文件抱怨。
 */
class Formatter final : public QObject {
    Q_OBJECT

public:
    explicit Formatter(QObject *parent = nullptr);

    /* 是否有任何办法格式化这个语言（内置 / 外部工具两者之一） */
    Q_INVOKABLE bool supported(const QString &language, const QString &filePath = QString()) const;

    /*
     * 会用什么来格式化（"内置格式化" / "clang-format" / "black" …）。
     * 什么都没找到时返回空串 —— 界面据此拼那句"本机没装 XX"的提示。
     */
    Q_INVOKABLE QString engineLabel(const QString &language,
                                    const QString &filePath = QString()) const;

    /*
     * 格式化一份正文。
     *
     * 成功：返回格式化后的文字。
     * 失败：返回**原样**的输入，原因在 lastError() 里（"本机没装 clang-format" /
     *       "第 3 行：标签没有闭合"…），用的工具在 lastEngine() 里。
     *
     * 返回值永远可以直接写回编辑器：失败时就是原文，不会出现"格式化失败
     * 结果正文被清空"这种事。
     */
    Q_INVOKABLE QString format(const QString &text, const QString &language,
                               const QString &filePath = QString());

    /* 上一次格式化失败的原因（成功时是空串）；界面弹提示用 */
    Q_INVOKABLE QString lastError() const { return m_lastError; }
    /* 上一次用的是哪个工具（状态栏那句话用） */
    Q_INVOKABLE QString lastEngine() const { return m_lastEngine; }

    /*
     * 某个语言的自定义工具命令（设置里填的）。
     *
     * 留空 = 用内置那份默认（见 kTools）；填了就整条覆盖 —— 命令里可以用
     * {file} 占位（被换成临时文件路径），不写占位就自动追加在末尾。
     * 落盘在 QSettings 的 format/tool/<language> 下。
     */
    Q_INVOKABLE QString toolFor(const QString &language) const;
    Q_INVOKABLE void setToolFor(const QString &language, const QString &command);

    /*
     * 工具是不是真能起来（设置面板里"测试"那个按钮用）。
     * 只看第一段（程序名）能不能被找到 / 跑起来，不动用户的正文。
     */
    Q_INVOKABLE bool toolAvailable(const QString &language) const;

    /* 支持格式化的语言清单 [{id,label,engine,builtin}]，设置面板列出来用 */
    Q_INVOKABLE QVariantList toolList() const;

signals:
    /* 自定义工具改过了（设置面板刷新用） */
    void toolsChanged();

private:
    /*
     * 内置那三样：kind = "json" / "xml" / "text"。
     * 失败时**返回原文**并把原因写进 m_lastError（见 .cpp 里各分支的说明）。
     */
    QString formatBuiltin(const QString &text, const QString &kind) const;

    /*
     * 跑外部工具（调用约定见文件头）。失败时返回原文 + 原因。
     *
     * outEngine 收"实际用了哪个程序"（m_lastEngine 那份）；命令跑不起来时
     * 也仍然报得出程序名，用户才知道该去装什么。
     */
    QString formatExternal(const QString &text, const QString &command,
                           const QString &suffix, bool inPlace,
                           QString *outEngine) const;

    QString builtinKindFor(const QString &language) const;

    /* 上一次的失败原因 / 用的工具（format() 每次进来都会重写） */
    mutable QString m_lastError;
    mutable QString m_lastEngine;
};
