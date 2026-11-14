#include "mainwindow.h"

#include "banddialog.h"
#include "conddialog.h"
#include "detaildialog.h"
#include "detailview.h"
#include "gpuhook.h"
#include "helppages.h"
#include "searchworker.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegExp>
#include <QRegExpValidator>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>

#include <algorithm>

namespace {
// 条件分段对话框的扫描区间上限: 条件自己决定真正范围, 这里给足整段
const int kOuterBegin = 2;      // F 3 起: 第 1 ~ 2 轮固定是普僵+路障/读报, 查不到别的
const int kOuterEnd = 10000;

// 左栏定宽。PE/FE 是僵尸种类最多的场景 (20 种), 条件列要能一行放下
// "必出 " + 20 个单字简称, 否则最长的那条会被截成 "…"
const int kLeftWidth = 400;

// 范围列只放 "11 到 40 F" 这种短串, 给死宽度免得挤压条件列
const int kRangeColumnWidth = 88;

// 出怪条件与全局条件两张表的高度统一: 表头 + 3 行可见, 再多就在表格里滚。
// 行高 18 是下限: 11px 的字一行约 15px, 再配合 QTableWidget::item 上下各 1px
// 的内边距, 18 刚好放得下且不切字。表头 20 同理 (上下各 2px 内边距)。
const int kConditionRowHeight = 18;
const int kConditionHeaderHeight = 20;
const int kConditionTableHeight =
    kConditionHeaderHeight + 3 * kConditionRowHeight + 2 + 2;

// 右栏宽度。只剩"种子输入框 104 + 查看出怪 ~90"这一行了, 240 就够。
// 再宽就是给结果表留白: 种子列是 Stretch, 多出来的宽度全变空白。
const int kRightWidth = 240;

QString hex8(quint32 v)
{
    return QString::number(v, 16).toUpper().rightJustified(8, QLatin1Char('0'));
}

QString bigNumber(quint64 v)
{
    const QString s = QString::number(v);
    QString out;
    int c = 0;
    for (int i = s.size() - 1; i >= 0; --i) {
        out.prepend(s.at(i));
        if (++c % 3 == 0 && i > 0) out.prepend(QLatin1Char(','));
    }
    return out;
}

// 卡片内边距与间距: 主界面四张卡必须用同一套数字, 否则卡片顶边、按钮位置
// 会相差 1~2px, 一排看过去就是歪的。抽成常量, 加卡片时直接引用。
const int kCardMarginH = 8;
const int kCardMarginV = 2;
const int kCardSpacing = 3;

// 标题行是自定义控件时走这个 (右栏那张卡的标题行里有种子输入框和按钮)
QFrame *makeCard(QWidget *titleRow, QWidget *child)
{
    QFrame *card = new QFrame;
    card->setObjectName("Card");
    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(kCardMarginH, kCardMarginV, kCardMarginH, kCardMarginV);
    lay->setSpacing(kCardSpacing);
    lay->addWidget(titleRow);
    lay->addWidget(child, 1);
    return card;
}

// 标题为纯文字时走这个 (左栏三张卡)
QFrame *makeCard(const QString &title, QWidget *child)
{
    QLabel *t = new QLabel(title);
    t->setObjectName("CardTitle");
    return makeCard(t, child);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QString::fromUtf8("种子查找器 2.0"));
    // 左栏定宽 kLeftWidth, 右栏定宽 kRightWidth。
    // 高度 464 = 字号 11px 后实测所需: 条件表行高提到 18 (否则文字被切),
    // 两张条件表各高 4px, 再加上进度条那一行 16px。
    // 高度以实测的 hint 为准, 不能用估算 —— 差 1px 就会出滚动条。
    resize(kLeftWidth + kRightWidth + 32, 464);
    setMinimumSize(kLeftWidth + kRightWidth + 24, 464);
    buildUi();

    // 启动时不预置任何出怪条件: 用户自己从"添加"开始加,
    // 免得上来就带着一条他没设过、也不知道该不该留的红眼条件。
    refreshGlobalTable();
}

MainWindow::~MainWindow()
{
    if (m_thread) {
        if (m_worker) m_worker->requestStop();
        m_thread->quit();
        m_thread->wait(3000);
    }
}

// 命令行截图用: 一套能同时展示"多段出怪条件 + 四组条件 + 全局条件"的演示参数。
// 每段 F 数范围都只覆盖 1 轮, 保证条件不会严到无解。
void MainWindow::loadDemo()
{
    m_bands.clear();

    sf::RuleBand b1;
    b1.beginFlag = 10;
    b1.endFlag = 12;
    b1.cond.needMask = (1u << Z_GIGA);      // 必出红眼
    b1.cond.sanitize();
    m_bands << b1;

    sf::RuleBand b2;
    b2.beginFlag = 20;
    b2.endFlag = 22;
    b2.cond.needMask = (1u << Z_GARG);      // 必出白眼
    b2.cond.sanitize();
    m_bands << b2;

    sf::RuleBand b3;
    b3.beginFlag = 30;
    b3.endFlag = 32;
    b3.cond.atLeastMask = (1u << Z_BUCKET) | (1u << Z_DOOR) | (1u << Z_FOOTBALL);
    b3.cond.atLeast = 2;                    // 铁桶 / 铁门 / 橄榄 至少出 2 种
    b3.cond.sanitize();
    m_bands << b3;

    sf::RuleBand b4;
    b4.beginFlag = 40;
    b4.endFlag = 42;
    b4.cond.refuseMask = (1u << Z_JACK);    // 禁出小丑
    b4.cond.sanitize();
    m_bands << b4;

    // 全局条件(硬性): 第 11~12 轮必须出现撑杆或读报
    m_hard.clear();
    sf::RuleBand h;
    h.beginFlag = 10;
    h.endFlag = 12;
    h.cond.needMask = (1u << Z_POLE) | (1u << Z_NEWS);
    h.cond.sanitize();
    m_hard << h;

    refreshBandTable();
    refreshGlobalTable();
}

void MainWindow::setSpanForTest(quint64 n)
{
    m_spanOverride = n;
}

void MainWindow::setSceneForTest(int sceneIndex)
{
    m_scene->setCurrentIndex(sceneIndex);
}

void MainWindow::setBadDemo()
{
    loadDemo();
    sf::RuleBand &b = m_bands.first();
    b.beginFlag = 2;                    // 第 2 轮 (F 3~4)
    b.endFlag = 4;
    b.cond = sf::WaveCondition();
    b.cond.needMask = (1u << Z_ZOMBONI); // 必出冰车
    b.cond.sanitize();
    refreshBandTable();
}

