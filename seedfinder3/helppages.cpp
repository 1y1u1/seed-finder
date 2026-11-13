#include "helppages.h"

#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {

QFrame *card(const QString &title, const QString &body)
{
    QFrame *f = new QFrame;
    f->setObjectName("Card");
    QVBoxLayout *lay = new QVBoxLayout(f);
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(6);

    QLabel *t = new QLabel(title);
    t->setObjectName("CardTitle");
    lay->addWidget(t);

    QLabel *b = new QLabel(body);
    b->setObjectName("Stat");
    b->setWordWrap(true);
    b->setTextFormat(Qt::RichText);
    lay->addWidget(b);
    return f;
}

QString head(const QString &title, const QString &sub)
{
    return QString::fromUtf8(
               "<div style='font-size:17px;font-weight:700;color:#111111;'>%1</div>"
               "<div style='color:#7a7a7a;font-size:11px;margin-top:2px;'>%2</div>")
        .arg(title, sub);
}

void buildShell(QDialog *dlg, const QString &titleText, QWidget *content,
                const QSize &size, const QString &okText)
{
    dlg->setWindowTitle(titleText);
    dlg->setWindowFlags(dlg->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg->setMinimumSize(size);

    QVBoxLayout *root = new QVBoxLayout(dlg);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    QScrollArea *sa = new QScrollArea(dlg);
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setWidget(content);
    root->addWidget(sa, 1);

    QHBoxLayout *btns = new QHBoxLayout;
    btns->addStretch(1);
    QPushButton *ok = new QPushButton(okText, dlg);
    ok->setObjectName("Primary");
    ok->setDefault(true);
    ok->setMinimumWidth(96);
    QObject::connect(ok, &QPushButton::clicked, dlg, &QDialog::accept);
    btns->addWidget(ok);
    root->addLayout(btns);
}

} // namespace

void showHowto(QWidget *parent)
{
    QDialog *dlg = buildHowto(parent);
    dlg->exec();
    dlg->deleteLater();
}

QDialog *buildHowto(QWidget *parent)
{
    QDialog *dlg = new QDialog(parent);
    QWidget *content = new QWidget;
    QVBoxLayout *lay = new QVBoxLayout(content);
    lay->setContentsMargins(0, 0, 6, 0);
    lay->setSpacing(10);

    QLabel *title = new QLabel;
    title->setTextFormat(Qt::RichText);
    title->setText(head(QString::fromUtf8("使用说明"),
                        QString::fromUtf8("三步找到想要的种子 · 所有条件都能自由组合")));
    lay->addWidget(title);

    lay->addWidget(card(
        QString::fromUtf8("① 填存档信息"),
        QString::fromUtf8(
            "「用户编号」是游戏里的用户 ID，「存档编号」是游戏模式（冒险/生存等），"
            "两者一起决定随机数偏移，填错会得到完全不同的结果。<br>"
            "「场景」决定哪些僵尸可能出现：例如冰车只在黑夜前院不出现，"
            "蹦极/扶梯/投篮只在屋顶系出现。切换场景后，出怪条件里可选僵尸会跟着变。")));

    lay->addWidget(card(
        QString::fromUtf8("② 加出怪条件（决定排名）"),
        QString::fromUtf8(
            "不用事先划区间：每加一条出怪条件，就在对话框里填它自己的 F 数范围，"
            "扫描范围会取所有条件覆盖范围的并集。<br>"
            "一条出怪条件 = 一段区间 + 四组具体要求，可以加任意多条，各自负责一段。<br>"
            "<b>必出</b>：勾选的僵尸必须<b>全部</b>在这一轮出现。<br>"
            "<b>禁出</b>：勾选的僵尸<b>一个都不能</b>出现。<br>"
            "<b>至少出</b>：勾选的这几种里，实际出现种类数 <b>≥</b> 你填的数量。<br>"
            "<b>至多出</b>：勾选的这几种里，实际出现种类数 <b>≤</b> 你填的数量。<br>"
            "同一只僵尸只能属于其中一组；四组全空表示这一段不做限制。<br>"
            "出怪条件<b>不淘汰</b>种子，只用来排名：满足的轮数越多，种子排得越前。")));

    lay->addWidget(card(
        QString::fromUtf8("③ 加全局条件（硬性要求）"),
        QString::fromUtf8(
            "和出怪条件一样的输入方式（一段区间 + 四组条件），但语义完全不同：<br>"
            "<b>这一段必须每一轮都满足</b>，只要有一轮不满足，这个种子<b>直接淘汰</b>，不会出现在结果里。<br>"
            "全局条件可以不加；加了就必须全部满足。")));

    lay->addWidget(card(
        QString::fromUtf8("④ 怎么算"),
        QString::fromUtf8(
            "点「开始查找」后，程序从 0 一直扫到 <b>FF FFFF FF</b>，把全部 "
            "4,294,967,296 个种子都过一遍，不需要你设数量。<br>"
            "扫描过程中，「输出种子数」下面是<b>进度条</b>和预估剩余时间；"
            "点「暂停」可以原地停下，按钮变成「继续」后接着算，暂停时间不计入预估。<br>"
            "先按全局条件淘汰，剩下的种子按「满足出怪条件的轮数」从多到少排名；"
            "一轮都没满足的种子不会列出，如果全都不满足会弹窗提示最好的一档。<br>"
            "「输出种子数」控制结果表总共列多少个种子（从满足轮数最多的档位往下取），其余只统计个数。")));

    lay->addWidget(card(
        QString::fromUtf8("⑤ 看结果"),
        QString::fromUtf8(
            "结果表的「满足轮数」是「满足的轮数 / 有出怪条件的轮数」，全部满足的行会加粗；"
            "点一行，双击会把它的逐轮出怪表弹到最前。黑白配色下用<b>字形</b>区分条件："
            "<b>●</b> 要求出现（必出 / 至少出）、<b>✕</b> 要求不出现（禁出 / 至多出）、"
            "<b>○</b> 出了但没要求、空白 没出；"
            "没满足的轮整行灰底。出怪表里<b>不显示普僵</b>（每轮必出，列出来没有信息量）。<br>"
            "「已完成 F 数」列就是 1.2 版的叫法：数字本身对应的轮次 = 数字 / 2 + 1。<br>"
            "右上角的<b>种子输入框</b>可以直接填一个种子（16 进制），点「查看出怪」就能看它在条件覆盖范围内的"
            "出怪表，不用先查找。")));

    lay->addWidget(card(
        QString::fromUtf8("小提示"),
        QString::fromUtf8(
            "· 红眼最早只能出现在第 6 轮（F 11 起），冰车 / 白眼最早第 3 轮（F 5 起），"
            "区间划得太靠前必然查不到。<br>"
            "· 普僵每一轮都必定出现，所以条件表里没有它 —— 设它没有意义。<br>"
            "· 路障和读报二者必出其一，所以第 1 轮至少会有其中一个。<br>"
            "· 第 n 轮只能新增 n 种僵尸（第 2 轮 2 个名额、第 3 轮 3 个……第 9 轮起 9 个），"
            "条件是这一段<b>每一轮</b>都要成立，所以名额按区间<b>最早</b>的那一轮算："
            "比如「3 到 10 F 必出 3 种」会卡在第 2 轮（只有 2 个名额）而查不到。<br>"
            "· 条件加得越死，越可能一个都不满足；程序会提示最好的一档，据此逐步放宽。<br>"
            "· 想留档就把参数存成预设，下次直接载入。")));

    lay->addStretch(1);
    buildShell(dlg, QString::fromUtf8("使用说明"), content, QSize(720, 700),
               QString::fromUtf8("知道了"));
    return dlg;
}

