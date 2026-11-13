#include "banddialog.h"
#include "conddialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

BandDialog::BandDialog(int scene, int outerBegin, int outerEnd, QWidget *parent)
    : QDialog(parent), m_scene(scene), m_outerBegin(outerBegin), m_outerEnd(outerEnd)
{
    // 内部边界是偶数 F 数: beginFlag 对应"起始轮 - 1", endFlag 对应"终止轮"
    // 范围最早只能从 3 F 开始 (第 1 ~ 2 轮固定是普僵+路障/读报, 没有查询意义)
    if (m_outerBegin < 2) m_outerBegin = 2;
    if (m_outerEnd - m_outerBegin < 2) m_outerEnd = m_outerBegin + 2;

    setWindowTitle(QString::fromUtf8("编辑出怪条件"));
    // 去掉标题栏上的"这是什么?"按钮：点它只会进入上下文帮助模式, 但没有内容可显示
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setMinimumWidth(560);
    buildUi();
    refreshSummary();
}

void BandDialog::buildUi()
{
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    m_titleRange = new QLabel(QString::fromUtf8("范围"));
    m_titleRange->setObjectName("CardTitle");
    root->addWidget(m_titleRange);

    // 起始 F 取奇数 / 终止 F 取偶数 —— 与出怪计算器一致。
    // 两个框都放开到完整范围, 输入过程中不改写用户敲的数
    // (以前在 valueChanged 里"顺手纠正"会在敲 4 时把前面的框改成 3,
    //  用户想输 40 就永远输不进去); 奇偶和先后顺序统一在点"确定"时处理。
    const int firstFlag = m_outerBegin + 1;       // 奇数
    const int lastFlag = m_outerEnd;              // 偶数

    // 最小值是"还没填"的哨兵值, 用 setSpecialValueText 显示成"—"。
    // 新建条件时两个框都停在这里, 点"确定"会要求先填范围 —— 以前这里
    // 自动填 3~12, 用户不留神就带着一个自己没想过的区间去查,
    // 查不出东西也不知道是为什么。
    // 哨兵取 firstFlag 前一档, 这样上下键的步进仍然落在正确奇偶上:
    // 起始框 — → 3 → 5 …, 终止框 — → 4 → 6 …
    QHBoxLayout *range = new QHBoxLayout;
    range->setSpacing(8);
    m_begin = new QSpinBox(this);
    m_begin->setRange(firstFlag - 2, lastFlag);
    m_begin->setSpecialValueText(QString::fromUtf8("—"));
    m_begin->setSingleStep(2);
    m_begin->setFixedWidth(96);
    range->addWidget(m_begin);
    range->addWidget(new QLabel(QString::fromUtf8("到")));
    m_end = new QSpinBox(this);
    m_end->setRange(firstFlag - 1, lastFlag);
    m_end->setSpecialValueText(QString::fromUtf8("—"));
    m_end->setSingleStep(2);
    m_end->setFixedWidth(96);
    range->addWidget(m_end);
    range->addWidget(new QLabel(QString::fromUtf8("F")));
    range->addStretch(1);
    root->addLayout(range);

    m_titleCond = new QLabel(QString::fromUtf8("出怪条件"));
    m_titleCond->setObjectName("CardTitle");
    root->addWidget(m_titleCond);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setMinimumHeight(48);
    m_summary->setStyleSheet(QString::fromUtf8(
        "background:#f7f7f7; border:1px solid #dcdcdc; border-radius:0; padding:10px;"));
    root->addWidget(m_summary);

    m_editBtn = new QPushButton(QString::fromUtf8("设置出怪条件…"), this);
    connect(m_editBtn, &QPushButton::clicked, this, &BandDialog::onEditCondition);
    QHBoxLayout *mid = new QHBoxLayout;
    mid->addWidget(m_editBtn);
    mid->addStretch(1);
    root->addLayout(mid);

    root->addStretch(1);

    QHBoxLayout *btns = new QHBoxLayout;
    btns->addStretch(1);
    QPushButton *cancel = new QPushButton(QString::fromUtf8("取消"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    btns->addWidget(cancel);
    QPushButton *ok = new QPushButton(QString::fromUtf8("确定"), this);
    ok->setObjectName("Primary");
    ok->setDefault(true);
    connect(ok, &QPushButton::clicked, this, &BandDialog::onAccept);
    btns->addWidget(ok);
    root->addLayout(btns);

    // 两个框都停在哨兵值 ("—"), 由 setBand() 决定是否填入已有区间
    m_begin->setValue(m_begin->minimum());
    m_end->setValue(m_end->minimum());
}

void BandDialog::setHardMode(bool hard)
{
    m_hard = hard;
    // 除窗口标题外, 其余文案与「出怪条件」完全一致 —— 两张表的条件语义本来就相同,
    // 只是用途不同(全局条件再叠一层"每轮都要满足"的淘汰)。校验提示里仍会区分
    // "全局条件"/"出怪条件", 因为报错时要指明是哪一张表的条件。
    setWindowTitle(hard ? QString::fromUtf8("编辑全局条件")
                        : QString::fromUtf8("编辑出怪条件"));
}

void BandDialog::setBand(const sf::RuleBand &band)
{
    m_cond = band.cond;
    m_cond.sanitize();

    const int beginRound = band.beginFlag + 1;
    const int endRound = band.endFlag;
    m_begin->setValue(std::max(m_outerBegin + 1, beginRound | 1));    // 夹到奇数
    m_end->setValue(std::min(m_outerEnd, std::max(m_begin->value() + 1, endRound & ~1)));

    refreshSummary();
}

sf::RuleBand BandDialog::band() const
{
    sf::RuleBand b;
    b.beginFlag = m_begin->value() - 1;   // 奇数轮 -> 偶数 F 边界
    b.endFlag = m_end->value();
    b.cond = m_cond;
    return b;
}

void BandDialog::onEditCondition()
{
    CondDialog dlg(m_scene, this);
    dlg.setCondition(m_cond);
    if (dlg.exec() == QDialog::Accepted) {
        m_cond = dlg.condition();
        refreshSummary();
    }
}

void BandDialog::refreshSummary()
{
    if (m_cond.isEmpty()) {
        m_summary->setText(QString());
        return;
    }
    m_summary->setText(m_cond.summary());
}

void BandDialog::onAccept()
{
    const int sentinelBegin = m_begin->minimum();
    const int sentinelEnd = m_end->minimum();

    // 1) 范围必须先填。新建条件时两个框都是"—", 不留默认值替用户做主。
    if (m_begin->value() == sentinelBegin || m_end->value() == sentinelEnd) {
        QMessageBox::warning(this, QString::fromUtf8("范围未填写"),
                             QString::fromUtf8("请先填写要检查的 F 数范围。"));
        (m_begin->value() == sentinelBegin ? m_begin : m_end)->setFocus();
        return;
    }

    // 2) 归一化奇偶: 起始取奇数、终止取偶数。输入过程中不改写, 到这里一次纠正,
    //    并把改后的值写回框里, 让用户看得见最终存的是什么。
    const int rawBegin = m_begin->value();
    const int rawEnd = m_end->value();
    const int fixedBegin = (rawBegin & 1) ? rawBegin : rawBegin + 1;   // 向上取奇
    const int fixedEnd = (rawEnd & 1) ? rawEnd - 1 : rawEnd;           // 向下取偶
    if (fixedBegin != rawBegin || fixedEnd != rawEnd) {
        m_begin->setValue(std::min(fixedBegin, m_begin->maximum()));
        m_end->setValue(std::max(fixedEnd, std::max(m_end->minimum(), sentinelEnd)));
        QMessageBox::information(
            this, QString::fromUtf8("F 数已调整"),
            QString::fromUtf8("F 数只支持「起始取奇数、终止取偶数」，已改为 %1 到 %2 F。")
                .arg(m_begin->value()).arg(m_end->value()));
    }

    // 3) 先后顺序
    if (m_end->value() <= m_begin->value()) {
        QMessageBox::warning(this, QString::fromUtf8("范围无效"),
                             QString::fromUtf8("终止 F 数必须大于起始 F 数。"));
        m_end->setFocus();
        return;
    }

    // 4) 没设置任何出怪要求就等于不限制, 直接拦下 —— 这种条件留在表里
    //    只会让人以为约束生效了, 实际上什么都没过滤。
    if (m_cond.isEmpty()) {
        QMessageBox::warning(
            this, QString::fromUtf8("没有设置出怪条件"),
            m_hard ? QString::fromUtf8("这条全局条件没有设置任何出怪要求，请先设置条件。")
                   : QString::fromUtf8("这条出怪条件没有设置任何出怪要求，请先设置条件。"));
        m_editBtn->setFocus();
        return;
    }

    // 5) 条件本身能否满足 (与"开始查找"用的是同一套规则, 提前到这里提示)
    const sf::RuleBand b = band();
    const QString kind = m_hard ? QString::fromUtf8("全局条件") : QString::fromUtf8("出怪条件");
    const QString bad = sf::checkBand(b, m_scene, kind, m_siblings);
    if (!bad.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("条件无法满足"), bad);
        return;
    }

    accept();
}

void BandDialog::setSiblings(const QVector<sf::RuleBand> &siblings, bool hard)
{
    Q_UNUSED(hard)
    m_siblings = siblings;
}