// 测试用: 按 index 装入第 n 组非法条件, 覆盖 checkConditions 的每条拦截规则。
// 返回 false 表示 index 超出范围。
bool MainWindow::setBadCase(int index)
{
    loadDemo();
    m_bands.clear();
    m_hard.clear();
    // 每组用例都从默认场景 (PE/FE) 开始, 免得受上一条用例的影响。
    setSceneForTest(2);
    sf::RuleBand b;
    switch (index) {
    case 0:     // 必出红眼早于第 6 轮
        b.beginFlag = 2; b.endFlag = 4;
        b.cond.needMask = (1u << Z_GIGA);
        break;
    case 1:     // 必出冰车早于第 3 轮
        b.beginFlag = 2; b.endFlag = 4;
        b.cond.needMask = (1u << Z_ZOMBONI);
        break;
    case 2:     // 第 1 轮路障读报同时禁出
        b.beginFlag = 0; b.endFlag = 2;
        b.cond.refuseMask = (1u << Z_CONE) | (1u << Z_NEWS);
        break;
    case 3:     // 早期轮要求种类数超过上限 (第 2 轮最多 4 种)
        b.beginFlag = 2; b.endFlag = 4;
        b.cond.atLeastMask = (1u << Z_BUCKET) | (1u << Z_DOOR) | (1u << Z_FOOTBALL)
                             | (1u << Z_DANCE) | (1u << Z_SNORKEL);
        b.cond.atLeast = 5;
        break;
    case 4:     // 至多出 0 种, 但路障/读报每轮必出其一
        // (原来这条测的是"至少出数量大于至多出数量", 但这两个集合可以不同,
        //  那样组合本身是合法的, 校验已按要求删除, 换成至多出限死用例)
        b.beginFlag = 10; b.endFlag = 12;
        b.cond.atMostMask = (1u << Z_CONE) | (1u << Z_NEWS);
        b.cond.atMost = 0;
        break;
    case 5:     // 必出∩禁出
        b.beginFlag = 10; b.endFlag = 12;
        b.cond.needMask = (1u << Z_JACK);
        b.cond.refuseMask = (1u << Z_JACK);
        break;
    case 6:     // 第 1 轮要求同轮出现 3 种非固定僵尸: 第 1 轮只有 2 个新增名额
        // (原来这条测的是"勾了本场景不会出现的僵尸", 但按需求这类僵尸在条件
        //  对话框里根本选不到, 引擎不再单独拦截, 换成同类的名额超限用例)
        b.beginFlag = 0; b.endFlag = 2;
        b.cond.needMask = (1u << Z_POLE) | (1u << Z_BUCKET) | (1u << Z_DOOR);
        b.cond.needTotal = 3;
        break;
    case 7: {   // 跨段矛盾: 一段必出小丑, 重叠段禁出小丑
        b.beginFlag = 10; b.endFlag = 16;
        b.cond.needMask = (1u << Z_JACK);
        m_bands << b;
        sf::RuleBand c;
        c.beginFlag = 14; c.endFlag = 20;
        c.cond.refuseMask = (1u << Z_JACK);
        c.cond.sanitize();
        m_bands << c;
        refreshBandTable();
        refreshGlobalTable();
        return true;
    }
    case 8:     // 路障 + 读报同时禁出 (任意范围都非法)
        b.beginFlag = 10; b.endFlag = 16;
        b.cond.refuseMask = (1u << Z_CONE) | (1u << Z_NEWS);
        break;
    case 9:     // 早期轮必出 3 种非固定僵尸: 第 2 轮只有 2 个新增名额
        b.beginFlag = 2; b.endFlag = 4;
        b.cond.needMask = (1u << Z_POLE) | (1u << Z_BUCKET) | (1u << Z_DOOR);
        b.cond.needTotal = 3;
        break;
    case 10:    // 长区间但起点很早: 3 到 10 F 必出 海豚/小丑/气球 (第 2 轮只有 2 个名额)
        b.beginFlag = 2; b.endFlag = 10;
        b.cond.needMask = (1u << Z_DOLPHIN) | (1u << Z_JACK) | (1u << Z_BALLOON);
        b.cond.needTotal = 3;
        break;
    default:
        return false;
    }
    b.cond.sanitize();
    m_bands << b;
    refreshBandTable();
    refreshGlobalTable();
    return true;
}

// ---------------------------------------------------------------------------
//  界面
// ---------------------------------------------------------------------------
void MainWindow::buildUi()
{
    buildMenuBar();

    QWidget *central = new QWidget(this);
    QVBoxLayout *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    QWidget *body = new QWidget(central);
    body->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout *bodyLay = new QHBoxLayout(body);
    bodyLay->setContentsMargins(10, 10, 10, 10);
    bodyLay->setSpacing(10);

    // ---- 左栏 ----
    QWidget *leftInner = new QWidget;
    QVBoxLayout *leftLay = new QVBoxLayout(leftInner);
    leftLay->setContentsMargins(0, 0, 4, 0);
    leftLay->setSpacing(5);
    leftLay->addWidget(buildParamCard());
    leftLay->addWidget(buildBandCard());
    leftLay->addWidget(buildGlobalCard());
    leftLay->addStretch(1);
    for (int i = 0; i < leftLay->count(); ++i) {
        if (QWidget *card = leftLay->itemAt(i)->widget())
            card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    }

    QScrollArea *scroll = new QScrollArea;
    scroll->setWidget(leftInner);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 左栏定宽: 按最多种僵尸的场景 (PE/FE 的 20 种) 的单字简称算出来,
    // 保证"必出 20 个字"这种最长的一行不会被截断。
    scroll->setFixedWidth(kLeftWidth);

    QWidget *right = buildResultCard();

    // 左右两栏宽度固定 (左栏按最多的 20 种僵尸算), 不提供拖动分隔条
    QHBoxLayout *split = new QHBoxLayout;
    split->setContentsMargins(0, 0, 0, 0);
    split->setSpacing(6);
    split->addWidget(scroll, 0);
    split->addWidget(right, 1);

    bodyLay->addLayout(split);
    root->addWidget(body, 1);
    setCentralWidget(central);
}

// 菜单栏: 与 1.2 版一致, 帮助放在窗口顶部的原生菜单里
void MainWindow::buildMenuBar()
{
    QMenuBar *bar = menuBar();

    QMenu *presetMenu = bar->addMenu(QString::fromUtf8("预设"));
    QAction *actSave = presetMenu->addAction(QString::fromUtf8("保存当前参数…"));
    QAction *actLoad = presetMenu->addAction(QString::fromUtf8("载入预设…"));
    QAction *actDel = presetMenu->addAction(QString::fromUtf8("删除预设…"));
    connect(actSave, &QAction::triggered, this, &MainWindow::onSavePreset);
    connect(actLoad, &QAction::triggered, this, &MainWindow::onLoadPreset);
    connect(actDel, &QAction::triggered, this, &MainWindow::onDeletePreset);

    QMenu *helpMenu = bar->addMenu(QString::fromUtf8("帮助"));
    QAction *actHowto = helpMenu->addAction(QString::fromUtf8("使用说明"));
    QAction *actAbout = helpMenu->addAction(QString::fromUtf8("关于"));
    connect(actHowto, &QAction::triggered, this, &MainWindow::onShowHowto);
    connect(actAbout, &QAction::triggered, this, &MainWindow::onShowAbout);
}