void showAbout(QWidget *parent)
{
    QDialog *dlg = buildAbout(parent);
    dlg->exec();
    dlg->deleteLater();
}

QDialog *buildAbout(QWidget *parent)
{
    QDialog *dlg = new QDialog(parent);
    QWidget *content = new QWidget;
    QVBoxLayout *lay = new QVBoxLayout(content);
    lay->setContentsMargins(0, 0, 6, 0);
    lay->setSpacing(10);

    QLabel *title = new QLabel;
    title->setTextFormat(Qt::RichText);
    title->setText(head(QString::fromUtf8("植物大战僵尸 · 无尽种子查找器"),
                        QString::fromUtf8("v3.0 · Qt 5.12.9 静态构建 · 零外部依赖")));
    lay->addWidget(title);

    lay->addWidget(card(
        QString::fromUtf8("这是什么"),
        QString::fromUtf8(
            "给《植物大战僵尸》无尽模式用的种子查找器。它把「第几 F 必须出什么僵尸」"
            "这类要求写成规则，反查满足要求的种子，省掉手动重开几十次的时间。<br>"
            "相比 v2.0，这一版把 F 数区间和出怪条件都做成了可自由组合的形式："
            "多段区间、每段各自的必出/禁出/至少/至多、外加跨整段的全局轮次约束。")));

    lay->addWidget(card(
        QString::fromUtf8("引擎"),
        QString::fromUtf8(
            "逐位复刻游戏里的 MT19937 选卡流程（含读报/路障二选一、红眼与白眼的前移规则、"
            "按场景过滤的僵尸表），并对原版 seed-finder 的分页实现做了独立交叉验证。<br>"
            "这一版取消了 1024 种子一页的预计算缓存，改为按需直算，"
            "100 万个种子 × 25 轮约 0.1 ~ 0.4 秒，靠多线程铺满所有核心。<br>"
            "全量 42.9 亿个种子耗时取决于条件覆盖多少轮：只查 1 轮约 1 ~ 2 分钟，"
            "25 轮全查约 25 ~ 30 分钟。建议先用「停止」快速试几秒，看命中率是否合理。")));

    lay->addWidget(card(
        QString::fromUtf8("致谢"),
        QString::fromUtf8(
            "算法思路来自 Crescendo 的 <b>seed-finder-qt</b>，以及 Kndy666 的 "
            "<b>zombie_calculator</b>。本版为界面与引擎重写，出怪判定与原版保持一致。")));

    lay->addStretch(1);
    buildShell(dlg, QString::fromUtf8("关于"), content, QSize(620, 480),
               QString::fromUtf8("关闭"));
    return dlg;
}
