#pragma once

#include <QMainWindow>
#include <QElapsedTimer>
#include <QVector>

#include "presetstore.h"
#include "seedengine.h"

class QComboBox;
class QSpinBox;
class QCheckBox;
class QTableWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QThread;
class SearchWorker;
class DetailDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 供命令行自检/截图使用: 用当前参数跑一次查找, 完成后把窗口存成 png 并退出
    void autoRun(const QString &shotPath);

    // 供命令行截图使用: 装一套"多出怪条件 + 全局条件"的演示参数
    void loadDemo();

    // 仅供命令行自动化截图使用: 把全量扫描临时缩到前 n 个种子, 免得截图等太久
    void setSpanForTest(quint64 n);
    void setBadDemo();          // 测试用: 装入一组非法条件, 验证拦截提示
    bool setBadCase(int index); // 测试用: 依次装入第 n 组非法条件
    void setSceneForTest(int sceneIndex);   // 测试用: 切场景下拉
    QString checkConditionsForTest(QString *error) const { return checkConditions(error); }
    void lookupSeedForTest(const QString &hex);   // 测试用: 模拟在种子输入框里输入并点查看
    void showProgressForTest(int percent);        // 测试用: 让进度条停在某个百分比上截图

private slots:
    void onStart();
    void onStop();
    void onPauseToggle();
    void onAddBand();
    void onEditBand();
    void onRemoveBand();
    void onAddGlobal();
    void onEditGlobal();
    void onRemoveGlobal();
    void onSceneChanged(int index);
    void onProgress(int percent, quint64 scanned, quint64 qualified);
    void onFinished(const sf::SearchResult &result);
    void onSeedPicked();
    void onShowHowto();
    void onShowAbout();
    void onSavePreset();
    void onLoadPreset();
    void onFullOnlyToggled(bool on);
    void onLookupSeed();

private:
    void buildUi();
    void buildMenuBar();
    QWidget *buildParamCard();
    QWidget *buildBandCard();
    QWidget *buildGlobalCard();
    QWidget *buildResultCard();

    int currentScene() const;
    sf::SearchSpec currentSpec(QString *error) const;
    // 扫描区间 = 所有条件覆盖的 F 数范围之并 (界面上不再单独输入起始/终止)
    void effectiveRange(int *beginFlag, int *endFlag) const;
    // 条件合法性检查: 返回空串表示合法, 否则为可读的错误说明 (含僵尸名)
    QString checkConditions(QString *error) const;
    void refreshBandTable();
    void refreshGlobalTable();
    void setRunning(bool running);
    void showDetailFor(quint32 seed);
    // 把明细送进常驻详情窗口; 自动选中时不抢焦点
    void showDetailWindow(quint32 seed, const std::vector<sf::DetailRow> &rows,
                          const sf::RuleTable &rank, const sf::RuleTable &hard);
    // 按"当前界面"重算范围与条件表, 并返回该种子的逐轮明细
    std::vector<sf::DetailRow> detailRows(quint32 seed, sf::RuleTable *rank,
                                          sf::RuleTable *hard) const;
    void fillResultTable();
    void finishShotIfNeeded();
    void applyPreset(const Preset &p);
    Preset currentPreset() const;

    // 出怪条件 (只用于排名) / 全局条件 (硬性要求, 必须全部满足)
    QVector<sf::RuleBand> m_bands;
    QVector<sf::RuleBand> m_hard;

    sf::SearchResult m_lastResult;
    bool m_running = false;
    // 结果表自动选中第一行时不回填种子输入框 (只有用户手动点才算"选中")
    bool m_autoSelect = false;
    // 暂停状态: m_pausedMs 累计暂停时长, 估算剩余时间时要把它从已用时间里扣掉
    bool m_paused = false;
    qint64 m_pausedMs = 0;
    QElapsedTimer m_pauseClock;
    QString m_shotPath;      // 非空时: 查找结束自动截图并退出
    quint64 m_spanOverride = 0;   // 非 0 时: 只扫前 n 个种子(命令行截图用)
    QElapsedTimer m_runClock;     // 用于估算剩余时间

    // 参数区
    QSpinBox *m_uid = nullptr;
    QSpinBox *m_mode = nullptr;
    QComboBox *m_scene = nullptr;
    QSpinBox *m_outputSize = nullptr;
    // GPU 加速开关: 只有后端可用时才勾得上 (没编 CUDA / 没设备时置灰)
    QCheckBox *m_useGpu = nullptr;

    // 出怪条件 / 全局条件
    QTableWidget *m_bandTable = nullptr;
    QTableWidget *m_globalTable = nullptr;

    // 结果区
    QTableWidget *m_resultTable = nullptr;
    // 详情窗口: 常驻非模态, 点"查看出怪"才显示 (参考 1.2 版的独立详情窗)
    DetailDialog *m_detailDlg = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QLineEdit *m_seedInput = nullptr;
    QCheckBox *m_fullOnly = nullptr;
    QPushButton *m_startBtn = nullptr;
    QPushButton *m_pauseBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;

    QThread *m_thread = nullptr;
    SearchWorker *m_worker = nullptr;
};