QWidget *MainWindow::buildParamCard()
{
    QWidget *inner = new QWidget;
    QGridLayout *g = new QGridLayout(inner);
    g->setContentsMargins(0, 0, 0, 0);
    g->setHorizontalSpacing(6);
    g->setVerticalSpacing(3);

    auto addLabel = [g](int row, int col, const QString &text) {
        QLabel *l = new QLabel(text);
        l->setObjectName("Hint");
        g->addWidget(l, row, col);
    };

    int r = 0;
    // 第一行: 用户编号 / 存档编号 / 场景。
    // 场景原本独占一行, 跟这两项并排之后参数卡少一行, 主界面的高度才压得下来。
    addLabel(r, 0, QString::fromUtf8("用户编号"));
    m_uid = new QSpinBox;
    m_uid->setRange(1, 99);
    m_uid->setValue(1);
    g->addWidget(m_uid, r, 1);
    addLabel(r, 2, QString::fromUtf8("存档编号"));
    m_mode = new QSpinBox;
    m_mode->setRange(1, 99);
    m_mode->setValue(13);
    g->addWidget(m_mode, r, 3);
    addLabel(r, 4, QString::fromUtf8("场景"));
    m_scene = new QComboBox;
    m_scene->addItems(sceneDisplayNames());
    m_scene->setCurrentIndex(2);
    g->addWidget(m_scene, r, 5);
    connect(m_scene, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onSceneChanged);
    ++r;

    // 第二行: 输出种子数 + GPU 开关
    addLabel(r, 0, QString::fromUtf8("输出种子数"));
    m_outputSize = new QSpinBox;
    m_outputSize->setRange(1, 500);
    m_outputSize->setValue(20);
    g->addWidget(m_outputSize, r, 1);

    // GPU 开关: 只有真的编进了 CUDA 后端并且有可用设备时才可选,
    // 否则置灰勾不上 (此时引擎本来也只会走 CPU)。
    m_useGpu = new QCheckBox(QString::fromUtf8("用 GPU 加速"));
    m_useGpu->setChecked(sf::gpuBackendReady());
    m_useGpu->setEnabled(sf::gpuBackendReady());
    g->addWidget(m_useGpu, r, 2, 1, 4);
    ++r;

    g->setColumnStretch(1, 1);
    g->setColumnStretch(3, 1);
    g->setColumnStretch(5, 2);

    // 状态文字: 按 v1.2 的布局放在「输出种子数」和「开始查找」之间。
    m_status = new QLabel(QString::fromUtf8("就绪"));
    m_status->setObjectName("Stat");
    m_status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(14);

    // 进度条: 单独占一行, 固定在卡片上的位置, 一直显示。
    // 以前是隐藏的、开始查找才出现, 结果是左栏高度在"点开始"和"跑完"两个时刻
    // 各变一次, 会闪一下甚至临时撑出滚动条。常驻就没这个问题了。
    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(16);

    // 按钮行。三个按钮等宽 (1:1:1): 原来"开始查找"占两份,
    // 一行看过去宽度不均, 也不利于把左栏收窄。
    QHBoxLayout *btns = new QHBoxLayout;
    btns->setSpacing(6);
    m_startBtn = new QPushButton(QString::fromUtf8("开始查找"));
    m_startBtn->setObjectName("Primary");
    connect(m_startBtn, &QPushButton::clicked, this, &MainWindow::onStart);
    // 暂停键: 点一下原地暂停(文字变"继续"), 再点接着算。
    // 只在查找进行中可用, 暂停期间"开始查找"保持禁用, 避免暂停中又开一轮。
    m_pauseBtn = new QPushButton(QString::fromUtf8("暂停"));
    m_pauseBtn->setEnabled(false);
    connect(m_pauseBtn, &QPushButton::clicked, this, &MainWindow::onPauseToggle);
    m_stopBtn = new QPushButton(QString::fromUtf8("停止"));
    m_stopBtn->setObjectName("Danger");
    m_stopBtn->setEnabled(false);
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::onStop);

    // 状态文字在上, 进度条单独一行在下 (常驻, 不随查找显隐)。
    QVBoxLayout *wrap = new QVBoxLayout;
    wrap->setContentsMargins(0, 0, 0, 0);
    wrap->setSpacing(3);
    wrap->addWidget(inner);
    wrap->addWidget(m_status);
    wrap->addWidget(m_progress);
    btns->addWidget(m_startBtn, 1);
    btns->addWidget(m_pauseBtn, 1);
    btns->addWidget(m_stopBtn, 1);
    wrap->addLayout(btns);

    QWidget *holder = new QWidget;
    holder->setLayout(wrap);
    return makeCard(QString::fromUtf8("搜索参数"), holder);
}

QWidget *MainWindow::buildBandCard()
{
    QWidget *inner = new QWidget;
    QVBoxLayout *lay = new QVBoxLayout(inner);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);

    m_bandTable = new QTableWidget;
    m_bandTable->setColumnCount(2);
    m_bandTable->setHorizontalHeaderLabels(QStringList()
        << QString::fromUtf8("范围") << QString::fromUtf8("条件"));
    m_bandTable->verticalHeader()->setVisible(false);
    m_bandTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bandTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_bandTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bandTable->setAlternatingRowColors(true);
    m_bandTable->setShowGrid(false);
    m_bandTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_bandTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_bandTable->setColumnWidth(0, kRangeColumnWidth);
    m_bandTable->verticalHeader()->setDefaultSectionSize(kConditionRowHeight);
    // 表头也写死高度: QSS 里 QHeaderView::section 有 2px 上下内边距,
    // 不设死的话光表头就占 22px, 3 行可见就要多占几像素高度。
    m_bandTable->horizontalHeader()->setFixedHeight(kConditionHeaderHeight);
    // 两张条件表高度一致 (表头 + 3 行), 左右两块卡片看起来才整齐
    m_bandTable->setFixedHeight(kConditionTableHeight);
    m_bandTable->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    connect(m_bandTable, &QTableWidget::doubleClicked, this, [this](const QModelIndex &) {
        onEditBand();
    });
    lay->addWidget(m_bandTable, 1);

    QHBoxLayout *row = new QHBoxLayout;
    row->setSpacing(6);
    QPushButton *add = new QPushButton(QString::fromUtf8("添加"));
    add->setObjectName("Primary");
    connect(add, &QPushButton::clicked, this, &MainWindow::onAddBand);
    QPushButton *del = new QPushButton(QString::fromUtf8("删除"));
    del->setObjectName("Danger");
    connect(del, &QPushButton::clicked, this, &MainWindow::onRemoveBand);
    row->addWidget(add);
    row->addWidget(del);
    row->addStretch(1);
    lay->addLayout(row);

    return makeCard(QString::fromUtf8("出怪条件"), inner);
}

QWidget *MainWindow::buildGlobalCard()
{
    QWidget *inner = new QWidget;
    QVBoxLayout *lay = new QVBoxLayout(inner);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);

    m_globalTable = new QTableWidget;
    m_globalTable->setColumnCount(2);
    m_globalTable->setHorizontalHeaderLabels(QStringList()
        << QString::fromUtf8("范围") << QString::fromUtf8("条件"));
    m_globalTable->verticalHeader()->setVisible(false);
    m_globalTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_globalTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_globalTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_globalTable->setAlternatingRowColors(true);
    m_globalTable->setShowGrid(false);
    m_globalTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_globalTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_globalTable->setColumnWidth(0, kRangeColumnWidth);
    m_globalTable->verticalHeader()->setDefaultSectionSize(kConditionRowHeight);
    m_globalTable->horizontalHeader()->setFixedHeight(kConditionHeaderHeight);
    // 与「出怪条件」表同样高度和尺寸策略, 两块卡片对齐
    m_globalTable->setFixedHeight(kConditionTableHeight);
    m_globalTable->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    connect(m_globalTable, &QTableWidget::doubleClicked, this, [this](const QModelIndex &) {
        onEditGlobal();
    });
    lay->addWidget(m_globalTable);

    QHBoxLayout *row = new QHBoxLayout;
    row->setSpacing(6);
    QPushButton *add = new QPushButton(QString::fromUtf8("添加"));
    add->setObjectName("Primary");
    connect(add, &QPushButton::clicked, this, &MainWindow::onAddGlobal);
    QPushButton *del = new QPushButton(QString::fromUtf8("删除"));
    del->setObjectName("Danger");
    connect(del, &QPushButton::clicked, this, &MainWindow::onRemoveGlobal);
    row->addWidget(add);
    row->addWidget(del);
    row->addStretch(1);
    lay->addLayout(row);

    return makeCard(QString::fromUtf8("全局条件"), inner);
}

