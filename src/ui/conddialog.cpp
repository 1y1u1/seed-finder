#include "conddialog.h"
#include "style.h"
#include "zombiedata.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

// 表格行高。19 行 × 26 太高, 一屏塞不下; 勾选框本身只有 14px,
// 上下各留 2px 就够, 所以取 18。
const int kRowH = 18;
const int kHeadH = 22;

// 勾选框在 QTableWidgetItem 里是画在格子左边的, textAlignment 只管文字,
// 所以勾选框永远贴左。这里自己画: 背景交给 QStyledItemDelegate 照常画,
// 勾选框挪到格子正中。
// 点击也要一起接管 —— 默认的 editorEvent 只在原来那个靠左的小方块范围内才响应,
// 否则会出现"看着在中间, 点它却没反应"。
class CenteredCheckDelegate : public QStyledItemDelegate
{
public:
    explicit CenteredCheckDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const QVariant v = index.data(Qt::CheckStateRole);
        if (!v.isValid()) {            // 名字那一列没有勾选框, 照常画
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        // 勾选格只画背景 (交替行色、边框都在 PE_PanelItemViewItem 里),
        // 不走 QStyledItemDelegate::paint —— 它会把默认的勾选框一起画出来,
        // 清 HasCheckIndicator 也拦不住 (QSS 生效时由 QStyleSheetStyle 决定),
        // 结果是格子里并排出现两个方框。
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        QStyle *st = option.widget ? option.widget->style() : QApplication::style();
        st->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, option.widget);

        const int side = 14;   // 与 style.cpp 里 QTableWidget::indicator 的宽高一致
        opt.rect = QRect(option.rect.center().x() - side / 2,
                         option.rect.center().y() - side / 2, side, side);
        opt.state &= ~(QStyle::State_On | QStyle::State_Off);
        opt.state |= (v.toInt() == Qt::Checked) ? QStyle::State_On : QStyle::State_Off;
        st->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &opt, painter, option.widget);
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &, const QModelIndex &index) override
    {
        // 在"按下"时就翻, 不在"抬起"时翻: 双击的事件序列是
        // 按下 / 抬起 / 双击 / 抬起, 若在抬起时翻就会翻两次, 双击等于没点。
        // 按下时翻一次, 再把双击吞掉, 双击的结果就是"勾上"。
        if (event->type() == QEvent::MouseButtonDblClick) return true;
        if (event->type() != QEvent::MouseButtonPress) return false;
        const bool on = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        return model->setData(index, on ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
    }
};

} // namespace

CondDialog::CondDialog(int scene, QWidget *parent)
    : QDialog(parent), m_scene(scene)
{
    setWindowTitle(QString::fromUtf8("编辑出怪条件"));
    // 去掉标题栏上的"这是什么?"按钮：程序没有设置任何 whatsThis 文本, 点它只会
    // 进入上下文帮助模式却没有内容可显示, 看起来像失灵
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    // 宽度不在这里写死: 由 rebuildTable() 按"名字列 + 四个勾选列"的实际
    // 列宽算出来。早前写死 320, 但内容是 76 + 4×46 = 260, 加边框边距约 282,
    // 右边就多出一条二十几像素的灰带 —— 观感上像是窗口没对齐。
    buildUi();
    rebuildTable();
}

void CondDialog::buildUi()
{
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

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
    // 僵尸名居中, 名字一列定宽 (四个勾选列各 46), 窗口因此能收到很窄
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->setColumnWidth(0, 76);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    for (int c = 1; c < 5; ++c) {
        m_table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Fixed);
        m_table->setColumnWidth(c, 46);
    }
    m_table->verticalHeader()->setDefaultSectionSize(kRowH);
    // 表头也要压: QSS 里 QHeaderView::section 有 5px 上下内边距, 不设死高度的话
    // 光表头就占 30px。这里定死 22, 换行不了也无所谓 (列头都是两个字)。
    m_table->horizontalHeader()->setFixedHeight(kHeadH);
    // 勾选框自己居中画 (见上面的 CenteredCheckDelegate)
    m_table->setItemDelegate(new CenteredCheckDelegate(m_table));
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
    // 文案压到最短: 「至少出」 [2] 种　「至多出」 [2] 种, 整行才放得进 320 宽的窗口
    QHBoxLayout *cnt = new QHBoxLayout;
    cnt->setSpacing(4);

    m_atLeastLabel = new QLabel(QString::fromUtf8("「至少出」"));
    cnt->addWidget(m_atLeastLabel);
    m_atLeastSpin = new QSpinBox(this);
    m_atLeastSpin->setRange(0, 19);
    m_atLeastSpin->setFixedWidth(46);
    cnt->addWidget(m_atLeastSpin);
    cnt->addWidget(new QLabel(QString::fromUtf8("种")));

    cnt->addSpacing(8);

    m_atMostLabel = new QLabel(QString::fromUtf8("「至多出」"));
    cnt->addWidget(m_atMostLabel);
    m_atMostSpin = new QSpinBox(this);
    m_atMostSpin->setRange(0, 19);
    m_atMostSpin->setFixedWidth(46);
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
    const int rowsH = kHeadH + m_rows.size() * kRowH + m_table->frameWidth() * 2 + 2;
    m_table->setFixedHeight(rowsH);
    adjustSize();

    // 宽度同样按内容算, 不再写死: 表格各列总宽 + 边框 + 左右边距(=布局的
    // contentsMargins), 这样右边不会多出一条灰带。滚动条宽度也加进去 ——
    // 某些场景行数多到出滚动条时, 不然会刚好差那么十来个像素。
    int colsW = 0;
    for (int c = 0; c < m_table->columnCount(); ++c) colsW += m_table->columnWidth(c);
    const QMargins m = layout()->contentsMargins();
    int width = colsW + m_table->frameWidth() * 2 + m.left() + m.right();
    if (m_table->verticalScrollBar()->isVisible())
        width += m_table->verticalScrollBar()->sizeHint().width();
    setFixedWidth(width);

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
