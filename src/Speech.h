#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

/*
 * 朗读（QML 单例 Speech，见 src/main.cpp）。
 *
 * 翻译卡片上那个「播放」按钮就是它：把译文念出来。**不是模型念的** ——
 * 大模型只吐文字，没有音频输出（deepseek 这条 API 也一样）；念这一下走的是
 * 系统自带的语音合成（Windows SAPI5，见 Speech.cpp），和 Windows 的"讲述人"
 * 是同一套引擎。
 *
 * 为什么不用 Qt 自己的 QtTextToSpeech：这台机器上装的 Qt 6.11.2 里没有编那个
 * 模块。它是 Qt 的一个 **addon 包**（在线仓库里叫 `qt.qt6.6112.addons.qtspeech`，
 * "Qt Speech"，win64_msvc2022_64 那份 0.45 MB），用 Qt 的 MaintenanceTool
 * 勾一下就能补上 —— 但它**不是**默认装的，所以真用起来 = 换台机器 / 给别人编
 * 都要先装这个包，而 SAPI 是系统组件，装了系统就有，中文音色也是现成的
 * （这台机器上：Huihui / Kangkang / Yaoyao）。
 *
 * 2026-09 的取舍：**不装 qtspeech，就用 SAPI**（零依赖，自检 70 项全绿）。
 * 哪天想换过去，两个附带条件先记在这儿：
 *   * Windows 上 Qt 默认走的是 **winrt** 引擎（Windows.Media.SpeechSynthesis），
 *     不是 SAPI —— 它音质更好、拿的是系统里**全部**音色，但实现上要
 *     Qt Multimedia 的 QAudioSink 把 PCM 播出去（本工程的 Qt 里已经装了
 *     qtmultimedia，这一步是满足的）；
 *   * 换过去之后这里几个方法（speak / stop / pause / voiceFor）的语义能一一对上，
 *     但音色清单会从"两个"变成系统里全部，自检里那几条断言（比如"数得出音色"）
 *     仍然成立。
 *
 * 三条设计上的硬要求：
 *
 * 1. **不阻塞界面**。朗读几秒钟起步，SAPI 那套 COM 对象也有单元亲和性，
 *    所以引擎活在一个**自己的线程**里（见 Speech.cpp 的 Worker），QML 这边
 *    只喊一声就返回，进度靠 speakingChanged 信号报回来。
 *
 * 2. **能中途掐掉**。再点一次就是停（卡片上的按钮就是"播放 / 停止"两态）。
 *
 * 3. **没引擎也不能崩**。系统里一个音色都没有时（精简版 Windows / 没装语音
 *    包），available 是 false，QML 那边把按钮画成灰的、点不动，其余功能照旧 ——
 *    和 PinOcr 找不到 OCR 语言包时是同一个态度。
 *
 * QML 用法（见 qml/translate/TranslateCard.qml 标题栏那个喇叭按钮）：
 *
 *     import SmartClip.Globals 1.0
 *     onClicked: Speech.toggle(root.textOut, root.targetLang)
 *     // 按钮的样子看 Speech.speaking / Speech.available
 */
class Speech final : public QObject {
    Q_OBJECT

    /* 这台机器上到底能不能念（至少要有一个音色） */
    Q_PROPERTY(bool available READ available NOTIFY changed)
    /* 正在念（界面上的"播放 / 停止"两态看它） */
    Q_PROPERTY(bool speaking READ speaking NOTIFY speakingChanged)
    /* 念到一半停住（暂停态；界面可以不区分，放着给以后用） */
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    /* 最近一次出岔子的一句话人话（念成功了会清空） */
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    /* 这台机器上装了哪些音色（调试 / 以后做设置项看它） */
    Q_PROPERTY(QStringList voices READ voices NOTIFY voicesChanged)

public:
    explicit Speech(QObject *parent = nullptr);
    ~Speech() override;

    bool available() const { return m_available; }
    bool speaking() const { return m_speaking; }
    bool paused() const { return m_paused; }
    QString error() const { return m_error; }
    QStringList voices() const { return m_voices; }

    /* 念一段（已经在念就先掐掉再念这段）。空串 = 什么都不做 */
    Q_INVOKABLE void speak(const QString &text, const QString &lang = QString());
    /* 掐掉当前的（没在念就什么都不做） */
    Q_INVOKABLE void stop();
    /* 停住但记住位置，再调一次接着念 */
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    /*
     * 卡片上那个按钮的动作：在念 -> 停；没念 -> 念这段。
     * 返回"喊完之后在不在念"（喊的那一下还不知道，看 speaking 属性 / 信号）。
     */
    Q_INVOKABLE void toggle(const QString &text, const QString &lang = QString());
    /*
     * 某种语言该用哪个音色：给 "中文（简体）" / "zh-CN" / "Chinese" 都能认，
     * 挑不出对应的就退回系统默认音色。空串 = 用默认。
     */
    Q_INVOKABLE QString voiceFor(const QString &lang) const;

signals:
    void changed();
    void availableChanged();
    void speakingChanged();
    void pausedChanged();
    void errorChanged();
    void voicesChanged();
    /* 一段念完了（被 stop() 掐掉的不算） */
    void finished();

private:
    /* 引擎那边报回来的状态（见 Speech.cpp：只该由 WorkerEntry 调） */
    void engineReady(bool available, const QStringList &voices);
    void stateFrom(int state, const QString &error);
    void finishedFrom();
    void setError(const QString &text);

    bool m_available = false;
    bool m_speaking = false;
    bool m_paused = false;
    QString m_error;
    QStringList m_voices;
    /* 系统默认音色（SAPI 清单里第一个）：挑不出对应语言的音色时退到它，
     * 也是 voiceFor() 认不出语言时的答案 */
    QString m_defaultVoice;

    class Worker;
    Worker *m_worker = nullptr;
};