QWidget *MainWindow::buildResultCard()
{
    // 标题、种子输入框、"查看出怪" 放在同一行: 纵向上省下一整行,
    // 右栏能多显示一行结果。
    // 标题行是自定义控件, 所以走 makeCard(titleRow, child) 那个重载,
    // 边距/间距与左栏三张卡完全一样。
    QWidget *topRow = new QWidget;
    QHBoxLayout *top = new QHBoxLayout(topRow);
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(8);
    QLabel *title = new QLabel(QString::fromUtf8("查找结果"));
    title->setObjectName("CardTitle");
    top->addWidget(title);
    top->addStretch(1);

    // 种子输入框: 手动输入一个种子, 直接看它在条件覆盖范围内的出怪
    // 不加"种子"两个字的小标签: placeholder 已经说明了要填什么。
    m_seedInput = new QLineEdit;
    m_seedInput->setPlaceholderText(QString::fromUtf8("种子，如 1A2B3C4D"));
    m_seedInput->setMaxLength(8);
    // 8 位 Consolas 实测 88px, 加左右内边距 104 刚好填满, 不留空。
    m_seedInput->setFixedWidth(104);
    m_seedInput->setFont(QFont(QString::fromUtf8("Consolas"), 10));
    // 只允许 16 进制字符
    m_seedInput->setValidator(new QRegExpValidator(QRegExp(QString::fromUtf8("[0-9A-Fa-f]{1,8}")), this));
    connect(m_seedInput, &QLineEdit::returnPressed, this, &MainWindow::onLookupSeed);
    top->addWidget(m_seedInput);
    QPushButton *lookup = new QPushButton(QString::fromUtf8("查看出怪"));
    lookup->setObjectName("Ghost");
    connect(lookup, &QPushButton::clicked, this, &MainWindow::onLookupSeed);
    top->addWidget(lookup);

    m_resultTable = new QTableWidget;
    m_resultTable->setColumnCount(2);
    m_resultTable->setHorizontalHeaderLabels(QStringList()
        << QString::fromUtf8("种子") << QString::fromUtf8("满足轮数"));
    m_resultTable->verticalHeader()->setVisible(false);
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultTable->setAlternatingRowColors(true);
    m_resultTable->setShowGrid(false);
    m_resultTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_resultTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    // 「满足轮数」定宽 84: 实测表头"满足轮数"加粗后 72px, 加左右各 4px 内边距
    // 是 80, 留 4px 余量。写成 Stretch 会把窗口剩余宽度全吃掉, 表格看着很宽,
    // 主界面也因此收不窄。多出来的宽度留给"种子"那一列。
    m_resultTable->horizontalHeader()->setStretchLastSection(false);
    m_resultTable->setColumnWidth(1, 84);
    // 竖向滚动条常驻: 滚动条一出现 viewport 就窄 10px, "满足轮数"整列左移 10px、
    // "种子"列的居中文字左移 5px —— 结果一多就会看到整张表横向抖一下。
    // 常驻之后宽度恒定, 列永远不跳; 代价是右边一直有一条 10px 的浅灰滚动条。
    m_resultTable->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_resultTable->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    // 行高跟条件表和详情表一个量级: 11px 字号下 26 太松, 一屏少显示好几行
    m_resultTable->verticalHeader()->setDefaultSectionSize(22);
    m_resultTable->setFont(QFont(QString::fromUtf8("Consolas"), 10));
    connect(m_resultTable, &QTableWidget::itemSelectionChanged, this, &MainWindow::onSeedPicked);
    connect(m_resultTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *it) {
        if (!it) return;
        QTableWidgetItem *first = m_resultTable->item(it->row(), 0);
        if (first) showDetailFor(quint32(first->text().toUInt(nullptr, 16)));
    });

    // 详情不再占主界面: 双击结果行 (或点"查看出怪") 时弹独立窗口
    return makeCard(topRow, m_resultTable);
}

// ---------------------------------------------------------------------------
//  参数
// ---------------------------------------------------------------------------
int MainWindow::currentScene() const
{
    const QVector<int> ids = sceneIds();
    const int idx = m_scene->currentIndex();
    if (idx < 0 || idx >= ids.size()) return 2;
    return ids[idx];
}

// 扫描区间 = 所有条件(出怪条件 + 全局条件)覆盖的 F 数范围之并
void MainWindow::effectiveRange(int *beginFlag, int *endFlag) const
{
    int b = -1, e = -1;
    const auto take = [&b, &e](const QVector<sf::RuleBand> &list) {
        for (int i = 0; i < list.size(); ++i) {
            const sf::RuleBand &x = list[i];
            if (x.endFlag <= x.beginFlag) continue;
            if (b < 0 || x.beginFlag < b) b = x.beginFlag;
            if (e < 0 || x.endFlag > e) e = x.endFlag;
        }
    };
    take(m_bands);
    take(m_hard);
    if (beginFlag) *beginFlag = b < 0 ? 0 : b;
    if (endFlag) *endFlag = e < 0 ? 0 : e;
}

sf::SearchSpec MainWindow::currentSpec(QString *error) const
{
    sf::SearchSpec spec;
    spec.uid = m_uid->value();
    spec.mode = m_mode->value();
    spec.scene = currentScene();

    int b = 0, e = 0;
    effectiveRange(&b, &e);
    spec.beginFlag = b;
    spec.endFlag = e;

    // 没有任何条件时区间必然是 0/0, 先判这一条, 免得用户看到"区间无效"这种不着边际的提示
    if (m_bands.isEmpty() && m_hard.isEmpty()) {
        if (error) *error = QString::fromUtf8("请至少添加一条出怪条件");
        return spec;
    }
    if (spec.endFlag <= spec.beginFlag) {
        // 有条件但范围为空: 例如所有带的结束轮都不大于起始轮
        if (error) *error = QString::fromUtf8("出怪条件的范围无效，终止轮必须大于起始轮");
        return spec;
    }

    // 从 0 扫到 FFFFFFFF: 检查全部种子, 不再随机抽样
    spec.seedBegin = 0;
    spec.span = m_spanOverride > 0 ? m_spanOverride : 0x100000000ULL;
    spec.outputSize = m_outputSize->value();
    // 结果表要够 outputSize 个种子: 集齐这么多"全部满足"的种子就够了, 提前收工
    spec.autoStop = quint64(m_outputSize->value());
    spec.bands = m_bands;
    spec.hard = m_hard;
    // 没勾"用 GPU 加速"或者后端不可用时走 CPU。引擎自己也会在小规模检索时
    // 退回 CPU, 所以这里只是用户的显式选择。
    spec.gpu = (m_useGpu && m_useGpu->isChecked()) ? 1 : 0;

    return spec;
}

// 条件合法性检查: 空串 = 合法; 否则返回可读的错误说明。
// 具体规则在引擎的 sf::checkBand 里 (对话框点"确定"时用的是同一个函数),
// 这里只负责把两张表都过一遍——保留它是为了兜底, 例如从预设文件读到
// 一套没经过对话框的脏条件。
QString MainWindow::checkConditions(QString *error) const
{
    if (error) error->clear();
    if (m_bands.isEmpty() && m_hard.isEmpty()) {
        return QString::fromUtf8("请先添加至少一条出怪条件。");
    }

    const int scene = currentScene();
    const QString bandKind = QString::fromUtf8("出怪条件");
    const QString hardKind = QString::fromUtf8("全局条件");

    for (int i = 0; i < m_bands.size(); ++i) {
        const QString bad = sf::checkBand(m_bands[i], scene, bandKind, m_bands);
        if (!bad.isEmpty()) return bad;
    }
    for (int i = 0; i < m_hard.size(); ++i) {
        const QString bad = sf::checkBand(m_hard[i], scene, hardKind, m_hard);
        if (!bad.isEmpty()) return bad;
    }
    return QString();
}

void MainWindow::setRunning(bool running)
{
    m_running = running;
    m_startBtn->setEnabled(!running);
    m_pauseBtn->setEnabled(running);
    m_stopBtn->setEnabled(running);
    // 查找中锁住 GPU 开关: 改了也不会影响正在跑的这一轮, 反而让人以为生效了
    if (m_useGpu) m_useGpu->setEnabled(!running && sf::gpuBackendReady());
    m_startBtn->setText(running ? QString::fromUtf8("查找中…") : QString::fromUtf8("开始查找"));
}

