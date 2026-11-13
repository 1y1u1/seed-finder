#include "mainwindow.h"

#include "banddialog.h"
#include "conddialog.h"
#include "detaildialog.h"
#include "detailview.h"
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
const int kLeftWidth = 452;

// 范围列只放 "11 到 40 F" 这种短串, 给死宽度免得挤压条件列
const int kRangeColumnWidth = 96;

// 出怪条件与全局条件两张表的高度统一: 表头 + 4 行, 不再留空行
const int kConditionTableHeight = 128;


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

QFrame *makeCard(const QString &title, QWidget *child)
{
    QFrame *card = new QFrame;
    card->setObjectName("Card");
    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(11, 4, 11, 4);
    lay->setSpacing(4);
    QLabel *t = new QLabel(title);
    t->setObjectName("CardTitle");
    lay->addWidget(t);
    lay->addWidget(child, 1);
    return card;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QString::fromUtf8("种子查找器 v3.0"));
    // 左栏定宽 kLeftWidth, 右栏跟着窗口——结果表只剩两列, 不需要那么宽。
    // 高度刚好放下"参数卡 + 两张条件表(kConditionTableHeight) + 按钮行",
    // 不留大片空白; 查找时多出来的进度条只占 14px, 也能一起放下。
    // 右栏下限由"只看全部满足 + 种子输入框 + 查看出怪"这一行决定, 比这更窄
    // 就会把"查看出怪"按钮挤出去, 所以留 356。
    resize(kLeftWidth + 380, 652);
    setMinimumSize(kLeftWidth + 356, 520);
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
    connect(actSave, &QAction::triggered, this, &MainWindow::onSavePreset);
    connect(actLoad, &QAction::triggered, this, &MainWindow::onLoadPreset);

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
    g->setHorizontalSpacing(8);
    g->setVerticalSpacing(4);

    auto addLabel = [g](int row, int col, const QString &text) {
        QLabel *l = new QLabel(text);
        l->setObjectName("Hint");
        g->addWidget(l, row, col);
    };

    int r = 0;
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
    ++r;

    addLabel(r, 0, QString::fromUtf8("场景"));
    m_scene = new QComboBox;
    m_scene->addItems(sceneDisplayNames());
    m_scene->setCurrentIndex(2);
    g->addWidget(m_scene, r, 1, 1, 3);
    connect(m_scene, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onSceneChanged);
    ++r;

    addLabel(r, 0, QString::fromUtf8("输出种子数"));
    m_outputSize = new QSpinBox;
    m_outputSize->setRange(1, 500);
    m_outputSize->setValue(20);
    g->addWidget(m_outputSize, r, 1);
    ++r;

    g->setColumnStretch(1, 1);
    g->setColumnStretch(3, 1);

    // 状态文字: 按 v1.2 的布局放在「输出种子数」和「开始查找」之间。
    // 至少给两行高度, 免得进度文字一行两行切换时下面的按钮跟着上下跳。
    m_status = new QLabel(QString::fromUtf8("就绪"));
    m_status->setObjectName("Stat");
    m_status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(24);

    // 进度条: 只在查找过程中出现, 与状态文字同宽横排 (参考 1.2 版的位置),
    // 平时隐藏所以不占地方。
    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setVisible(false);

    // 按钮行
    QHBoxLayout *btns = new QHBoxLayout;
    btns->setSpacing(8);
    m_startBtn = new QPushButton(QString::fromUtf8("开始查找"));
    m_startBtn->setObjectName("Primary");
    m_startBtn->setMinimumHeight(32);
    connect(m_startBtn, &QPushButton::clicked, this, &MainWindow::onStart);
    // 暂停键: 点一下原地暂停(文字变"继续"), 再点接着算。
    // 只在查找进行中可用, 暂停期间"开始查找"保持禁用, 避免暂停中又开一轮。
    m_pauseBtn = new QPushButton(QString::fromUtf8("暂停"));
    m_pauseBtn->setMinimumHeight(32);
    m_pauseBtn->setEnabled(false);
    connect(m_pauseBtn, &QPushButton::clicked, this, &MainWindow::onPauseToggle);
    m_stopBtn = new QPushButton(QString::fromUtf8("停止"));
    m_stopBtn->setObjectName("Danger");
    m_stopBtn->setMinimumHeight(32);
    m_stopBtn->setEnabled(false);
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::onStop);

    QVBoxLayout *wrap = new QVBoxLayout;
    wrap->setContentsMargins(0, 0, 0, 0);
    wrap->setSpacing(6);
    wrap->addWidget(inner);
    wrap->addWidget(m_status);
    wrap->addWidget(m_progress);
    btns->addWidget(m_startBtn, 2);
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
    lay->setSpacing(8);

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
    m_bandTable->verticalHeader()->setDefaultSectionSize(24);
    // 两张条件表高度一致 (表头 + 4 行), 左右两块卡片看起来才整齐
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
    lay->setSpacing(8);

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
    m_globalTable->verticalHeader()->setDefaultSectionSize(24);
    // 与「出怪条件」表同样高度, 两块卡片对齐
    m_globalTable->setFixedHeight(kConditionTableHeight);
    connect(m_globalTable, &QTableWidget::doubleClicked, this, [this](const QModelIndex &) {
        onEditGlobal();
    });
    lay->addWidget(m_globalTable);

    QHBoxLayout *row = new QHBoxLayout;
    row->setSpacing(6);
    QPushButton *add = new QPushButton(QString::fromUtf8("添加"));
    connect(add, &QPushButton::clicked, this, &MainWindow::onAddGlobal);
    QPushButton *del = new QPushButton(QString::fromUtf8("删除"));
    connect(del, &QPushButton::clicked, this, &MainWindow::onRemoveGlobal);
    row->addWidget(add);
    row->addWidget(del);
    row->addStretch(1);
    lay->addLayout(row);

    return makeCard(QString::fromUtf8("全局条件"), inner);
}

