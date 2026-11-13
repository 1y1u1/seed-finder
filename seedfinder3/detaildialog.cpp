#include "detaildialog.h"

#include "detailview.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMargins>
#include <QPushButton>
#include <QVBoxLayout>

DetailDialog::DetailDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QString::fromUtf8("出怪详情"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    // 最小尺寸不再写死: 表格各列都是定宽, 窗口宽度在 setData() 里按内容收
    setMinimumSize(420, 420);

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(9);

    m_title = new QLabel(this);
    m_title->setObjectName("CardTitle");
    root->addWidget(m_title);

    m_view = new DetailView(this);
    root->addWidget(m_view, 1);

    QHBoxLayout *btns = new QHBoxLayout;
    btns->addStretch(1);
    QPushButton *close = new QPushButton(QString::fromUtf8("关闭"), this);
    close->setObjectName("Primary");
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    btns->addWidget(close);
    root->addLayout(btns);
}

void DetailDialog::clear()
{
    m_title->setText(QString());
    m_view->clear();
}

void DetailDialog::setData(quint32 seed, int scene,
                           const std::vector<sf::DetailRow> &rows,
                           const sf::RuleTable &rank, const sf::RuleTable &hard)
{
    QString sceneName = QString::number(scene);
    const QVector<int> ids = sceneIds();
    const int idx = ids.indexOf(scene);
    if (idx >= 0) sceneName = sceneDisplayNames().value(idx, sceneName);

    m_title->setText(QString::fromUtf8("种子 %1　·　%2")
                         .arg(QString::number(seed, 16).toUpper().rightJustified(8, QLatin1Char('0')))
                         .arg(sceneName));
    m_view->setData(seed, scene, rows, rank, hard);

    // 窗口收到刚好放得下表格的宽度, 右侧不留白。
    // 内容变化时只在"当前宽度不够"的情况下放, 免得用户手动拉宽后又被缩回去。
    // 多留 16px: 表头分界线与布局间隙不计入 columnWidth, 算得太紧就会蹦出横向滚动条。
    const QMargins mg = layout()->contentsMargins();
    const int needW = m_view->preferredWidth() + mg.left() + mg.right() + 16;
    if (width() < needW) resize(needW, height());
}