// ---------------------------------------------------------------------------
//  列表刷新
// ---------------------------------------------------------------------------
static void fillRuleRows(QTableWidget *table, const QVector<sf::RuleBand> &bands)
{
    const int n = bands.size();
    table->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        const sf::RuleBand &b = bands.at(i);

        QTableWidgetItem *rng = new QTableWidgetItem(
            sf::flagRangeText(b.beginFlag, b.endFlag));
        rng->setTextAlignment(Qt::AlignCenter);
        rng->setForeground(QColor(0x11, 0x11, 0x11));
        QFont f = rng->font();
        f.setBold(true);
        rng->setFont(f);
        table->setItem(i, 0, rng);

        QTableWidgetItem *cond = new QTableWidgetItem(
            b.cond.isEmpty() ? QString::fromUtf8("（无限制）") : b.cond.shortText());
        table->setItem(i, 1, cond);
    }
}

void MainWindow::refreshBandTable() { fillRuleRows(m_bandTable, m_bands); }

void MainWindow::refreshGlobalTable() { fillRuleRows(m_globalTable, m_hard); }

// ---------------------------------------------------------------------------
//  出怪条件操作
// ---------------------------------------------------------------------------
void MainWindow::onAddBand()
{
    BandDialog dlg(currentScene(), kOuterBegin, kOuterEnd, this);
    // 同时把已有条件交给对话框, 点"确定"时就能拦下跨段矛盾 (不用等开始查找)
    dlg.setSiblings(m_bands);
    // 不预填范围: 两个框留空("—"), 由用户自己填, 点"确定"时空着会报错。
    // 以前这里塞了 3~12 的默认值, 用户不留神就带着一个没想过的区间去查。
    if (dlg.exec() != QDialog::Accepted) return;

    sf::RuleBand b = dlg.band();
    m_bands << b;
    refreshBandTable();
    m_bandTable->selectRow(m_bands.size() - 1);
}

void MainWindow::onEditBand()
{
    const int row = m_bandTable->currentRow();
    if (row < 0 || row >= m_bands.size()) {
        QMessageBox::information(this, QString::fromUtf8("提示"),
                                 QString::fromUtf8("请先选中一条出怪条件。"));
        return;
    }
    BandDialog dlg(currentScene(), kOuterBegin, kOuterEnd, this);
    // 兄弟列表要排除自己, 否则"自己跟自己重叠"永远成立
    QVector<sf::RuleBand> others = m_bands;
    others.remove(row);
    dlg.setSiblings(others);
    dlg.setBand(m_bands[row]);
    if (dlg.exec() != QDialog::Accepted) return;
    sf::RuleBand b = dlg.band();
    m_bands[row] = b;
    refreshBandTable();
    m_bandTable->selectRow(row);
}

void MainWindow::onRemoveBand()
{
    const int row = m_bandTable->currentRow();
    if (row < 0 || row >= m_bands.size()) return;
    m_bands.remove(row);
    refreshBandTable();
}

// 全局条件: 与出怪条件同样的分段输入, 但语义是"这几轮必须全部满足"
void MainWindow::onAddGlobal()
{
    BandDialog dlg(currentScene(), kOuterBegin, kOuterEnd, this);
    dlg.setHardMode(true);
    dlg.setSiblings(m_hard);
    // 同样不预填范围, 留空让用户填
    if (dlg.exec() != QDialog::Accepted) return;

    sf::RuleBand b = dlg.band();
    m_hard << b;
    refreshGlobalTable();
    m_globalTable->selectRow(m_hard.size() - 1);
}

void MainWindow::onEditGlobal()
{
    const int row = m_globalTable->currentRow();
    if (row < 0 || row >= m_hard.size()) {
        QMessageBox::information(this, QString::fromUtf8("提示"),
                                 QString::fromUtf8("请先选中一条全局条件。"));
        return;
    }
    BandDialog dlg(currentScene(), kOuterBegin, kOuterEnd, this);
    dlg.setHardMode(true);
    QVector<sf::RuleBand> others = m_hard;
    others.remove(row);
    dlg.setSiblings(others);
    dlg.setBand(m_hard[row]);
    if (dlg.exec() != QDialog::Accepted) return;
    sf::RuleBand b = dlg.band();
    m_hard[row] = b;
    refreshGlobalTable();
    m_globalTable->selectRow(row);
}

void MainWindow::onRemoveGlobal()
{
    const int row = m_globalTable->currentRow();
    if (row < 0 || row >= m_hard.size()) return;
    m_hard.remove(row);
    refreshGlobalTable();
}

void MainWindow::onSceneChanged(int)
{
    // 场景一变, 原来条件里"这个场景不会出现的僵尸"就变成幽灵位:
    // 对话框里看不见(它按本场景的僵尸列表建行), 也就点不掉, 但 matches() 仍然
    // 拿它去过滤种子 —— 结果是"查不到本该查到的种子". 这里把这类位裁掉。
    const quint32 visible = sceneZombieMask(currentScene());

    auto prune = [visible](QVector<sf::RuleBand> &list) {
        for (int i = 0; i < list.size(); ++i) {
            sf::WaveCondition &c = list[i].cond;
            c.needMask &= visible;
            c.refuseMask &= visible;
            c.atLeastMask &= visible;
            c.atMostMask &= visible;
            c.sanitize();   // 重算 needTotal, 并把 atLeast/atMost 夹回合法范围
        }
    };
    prune(m_bands);
    prune(m_hard);

    refreshBandTable();
    refreshGlobalTable();
    m_status->setText(QString::fromUtf8("已切换场景：%1").arg(m_scene->currentText()));
}

// ---------------------------------------------------------------------------
//  检查
// ---------------------------------------------------------------------------
void MainWindow::onStart()
{
    if (m_running) return;

    QString error;
    const sf::SearchSpec spec = currentSpec(&error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("参数有误"), error);
        return;
    }

    // 非法条件拦截: 例如"3~4 F 必出红眼"在任何种子里都不可能满足, 直接提示, 不开查找
    QString bad = checkConditions(&error);
    if (!bad.isEmpty()) {
        std::fprintf(stderr, "conditions rejected: %s\n", bad.toUtf8().constData());
        QMessageBox::warning(this, QString::fromUtf8("条件无法满足"), bad);
        return;
    }

    sf::RuleTable rank, hard;
    rank.build(spec.bands, spec.beginFlag / 2, spec.endFlag / 2);
    hard.build(spec.hard, spec.beginFlag / 2, spec.endFlag / 2);
    if (rank.empty() && hard.empty()) {
        QMessageBox::warning(this, QString::fromUtf8("缺少条件"),
                             QString::fromUtf8("请先给至少一条出怪条件设置具体要求。"));
        return;
    }

    m_resultTable->setRowCount(0);
    if (m_detailDlg) m_detailDlg->clear();
    m_progress->setValue(0);
    m_lastResult = sf::SearchResult();
    // 记下这一次查找用的"输出种子数": 结果表按它铺行, 之后用户再去改控件
    // 不会让已经出来的结果对不上。
    m_outputSizeAtRun = m_outputSize ? m_outputSize->value() : 20;
    // 上一轮可能停在"继续"上, 新的一轮要从"暂停"这个初始状态开始
    m_paused = false;
    m_pausedMs = 0;
    m_pauseBtn->setText(QString::fromUtf8("暂停"));
    setRunning(true);
    m_runClock.start();
    m_status->setText(QString::fromUtf8("正在查找…"));

    m_thread = new QThread(this);
    m_worker = new SearchWorker(spec);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::started, m_worker, &SearchWorker::run);
    connect(m_worker, &SearchWorker::progress, this, &MainWindow::onProgress);
    connect(m_worker, &SearchWorker::finished, this, &MainWindow::onFinished);
    connect(m_worker, &SearchWorker::finished, m_thread, &QThread::quit);
    // 标准的跨线程释放写法: deleteLater 投到 worker 自己所在的线程,
    // 线程事件循环结束时会被统一处理 (在 GUI 线程里对已停止线程的对象调
    // deleteLater 永远不会执行)
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, this, [this, th = m_thread, w = m_worker]() {
        // 只清掉"自己这一代"的指针: 旧线程的 finished 可能晚于新一轮查找到来
        if (m_worker == w) m_worker = nullptr;
        if (m_thread == th) m_thread = nullptr;
        th->deleteLater();
    });
    m_thread->start();
}