QWidget *MainWindow::buildResultCard()
{
    QFrame *card = new QFrame;
    card->setObjectName("Card");
    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(8);

    QHBoxLayout *top = new QHBoxLayout;
    QLabel *title = new QLabel(QString::fromUtf8("查找结果"));
    title->setObjectName("CardTitle");
    top->addWidget(title);
    top->addStretch(1);
    lay->addLayout(top);

    QHBoxLayout *filterRow = new QHBoxLayout;
    filterRow->setSpacing(8);
    m_fullOnly = new QCheckBox(QString::fromUtf8("只看全部满足"));
    connect(m_fullOnly, &QCheckBox::toggled, this, &MainWindow::onFullOnlyToggled);
    filterRow->addWidget(m_fullOnly);
    filterRow->addStretch(1);

    // 种子输入框: 手动输入一个种子, 直接看它在条件覆盖范围内的出怪
    // 不加"种子"两个字的小标签: placeholder 已经说明了要填什么, 省下的宽度
    // 直接让整窗收窄 (这一行是右栏最宽的部件, 它决定主界面能收到多窄)。
    m_seedInput = new QLineEdit;
    m_seedInput->setPlaceholderText(QString::fromUtf8("种子，如 1A2B3C4D"));
    m_seedInput->setMaxLength(8);
    m_seedInput->setFixedWidth(124);
    m_seedInput->setFont(QFont(QString::fromUtf8("Consolas"), 10));
    // 只允许 16 进制字符
    m_seedInput->setValidator(new QRegExpValidator(QRegExp(QString::fromUtf8("[0-9A-Fa-f]{1,8}")), this));
    connect(m_seedInput, &QLineEdit::returnPressed, this, &MainWindow::onLookupSeed);
    filterRow->addWidget(m_seedInput);
    QPushButton *lookup = new QPushButton(QString::fromUtf8("查看出怪"));
    lookup->setObjectName("Ghost");
    connect(lookup, &QPushButton::clicked, this, &MainWindow::onLookupSeed);
    filterRow->addWidget(lookup);
    lay->addLayout(filterRow);

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
    // 「满足轮数」定宽 88: 写成 Stretch 会把窗口剩余宽度全吃掉, 表格看着很宽,
    // 主界面也因此收不窄。多出来的宽度留给"种子"那一列。
    m_resultTable->horizontalHeader()->setStretchLastSection(false);
    m_resultTable->setColumnWidth(0, 88);
    m_resultTable->setColumnWidth(1, 88);
    m_resultTable->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    m_resultTable->verticalHeader()->setDefaultSectionSize(26);
    m_resultTable->setFont(QFont(QString::fromUtf8("Consolas"), 10));
    connect(m_resultTable, &QTableWidget::itemSelectionChanged, this, &MainWindow::onSeedPicked);
    connect(m_resultTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *it) {
        if (!it) return;
        QTableWidgetItem *first = m_resultTable->item(it->row(), 0);
        if (first) showDetailFor(quint32(first->text().toUInt(nullptr, 16)));
    });

    // 详情不再占主界面: 双击结果行 (或点"查看出怪") 时弹独立窗口
    lay->addWidget(m_resultTable, 1);

    return card;
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
    dlg.setSiblings(m_bands, false);
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
    dlg.setSiblings(others, false);
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
    dlg.setSiblings(m_hard, true);
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
    dlg.setSiblings(others, true);
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
    m_progress->setVisible(true);
    m_lastResult = sf::SearchResult();
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
    Q_UNUSED(scanned)
    Q_UNUSED(qualified)

    m_progress->setValue(percent);

    // 暂停时不再估时间: 已用时间停在这儿了, 报"约剩 N 秒"会一直在跳。
    if (m_paused) {
        m_status->setText(QString::fromUtf8("已暂停（已完成 %1%）").arg(percent));
        return;
    }

    // 状态栏只报"预测的时间进度": 全量扫描要跑几十分钟, 用户关心的是还剩多久,
    // 种子数与命中数在结果表里都能看到, 重复显示反而把行撑长。
    if (percent >= 100) {
        m_status->setText(QString::fromUtf8("正在收尾…"));
        return;
    }
    if (!m_runClock.isValid() || percent < 1) {
        m_status->setText(QString::fromUtf8("正在查找…已完成 %1%").arg(percent));
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
    m_status->setText(QString::fromUtf8("已完成 %1%，%2").arg(percent).arg(eta));
}

