#include "conddialog.h"
#include "style.h"
#include "zombiedata.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

CondDialog::CondDialog(int scene, QWidget *parent)
    : QDialog(parent), m_scene(scene)
{
    setWindowTitle(QString::fromUtf8("编辑出怪条件"));
    // 去掉标题栏上的"这是什么?"按钮：程序没有设置任何 whatsThis 文本, 点它只会
    // 进入上下文帮助模式却没有内容可显示, 看起来像失灵
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    // 窗口宽度由内容决定: 名字一列定宽 96、四个勾选列 56, 合计约 320,
    // 所以整窗收到 400 就够, 不再固定 560。
    setFixedWidth(400);
    buildUi();
    rebuildTable();
}

void CondDialog::buildUi()
{
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    QLabel *title = new QLabel(QString::fromUtf8("出怪要求"));
    title->setObjectName("CardTitle");
    root->addWidget(title);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(QStringList()
        << QString::fromUtf8("僵尸") << QString::fromUtf8("必出")
        << QString::fromUtf8("禁出") << QString::fromUtf8("至少出")
        << QString::fromUtf8("至多出"));
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->horizontalHeader()->setStretchLastSection(false);
    // 僵尸名居中, 名字一列定宽 (四个勾选列各 56), 窗口因此能收到很窄
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->setColumnWidth(0, 96);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    for (int c = 1; c < 5; ++c) {
        m_table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Fixed);
        m_table->setColumnWidth(c, 56);
    }
    m_table->verticalHeader()->setDefaultSectionSize(26);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (!item || item->column() == 0) return;
        const int row = item->row();
        if (item->checkState() == Qt::Checked) {
            QSignalBlocker block(m_table);
            for (int c = 1; c < 5; ++c) {
                if (c == item->column()) continue;
                QTableWidgetItem *o = m_table->item(row, c);
                if (o && o->checkState() == Qt::Checked) o->setCheckState(Qt::Unchecked);
            }
        }
        // 汇总到成员
        m_needMask = m_refuseMask = m_atLeastMask = m_atMostMask = 0;
        for (int r = 0; r < m_rows.size(); ++r) {
            const quint32 bit = (1u << m_rows[r]);
            if (m_table->item(r, 1) && m_table->item(r, 1)->checkState() == Qt::Checked) m_needMask |= bit;
            if (m_table->item(r, 2) && m_table->item(r, 2)->checkState() == Qt::Checked) m_refuseMask |= bit;
            if (m_table->item(r, 3) && m_table->item(r, 3)->checkState() == Qt::Checked) m_atLeastMask |= bit;
            if (m_table->item(r, 4) && m_table->item(r, 4)->checkState() == Qt::Checked) m_atMostMask |= bit;
        }
        syncCountLimits();
    });
    root->addWidget(m_table, 1);

    // --- 至少 / 至多 的数量 ---
    // 文案压到最短: 「至少出」 [2] 种　「至多出」 [2] 种, 整行才放得进窄窗口
    QHBoxLayout *cnt = new QHBoxLayout;
    cnt->setSpacing(6);

    m_atLeastLabel = new QLabel(QString::fromUtf8("「至少出」"));
    cnt->addWidget(m_atLeastLabel);
    m_atLeastSpin = new QSpinBox(this);
    m_atLeastSpin->setRange(0, 19);
    m_atLeastSpin->setFixedWidth(56);
    cnt->addWidget(m_atLeastSpin);
    cnt->addWidget(new QLabel(QString::fromUtf8("种")));

    cnt->addSpacing(12);

    m_atMostLabel = new QLabel(QString::fromUtf8("「至多出」"));
    cnt->addWidget(m_atMostLabel);
    m_atMostSpin = new QSpinBox(this);
    m_atMostSpin->setRange(0, 19);
    m_atMostSpin->setFixedWidth(56);
    cnt->addWidget(m_atMostSpin);
    cnt->addWidget(new QLabel(QString::fromUtf8("种")));
    cnt->addStretch(1);
    root->addLayout(cnt);

    QHBoxLayout *btns = new QHBoxLayout;
    QPushButton *clear = new QPushButton(QString::fromUtf8("全部清空"), this);
    clear->setObjectName("Danger");
    connect(clear, &QPushButton::clicked, this, &CondDialog::onClear);
    btns->addWidget(clear);
    btns->addStretch(1);

    QPushButton *cancel = new QPushButton(QString::fromUtf8("取消"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    btns->addWidget(cancel);

    QPushButton *ok = new QPushButton(QString::fromUtf8("确定"), this);
    ok->setObjectName("Primary");
    ok->setDefault(true);
    connect(ok, &QPushButton::clicked, this, &CondDialog::onAccept);
    btns->addWidget(ok);

    root->addLayout(btns);
}

void CondDialog::rebuildTable()
{
    m_rows = sceneZombieList(m_scene);
    // 普僵每一轮都固定出现, 设不设条件都一样, 不给用户选择的机会
    m_rows.removeAll(Z_REGULAR);
    QSignalBlocker block(m_table);
    m_table->setRowCount(m_rows.size());

    for (int r = 0; r < m_rows.size(); ++r) {
        const int z = m_rows[r];
        QTableWidgetItem *name = new QTableWidgetItem(zombieName(z));
        name->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(r, 0, name);

        for (int c = 1; c < 5; ++c) {
            QTableWidgetItem *it = new QTableWidgetItem();
            it->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            it->setCheckState(Qt::Unchecked);
            it->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(r, c, it);
        }
    }
    m_table->setCurrentCell(-1, -1);   // 别让第一行留下虚线焦点框

    // 高度按行数算准: 之前窗口高度写死 620, 场景行数少时下面就空出近百像素;
    // 现在表格多高窗口就多高, 不铺满也不留白。
    const int rowsH = m_table->horizontalHeader()->sizeHint().height()
                      + m_rows.size() * 26
                      + m_table->frameWidth() * 2 + 2;
    m_table->setFixedHeight(rowsH);
    adjustSize();
    setFixedHeight(sizeHint().height());

    syncCountLimits();
}

void CondDialog::setCondition(const sf::WaveCondition &c)
{
    // 只保留本场景会出现、并且表格里有对应行的僵尸位。
    // 否则跨场景载入预设时会残留一条看不见也点不掉的"幽灵条件":
    // 表格里没有这一行, 用户无法取消, 但 condition() 照样把它交出去,
    // checkConditions() 又会报"勾选了本场景不会出现的僵尸", 无从下手。
    quint32 visible = 0;
    for (int r = 0; r < m_rows.size(); ++r) visible |= (1u << m_rows[r]);

    m_needMask = c.needMask & visible;
    m_refuseMask = c.refuseMask & visible;
    m_atLeastMask = c.atLeastMask & visible;
    m_atMostMask = c.atMostMask & visible;
    // 旧版 setCondition 只剥普僵, 这里统一走 visible, 普僵本来就不在 m_rows 里。
    normalizeMasks();

    QSignalBlocker block(m_table);
    for (int r = 0; r < m_rows.size(); ++r) {
        const quint32 bit = (1u << m_rows[r]);
        m_table->item(r, 1)->setCheckState((m_needMask & bit) ? Qt::Checked : Qt::Unchecked);
        m_table->item(r, 2)->setCheckState((m_refuseMask & bit) ? Qt::Checked : Qt::Unchecked);
        m_table->item(r, 3)->setCheckState((m_atLeastMask & bit) ? Qt::Checked : Qt::Unchecked);
        m_table->item(r, 4)->setCheckState((m_atMostMask & bit) ? Qt::Checked : Qt::Unchecked);
    }
    syncCountLimits();

    m_atLeastSpin->setValue(c.atLeastMask ? c.atLeast : 0);
    m_atMostSpin->setValue(c.atMostMask ? c.atMost : 0);
}

// 四种标记里同一个僵尸只能出现在一个格子里。界面上点勾选时由 itemChanged 保证,
// 但从预设载入的数据可能同时标了"必出"和"禁出", 那样界面会两列都打勾,
// 等于自己承认条件不可能满足。这里按 必出 > 禁出 > 至少 > 至多 的优先级去重。
void CondDialog::normalizeMasks()
{
    m_refuseMask &= ~m_needMask;
    m_atLeastMask &= ~(m_needMask | m_refuseMask);
    m_atMostMask &= ~(m_needMask | m_refuseMask | m_atLeastMask);
}

sf::WaveCondition CondDialog::condition() const
{
    sf::WaveCondition c;
    c.needMask = m_needMask;
    c.refuseMask = m_refuseMask;
    c.atLeastMask = m_atLeastMask;
    c.atMostMask = m_atMostMask;
    c.atLeast = m_atLeastMask ? m_atLeastSpin->value() : 0;
    c.atMost = m_atMostMask ? m_atMostSpin->value() : 0;
    c.sanitize();
    return c;
}

void CondDialog::syncCountLimits()
{
    const int nAtLeast = sf::popcnt(m_atLeastMask);
    const int nAtMost = sf::popcnt(m_atMostMask);

    m_atLeastSpin->setEnabled(nAtLeast > 0);
    m_atLeastLabel->setEnabled(nAtLeast > 0);
    if (nAtLeast > 0) {
        m_atLeastSpin->setRange(1, nAtLeast);
        if (m_atLeastSpin->value() < 1) m_atLeastSpin->setValue(1);
        if (m_atLeastSpin->value() > nAtLeast) m_atLeastSpin->setValue(nAtLeast);
    } else {
        // 没有勾任何"至少出"时就该显示 0, 而不是停在 1 让人以为设了 1 种
        m_atLeastSpin->setRange(0, 0);
        m_atLeastSpin->setValue(0);
    }

    m_atMostSpin->setEnabled(nAtMost > 0);
    m_atMostLabel->setEnabled(nAtMost > 0);
    if (nAtMost > 0) {
        m_atMostSpin->setRange(0, nAtMost);
        if (m_atMostSpin->value() > nAtMost) m_atMostSpin->setValue(nAtMost);
    } else {
        m_atMostSpin->setRange(0, 0);
        m_atMostSpin->setValue(0);
    }
}

void CondDialog::onAccept()
{
    // 条件自身就自相矛盾时, 在这里就拦下来 —— 不用等到点"开始查找"
    const sf::WaveCondition c = condition();
    const QString bad = sf::checkConditionLocal(c, QString::fromUtf8("条件"));
    if (!bad.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("条件无法满足"), bad);
        return;
    }
    accept();
}

void CondDialog::onClear()
{
    QSignalBlocker block(m_table);
    for (int r = 0; r < m_rows.size(); ++r) {
        for (int c = 1; c < 5; ++c) m_table->item(r, c)->setCheckState(Qt::Unchecked);
    }
    m_needMask = m_refuseMask = m_atLeastMask = m_atMostMask = 0;
    syncCountLimits();
    m_atLeastSpin->setValue(0);
    m_atMostSpin->setValue(0);
}