void MainWindow::onStop()
{
    // 先把暂停解掉: 工作线程在暂停循环里等的是"暂停标志", 只有它清掉才能走到
    // cancel 判断上去, 否则按"停止"会一直卡着。
    m_paused = false;
    if (m_pauseBtn) m_pauseBtn->setText(QString::fromUtf8("暂停"));
    if (m_worker) {
        m_worker->setPaused(false);
        m_worker->requestStop();
    }
    m_status->setText(QString::fromUtf8("正在停止…"));
}

void MainWindow::onPauseToggle()
{
    if (!m_worker) return;
    m_paused = !m_paused;
    m_worker->setPaused(m_paused);
    m_pauseBtn->setText(m_paused ? QString::fromUtf8("继续") : QString::fromUtf8("暂停"));
    if (m_paused) {
        m_pauseClock.start();
        onProgress(m_progress->value(), 0, 0);
    } else {
        // 暂停时长累计起来, 估算剩余时间时要从已用时间里扣掉, 否则一条"约剩 5 分钟"
        // 会随着暂停一起涨上去。
        m_pausedMs += m_pauseClock.elapsed();
    }
}

void MainWindow::onProgress(int percent, quint64 scanned, quint64 qualified)
{
    m_progress->setValue(percent);

    // 已检查种子数与命中数: 用简洁的 "1.2 亿 / 命中 35" 形式附在后面。
    // 全量扫描要跑几十分钟, 只报百分比时用户无法判断"到底扫了多少、找到几个",
    // 所以这两个数在进度期间就显示出来 (结束时状态栏会被 onFinished 覆盖)。
    QString progressText;
    if (scanned > 0) {
        progressText = QString::fromUtf8("（已检查 %1 个，命中 %2 个）")
                           .arg(bigNumber(scanned))
                           .arg(bigNumber(qualified));
    }

    // 暂停时不再估时间: 已用时间停在这儿了, 报"约剩 N 秒"会一直在跳。
    if (m_paused) {
        QString s = QString::fromUtf8("已暂停（已完成 %1%）").arg(percent);
        s += progressText;
        m_status->setText(s);
        return;
    }

    if (percent >= 100) {
        m_status->setText(QString::fromUtf8("正在收尾…"));
        return;
    }
    if (!m_runClock.isValid() || percent < 1) {
        QString s = QString::fromUtf8("正在查找…已完成 %1%").arg(percent);
        s += progressText;
        m_status->setText(s);
        return;
    }

    const double sec = double(m_runClock.elapsed() - m_pausedMs) / 1000.0;
    const double left = sec / double(percent) * double(100 - percent);
    QString eta;
    if (left >= 60.0) {
        eta = QString::fromUtf8("约剩 %1 分 %2 秒")
                  .arg(int(left / 60.0))
                  .arg(int(left - 60.0 * int(left / 60.0) + 0.5));
    } else {
        eta = QString::fromUtf8("约剩 %1 秒").arg(int(left + 0.5));
    }
    m_status->setText(QString::fromUtf8("已完成 %1%，%2").arg(percent).arg(eta) + progressText);
}

void MainWindow::onFinished(const sf::SearchResult &result)
{
    m_lastResult = result;
    m_paused = false;
    if (m_pauseBtn) m_pauseBtn->setText(QString::fromUtf8("暂停"));
    setRunning(false);
    m_progress->setValue(100);

    if (!result.error.isEmpty()) {
        m_status->setText(result.error);
        if (m_shotPath.isEmpty()) {
            QMessageBox::warning(this, QString::fromUtf8("查找失败"), result.error);
        } else {
            // --runshot 模式下不能弹模态框, 否则自动化流程挂死
            std::printf("run error: %s\n", result.error.toUtf8().constData());
        }
        finishShotIfNeeded();
        return;
    }

    fillResultTable();

    // 完成态文案必须压成一行: 左栏只有 400 宽, 换成两行就会把纵向撑出滚动条
    // (主界面高度是定值, 纵向没有余量),
    // 所以这里不用整句, 只留"几个种子 / 满足多少 / 几秒"三个数。
    const QString tail = result.stopped ? QString::fromUtf8("，已中断")
                         : (result.enoughFound ? QString::fromUtf8("，早停") : QString());
    const quint64 full = result.fullCount();
    QString statusText;
    if (full > 0) {
        statusText = QString::fromUtf8("%1 种子，全满足 %2，%3 秒")
                         .arg(bigNumber(result.scanned))
                         .arg(bigNumber(full))
                         .arg(result.seconds, 0, 'f', 2);
    } else if (!result.buckets.isEmpty()) {
        const int best = result.buckets.first().matched;
        statusText = QString::fromUtf8("%1 种子，最好 %2/%3，%4 秒")
                         .arg(bigNumber(result.scanned))
                         .arg(best)
                         .arg(result.activeWaves)
                         .arg(result.seconds, 0, 'f', 2);
    } else {
        statusText = QString::fromUtf8("%1 种子，无结果，%2 秒")
                         .arg(bigNumber(result.scanned))
                         .arg(result.seconds, 0, 'f', 2);
    }
    statusText += tail;
    // 让用户看得出这一轮到底是 GPU 算的还是 CPU 算的 ——
    // 小规模检索和没勾"用 GPU 加速"时都会走 CPU, 不标出来容易误以为 GPU 生效了。
    if (result.usedGpu) statusText.prepend(QStringLiteral("GPU · "));
    m_status->setText(statusText);

    if (m_resultTable->rowCount() == 0) {
        if (m_detailDlg) m_detailDlg->clear();
        if (m_shotPath.isEmpty() && !result.stopped) {
            QMessageBox::information(
                this, QString::fromUtf8("没有结果"),
                QString::fromUtf8("全部 %1 个种子里没有满足条件的。可以放宽出怪条件，或缩小轮次区间。")
                    .arg(bigNumber(result.scanned)));
        }
    } else if (full == 0 && m_shotPath.isEmpty() && !result.stopped) {
        // 没有"全部满足"的种子时必须明确提示, 免得用户以为程序没跑出东西
        const int best = result.buckets.isEmpty() ? 0 : result.buckets.first().matched;
        QMessageBox::information(
            this, QString::fromUtf8("没有完全满足的种子"),
            QString::fromUtf8("已检查全部 %1 个种子，没有一条能同时满足所有出怪条件。\n\n"
                              "最好的一档是 %2 / %3 轮，下面的列表按“差几轮”从少到多排列。\n"
                              "可以放宽出怪条件、缩小轮次区间，或把“输出种子数”调大一些再看看。")
                .arg(bigNumber(result.scanned))
                .arg(best)
                .arg(result.activeWaves));
    }

    finishShotIfNeeded();
}

// 截图模式下的统一收尾: 无论成功还是失败都截图并退出, 避免自动化流程挂死
void MainWindow::finishShotIfNeeded()
{
    if (m_shotPath.isEmpty()) return;
    const QString path = m_shotPath;
    m_shotPath.clear();
    QTimer::singleShot(500, this, [this, path]() {
        const bool ok = grab().save(path);
        std::printf("shot %s -> %s\n", path.toUtf8().constData(), ok ? "ok" : "failed");
        QCoreApplication::quit();
    });
}