void MainWindow::onFinished(const sf::SearchResult &result)
{
    m_lastResult = result;
    m_paused = false;
    if (m_pauseBtn) m_pauseBtn->setText(QString::fromUtf8("暂停"));
    setRunning(false);
    m_progress->setValue(100);
    // 先让进度条停在 100% 一小会儿再收起来, 否则"跑完瞬间进度条消失",
    // 看不出到底是跑完了还是被中断了。要是在这期间又开了一轮就不收了。
    QTimer::singleShot(1200, this, [this]() {
        if (!m_running) m_progress->setVisible(false);
    });

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

    const QString stopped = result.stopped ? QString::fromUtf8("（已中断）")
                            : (result.enoughFound ? QString::fromUtf8("（已找够，提前停止）") : QString());
    const quint64 full = result.fullCount();
    QString statusText;
    if (full > 0) {
        statusText = QString::fromUtf8("完成%1：检查 %2 个种子，全部满足 %3 个，用时 %4 秒")
                         .arg(stopped)
                         .arg(bigNumber(result.scanned))
                         .arg(bigNumber(full))
                         .arg(result.seconds, 0, 'f', 2);
    } else if (!result.buckets.isEmpty()) {
        const int best = result.buckets.first().matched;
        statusText = QString::fromUtf8("完成%1：检查 %2 个种子，没有全满足的；最好的是 %3 / %4 轮，用时 %5 秒")
                         .arg(stopped)
                         .arg(bigNumber(result.scanned))
                         .arg(best)
                         .arg(result.activeWaves)
                         .arg(result.seconds, 0, 'f', 2);
    } else {
        statusText = QString::fromUtf8("完成%1：检查 %2 个种子，没有任何结果，用时 %3 秒")
                         .arg(stopped)
                         .arg(bigNumber(result.scanned))
                         .arg(result.seconds, 0, 'f', 2);
    }
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
    m_progress->setVisible(true);
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

    // 没有条件时给个默认范围, 否则表里一行都没有
    int beginFlag = 0, endFlag = 0;
    effectiveRange(&beginFlag, &endFlag);
    if (endFlag <= beginFlag) {
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
    m_status->setText(QString::fromUtf8("已显示种子 %1 在 %2 到 %3 F 范围内的出怪")
                          .arg(hex8(seed))
                          .arg(beginFlag + 1)   // F 数从 1 开始, 内部 flag 从 0 开始
                          .arg(endFlag));
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

// 把 m_lastResult 铺进结果表 (受"只看全部满足" 影响)
void MainWindow::fillResultTable()
{
    const sf::SearchResult &result = m_lastResult;
    const bool fullOnly = m_fullOnly && m_fullOnly->isChecked();

    // "输出种子数"是总共列多少个种子: 从满足轮数最多的档位往下取
    const int want = m_outputSize ? m_outputSize->value() : 20;

    QSignalBlocker block(m_resultTable);
    m_resultTable->setRowCount(0);

    int rows = 0;

    // 结果里只会有"满足全部全局条件"的种子: buckets 已按满足轮数降序
    for (int i = 0; i < result.buckets.size() && rows < want; ++i) {
        const sf::CountBucket &b = result.buckets[i];
        const bool full = (b.matched == result.activeWaves);
        if (fullOnly && !full) continue;
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

void MainWindow::onFullOnlyToggled(bool)
{
    fillResultTable();
    if (m_resultTable->rowCount() == 0) {
        if (m_detailDlg) m_detailDlg->clear();
        m_status->setText(QString::fromUtf8("当前筛选下没有结果；关掉“只看全部满足”可以看到差几轮的种子。"));
    }
}

// 范围 / 条件表都按"当前界面"重算: 之前直接用 m_rankTable/m_hardTable,
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
        names << QString::fromUtf8("%1　· %2 %3~%4 F（%5 条条件）")
                     .arg(p.name)
                     .arg(sceneDisplayNames().value(sceneIds().indexOf(p.scene), QString()))
                     .arg(p.beginFlag + 1)
                     .arg(p.endFlag)
                     .arg(p.bands.size());
    }

    bool ok = false;
    const QString chosen = QInputDialog::getItem(
        this, QString::fromUtf8("载入预设"), QString::fromUtf8("选择一套参数："), names, 0, false, &ok);
    if (!ok || chosen.isEmpty()) return;

    const int idx = names.indexOf(chosen);
    if (idx < 0) return;
    applyPreset(list[idx]);
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
