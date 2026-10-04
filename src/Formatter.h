#pragma once

#include "CodeStyle.h"

#include <QHash>
#include <QObject>
#include <QPair>
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
 * ===========================================================================
 * 格式模版（观感由我们定，不由工具默认定）
 * ===========================================================================
 * 每种语言有一份模版（src/CodeStyle.h，IDEA 的 code style 那套取值：缩进 4 /
 * 续行 8 / 行宽 120 / 大括号同行 / 连续空行≤2 …），格式化时把模版翻译成那个
 * 工具能吃的参数再跑：clang-format 吃 --style={…}（全项落得下），prettier 只吃
 * --tab-width / --print-width，black 只吃 --line-length，shfmt 只吃 -i，
 * gofmt / rustfmt 压根没有风格开关 —— 所以界面那行小字会写清楚"这一份模版
 * 在当前这个工具身上能落地几项"，不假装全都生效。
 *
 * 为什么必须我们自己给参数而不是让工具读项目里的配置：正文是先写到临时目录再
 * 交给工具的（见 formatExternal），从那儿往上找**找不到**用户项目里的
 * .clang-format / .prettierrc —— 所以不给参数就等于用工具自带的默认观感。
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
    /*
     * 工具在不在。filePath 传了的话会**多查一层这份文件所在项目**：
     * prettier / black 通常是 npm i -D / pip install 装进某个项目的 node_modules、
     * .venv 里，全局 PATH 上根本没有（这台机实测：某个项目的
     * node_modules/.bin/prettier 能跑 3.8.1，而 npm 全局 bin 是空的）。
     * 不传就是问"这台机器上有没有"。
     */
    Q_INVOKABLE bool toolAvailable(const QString &language,
                                   const QString &filePath = QString()) const;

    /*
     * 这个语言现在生效的格式模版（见 src/CodeStyle.h），设置面板那行小字用。
     *
     * styleFor 是原样的一份 map（改文件之前它等于内置默认 + 文件里覆盖的那几项）；
     * styleSummary 是给人读的那一行：观感数字 + "当前这个工具能吃掉其中哪几项"；
     * styleError 是模版文件读坏了要说的话（整份没生效 / 某一条没生效），空串 = 没事。
     */
    Q_INVOKABLE QVariantMap styleFor(const QString &language) const;
    Q_INVOKABLE QString styleSummary(const QString &language,
                                     const QString &filePath = QString()) const;
    Q_INVOKABLE QString styleError(const QString &language) const;

    /*
     * 模版文件所在目录（每种语言一个 <语言>.json，改完立刻生效）。
     * 正常就是 `<安装目录>/styles`；那一档不可写时退回 %APPDATA%/SmartClip/SmartClip/styles
     * （见 src/CodeStyle.h 里那段说明）。设置面板那个"打开模版目录"按的就是这条路。
     */
    Q_INVOKABLE QString stylesDir() const;

    /* 上面那个目录现在能不能写（面板那句"改完立刻生效"要配真话） */
    Q_INVOKABLE bool stylesWritable() const;

    /*
     * 支持格式化的语言清单 [{id,label,tool,defaultCommand,command,available,source,
     * style,styleError}]，设置面板列出来用。filePath 是给上面那个 toolAvailable 用的：
     * 面板要把"正在编辑的这份文件"算进去，不然项目里装了 prettier 也照样显示没装。
     */
    Q_INVOKABLE QVariantList toolList(const QString &filePath = QString()) const;

    /*
     * 那条命令 + 当前语言的模版 -> 真正要执行的参数表（实现见 .cpp 里的说明）。
     *
     * 返回的是**参数表**而不是命令行字符串：clang-format 的 --style={…} 里有空格，
     * 拼回字符串再 splitCommand 会把这一个参数炸成十几段（实测会把 clang-format
     * 直接顶回去报 "Error parsing -style"）。命令里已经写了同名开关的那一项不追加 ——
     * 用户自己填的半句优先。
     *
     * 之所以摆在 public：自检要能在**不真跑工具**的前提下直接量这一条。
     */
    QStringList withStyleArgs(const QString &command, const fmtstyle::Style &style) const;

signals:
    /* 自定义工具改过了（设置面板刷新用） */
    void toolsChanged();

private:
    /*
     * 内置那三样：kind = "json" / "xml" / "text"。
     * style 是当前语言的格式模版（缩进宽度、空行上限、末尾换行都从它取）。
     * 失败时**返回原文**并把原因写进 m_lastError（见 .cpp 里各分支的说明）。
     */
    QString formatBuiltin(const QString &text, const QString &kind,
                          const fmtstyle::Style &style) const;

    /*
     * 跑外部工具（调用约定见文件头）。失败时返回原文 + 原因。
     *
     * argv 是**已经拆好的**命令行（第一段是程序，{file} 占位在这里换成临时文件）；
     * outEngine 收"实际用了哪个程序"（m_lastEngine 那份）；命令跑不起来时
     * 也仍然报得出程序名，用户才知道该去装什么。
     */
    QString formatExternal(const QString &text, const QStringList &argv,
                           const QString &suffix, bool inPlace,
                           const fmtstyle::Style &style, const QString &filePath,
                           QString *outEngine) const;

    QString builtinKindFor(const QString &language) const;

    /*
     * 命令行里第一段程序在不在。
     *
     * 查五档：PATH → 这份文件所在项目的 node_modules / venv → 用户级 bin
     * （~\go\bin、~\.cargo\bin、%APPDATA%\npm、pip 的 Scripts 这些没进 PATH 的地方）
     * → 已知落点（VS 的 Llvm、Qt Creator 自带的 clang、官方 LLVM 目录）→ npx 缓存。
     * filePath 空着就是只问机器（第二档没法问，因为不知道是哪份文件）。
     * resolvedProgram 给绝对路径（空 = 找不到），programFound 是它的非空判断，
     * programOrigin 说这个路径是从哪档来的（machine / project / npx）。
     *
     * 带缓存，理由见 .cpp 里那段说明：这条路会被设置面板和右键菜单反复调，
     * 一次 toolList() 要查十几个条目，而每查一个"没装"的都要扫整条 PATH。
     */
    QString resolvedProgram(const QString &command, const QString &filePath) const;
    bool programFound(const QString &command, const QString &filePath = QString()) const;
    QString programOrigin(const QString &command, const QString &filePath) const;

    /*
     * resolvedProgram 的缓存：key = 程序名 + 文件所在目录（带路径的命令不进这里），
     * value = (解析出来的绝对路径, 查询时刻的毫秒数)。空路径 = 查过，没有。
     */
    mutable QHash<QString, QPair<QString, qint64> > m_foundCache;

    /* 上一次的失败原因 / 用的工具（format() 每次进来都会重写） */
    mutable QString m_lastError;
    mutable QString m_lastEngine;
};