// 测试用: 让进度条显示出来并停在某个百分比, 用于截图确认样式
void MainWindow::showProgressForTest(int percent)
{
    if (!m_progress) return;
    m_progress->setValue(std::max(0, std::min(100, percent)));
    m_runClock.start();
    onProgress(std::max(0, std::min(100, percent)), 0, 0);
}

// 测试用: 模拟在种子输入框里打字再点"查看出怪"
void MainWindow::lookupSeedForTest(const QString &hex)
{
    if (m_seedInput) m_seedInput->setText(hex);
    onLookupSeed();
}

// 手动输入种子 -> 看它在条件覆盖范围内的出怪表
void MainWindow::onLookupSeed()
{
    const QString text = m_seedInput ? m_seedInput->text().trimmed() : QString();
    if (text.isEmpty()) {
        QMessageBox::information(this, QString::fromUtf8("种子为空"),
                                 QString::fromUtf8("请先输入一个种子（16 进制，例如 1A2B3C4D）。"));
        return;
    }
    bool ok = false;
    const quint32 seed = text.toUInt(&ok, 16);
    if (!ok) {
        QMessageBox::warning(this, QString::fromUtf8("种子格式有误"),
                             QString::fromUtf8("种子必须是 1~8 位 16 进制数字，例如 1A2B3C4D。"));
        return;
    }

    // 没有条件时给个默认范围, 否则表里一行都没有。
    // 这里必须明确告诉用户"用的是默认范围", 否则很容易以为表里的轮次
    // 就是自己在条件表里写的区间。
    int beginFlag = 0, endFlag = 0;
    effectiveRange(&beginFlag, &endFlag);
    const bool defaultRange = (endFlag <= beginFlag);
    if (defaultRange) {
        beginFlag = 0;
        endFlag = 2 * 25;   // 25 轮全查
    }
    const int beginLevel = beginFlag / 2;
    const int endLevel = std::max(beginLevel + 1, endFlag / 2);

    // 规则表可能是空的(还没点过"开始查找"), 临时按当前条件建一张, 只为着色
    sf::RuleTable rank, hard;
    rank.build(m_bands, beginLevel, endLevel);
    hard.build(m_hard, beginLevel, endLevel);

    const std::vector<sf::DetailRow> rows =
        sf::viewDetail(seed, m_uid->value(), m_mode->value(), currentScene(),
                       beginLevel, endLevel, rank, hard);
    showDetailWindow(seed, rows, rank, hard);
    m_status->setText(QString::fromUtf8("已显示种子 %1 在 %2 到 %3 F 范围内的出怪%4")
                          .arg(hex8(seed))
                          .arg(beginFlag + 1)   // F 数从 1 开始, 内部 flag 从 0 开始
                          .arg(endFlag)
                          .arg(defaultRange
                                   ? QString::fromUtf8("（还没有设置条件，按默认 25 轮显示）")
                                   : QString()));
}

// 详情窗口是常驻的非模态窗口 (参考 1.2 版): 第一次用的时候建, 之后复用。
// 自动选中第一行时只刷新内容、不抢焦点, 用户自己双击时才抬到最前。
void MainWindow::showDetailWindow(quint32 seed, const std::vector<sf::DetailRow> &rows,
                                  const sf::RuleTable &rank, const sf::RuleTable &hard)
{
    if (!m_detailDlg) m_detailDlg = new DetailDialog(this);
    m_detailDlg->setData(seed, currentScene(), rows, rank, hard);
    if (m_autoSelect) return;          // 自动选中第一行: 只在后台刷新内容
    m_detailDlg->show();
    m_detailDlg->raise();
    m_detailDlg->activateWindow();
}

void MainWindow::onSeedPicked()
{
    const int row = m_resultTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem *it = m_resultTable->item(row, 0);
    if (!it) return;
    showDetailFor(quint32(it->text().toUInt(nullptr, 16)));
}

// 把 m_lastResult 铺进结果表
void MainWindow::fillResultTable()
{
    const sf::SearchResult &result = m_lastResult;

    // "输出种子数"是总共列多少个种子: 从满足轮数最多的档位往下取。
    // 用发起查找时记下的值, 而不是控件当前值 —— 查找结束后用户可能再去调
    // "输出种子数", 那时表格应该反映这次查找实际用的设置, 不该被后来的输入改掉。
    const int want = m_outputSizeAtRun > 0 ? m_outputSizeAtRun : 20;

    QSignalBlocker block(m_resultTable);
    m_resultTable->setRowCount(0);

    int rows = 0;

    // 结果里只会有"满足全部全局条件"的种子: buckets 已按满足轮数降序
    for (int i = 0; i < result.buckets.size() && rows < want; ++i) {
        const sf::CountBucket &b = result.buckets[i];
        const bool full = (b.matched == result.activeWaves);
        // 一轮都没满足的种子不算"成绩", 不列出来;
        // 但只设了全局条件(没有出怪条件)时 activeWaves 就是 0, 这时能留下来的种子
        // 本身就是"全部满足", 不能按 0 轮淘汰, 否则界面会误报"没有结果"。
        if (b.matched == 0 && result.activeWaves > 0) continue;

        for (int k = 0; k < b.examples.size() && rows < want; ++k) {
            m_resultTable->insertRow(rows);
            QTableWidgetItem *seedItem = new QTableWidgetItem(hex8(b.examples[k]));
            seedItem->setTextAlignment(Qt::AlignCenter);
            m_resultTable->setItem(rows, 0, seedItem);

            // 只设全局条件时没有"出怪条件轮数"可数, 显示 "0 / 0" 会让人以为一个都没满足
            QTableWidgetItem *mItem = new QTableWidgetItem(
                result.activeWaves > 0
                    ? QString::fromUtf8("%1 / %2").arg(b.matched).arg(result.activeWaves)
                    : QString::fromUtf8("—"));
            mItem->setTextAlignment(Qt::AlignCenter);
            mItem->setForeground(full ? QColor(0x11, 0x11, 0x11) : QColor(0x8a, 0x8a, 0x8a));
            QFont mf = mItem->font();
            mf.setBold(full);
            mItem->setFont(mf);
            m_resultTable->setItem(rows, 1, mItem);
            ++rows;
        }
    }
    block.unblock();

    if (rows > 0) {
        // 自动选中第一行只是为了让下方详情有内容可显示,
        // 不能顺带把种子回填进输入框 (用户 m02595 的要求)
        m_autoSelect = true;
        m_resultTable->selectRow(0);
        m_autoSelect = false;
    }
}

// 范围 / 条件表都按"当前界面"重算: 之前直接用缓存的规则表,
// 而它们只在 onStart 里重建, 用户改了条件却没重新查找时会画出过期的条件。
std::vector<sf::DetailRow> MainWindow::detailRows(quint32 seed, sf::RuleTable *rank,
                                                 sf::RuleTable *hard) const
{
    int beginFlag = 0, endFlag = 0;
    effectiveRange(&beginFlag, &endFlag);
    rank->build(m_bands, beginFlag / 2, endFlag / 2);
    hard->build(m_hard, beginFlag / 2, endFlag / 2);
    return sf::viewDetail(seed, m_uid->value(), m_mode->value(), currentScene(),
                          beginFlag / 2, endFlag / 2, *rank, *hard);
}

void MainWindow::showDetailFor(quint32 seed)
{
    sf::RuleTable rank, hard;
    const std::vector<sf::DetailRow> rows = detailRows(seed, &rank, &hard);
    showDetailWindow(seed, rows, rank, hard);

    // 手动选中/双击某个种子时回填输入框, 方便接着微调或复制
    if (!m_autoSelect && m_seedInput) m_seedInput->setText(hex8(seed));
}

// ---------------------------------------------------------------------------
//  帮助
// ---------------------------------------------------------------------------
void MainWindow::onShowHowto() { showHowto(this); }
void MainWindow::onShowAbout() { showAbout(this); }

// ---------------------------------------------------------------------------
//  预设
// ---------------------------------------------------------------------------
Preset MainWindow::currentPreset() const
{
    Preset p;
    p.uid = m_uid->value();
    p.mode = m_mode->value();
    p.scene = currentScene();
    effectiveRange(&p.beginFlag, &p.endFlag);
    p.outputSize = m_outputSize->value();
    p.bands = m_bands;
    p.hard = m_hard;
    return p;
}

void MainWindow::applyPreset(const Preset &p)
{
    {
        const QSignalBlocker b1(m_uid);
        const QSignalBlocker b2(m_mode);
        const QSignalBlocker b3(m_scene);
        const QSignalBlocker b6(m_outputSize);

        m_uid->setValue(p.uid);
        m_mode->setValue(p.mode);
        const int idx = sceneIds().indexOf(p.scene);
        if (idx >= 0) m_scene->setCurrentIndex(idx);
        m_outputSize->setValue(p.outputSize);
    }

    // 普僵每轮必出，已从条件表里移除；旧预设里残留的普僵标记统一清掉,
    // 免得出现一条用户看不见、也改不了的条件把结果全过滤掉。
    // 同理, 预设如果是在别的场景下存的, 里面可能有本场景不会出现的僵尸,
    // 那些位在条件对话框里看不见、点不掉, 必须一起裁掉。
    const quint32 keep = (~(1u << Z_REGULAR)) & sceneZombieMask(currentScene());
    const auto prune = [keep](const QVector<sf::RuleBand> &src) {
        QVector<sf::RuleBand> out = src;
        for (int i = 0; i < out.size(); ++i) {
            sf::WaveCondition &c = out[i].cond;
            c.needMask &= keep;
            c.refuseMask &= keep;
            c.atLeastMask &= keep;
            c.atMostMask &= keep;
            c.sanitize();
        }
        return out;
    };
    m_bands = prune(p.bands);
    m_hard = prune(p.hard);
    refreshBandTable();
    refreshGlobalTable();
    m_status->setText(QString::fromUtf8("已载入预设：%1").arg(p.name));
}

void MainWindow::onSavePreset()
{
    int b = 0, e = 0;
    effectiveRange(&b, &e);
    bool ok = false;
    const QString defName = (e > b)
        ? QString::fromUtf8("%1 · %2 到 %3 F").arg(m_scene->currentText()).arg(b + 1).arg(e)
        : QString::fromUtf8("%1 · 未设条件").arg(m_scene->currentText());
    const QString name = QInputDialog::getText(
        this, QString::fromUtf8("保存预设"), QString::fromUtf8("预设名称："),
        QLineEdit::Normal, defName, &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    Preset p = currentPreset();
    p.name = name.trimmed();

    QString error;
    QVector<Preset> list = loadPresets(&error);
    list << p;
    if (!savePresets(list, &error)) {
        QMessageBox::warning(this, QString::fromUtf8("保存失败"), error);
        return;
    }
    m_status->setText(QString::fromUtf8("已保存预设：%1").arg(p.name));
}

void MainWindow::onLoadPreset()
{
    QString error;
    const QVector<Preset> list = loadPresets(&error);
    if (list.isEmpty()) {
        QMessageBox::information(
            this, QString::fromUtf8("没有预设"),
            error.isEmpty() ? QString::fromUtf8("还没有保存过预设。先在“预设 → 保存当前参数”里存一套。")
                            : error);
        return;
    }

    QStringList names;
    for (int i = 0; i < list.size(); ++i) {
        const Preset &p = list[i];
        // 出怪条件与全局条件都算上: 只数前者时, 一套"只有全局条件"的预设
        // 会显示成"0 条条件", 看起来像空预设。
        const int total = p.bands.size() + p.hard.size();
        names << QString::fromUtf8("%1　· %2 %3~%4 F（%5 条条件）")
                     .arg(p.name)
                     .arg(sceneDisplayNames().value(sceneIds().indexOf(p.scene), QString()))
                     .arg(p.beginFlag + 1)
                     .arg(p.endFlag)
                     .arg(total);
    }

    bool ok = false;
    const QString chosen = QInputDialog::getItem(
        this, QString::fromUtf8("载入预设"), QString::fromUtf8("选择一套参数："), names, 0, false, &ok);
    if (!ok || chosen.isEmpty()) return;

    const int idx = names.indexOf(chosen);
    if (idx < 0) return;
    applyPreset(list[idx]);
}

// 删除预设: 之前只能手改 presets.json, 普通用户根本没有这个入口。
// 删之前要确认一次, 并且用完整路径提示删的是哪个文件。
void MainWindow::onDeletePreset()
{
    QString error;
    QVector<Preset> list = loadPresets(&error);
    if (list.isEmpty()) {
        QMessageBox::information(
            this, QString::fromUtf8("没有预设"),
            error.isEmpty() ? QString::fromUtf8("还没有保存过预设，没有可删除的。") : error);
        return;
    }

    QStringList names;
    for (int i = 0; i < list.size(); ++i) {
        const Preset &p = list[i];
        const int total = p.bands.size() + p.hard.size();
        names << QString::fromUtf8("%1　· %2 %3~%4 F（%5 条条件）")
                     .arg(p.name)
                     .arg(sceneDisplayNames().value(sceneIds().indexOf(p.scene), QString()))
                     .arg(p.beginFlag + 1)
                     .arg(p.endFlag)
                     .arg(total);
    }

    bool ok = false;
    const QString chosen = QInputDialog::getItem(
        this, QString::fromUtf8("删除预设"), QString::fromUtf8("要删除哪一套参数："),
        names, 0, false, &ok);
    if (!ok || chosen.isEmpty()) return;

    const int idx = names.indexOf(chosen);
    if (idx < 0) return;

    const QString name = list[idx].name;
    const auto answer = QMessageBox::question(
        this, QString::fromUtf8("删除预设"),
        QString::fromUtf8("确定要删除预设「%1」吗？\n\n只会删掉这一套参数，不影响其他预设。").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    list.removeAt(idx);
    if (!savePresets(list, &error)) {
        QMessageBox::warning(this, QString::fromUtf8("删除失败"), error);
        return;
    }
    m_status->setText(QString::fromUtf8("已删除预设：%1").arg(name));
}

// ---------------------------------------------------------------------------
//  自动跑: 跑一次查找 -> 截图 -> 退出 (仅命令行调试用)
// ---------------------------------------------------------------------------
void MainWindow::autoRun(const QString &shotPath)
{
    m_shotPath = shotPath;
    QTimer::singleShot(200, this, &MainWindow::onStart);
    // 命令行自动跑时若弹了模态框(参数/条件校验失败), 定时关掉, 让进程能退出。
    // 模态框弹出后 exec() 会开自己的事件循环, 外层 timer 照常触发, 但 close()
    // 只是隐藏, exec() 不返回——必须 delete 才能让 exec() 返回。
    QTimer::singleShot(1200, this, [this]() {
        const auto boxes = findChildren<QMessageBox *>();
        for (QMessageBox *b : boxes) {
            b->deleteLater();
            b->hide();
        }
        // 关完模态框后若还没开始查找(被拦下), 补一张截图再退出
        if (!m_running && !m_shotPath.isEmpty()) {
            const bool ok = grab().save(m_shotPath);
            std::printf("shot %s -> %s (rejected)\n", m_shotPath.toUtf8().constData(),
                        ok ? "ok" : "failed");
            QCoreApplication::quit();
        }
    });
}
