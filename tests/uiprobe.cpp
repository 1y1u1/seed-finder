// 界面布局探针: 在无头平台上把主窗口按几种尺寸摆一遍, 打印关键控件的几何信息。
//
// 用途:
//   1) 确认条件表的竖向滚动条有没有压住表头那一行;
//   2) 确认窗口纵向放大时两张条件卡是不是把多出来的高度平分掉;
//   3) 确认窗口横向放大时卡片宽度不变、多出的宽度只进两侧边距;
//   4) 打印"刚好放下 99 / MGE/AQE"所需的宽度, 给左栏收窄当依据。
//
// 用法: uiprobe.exe [输出文件]
//   不带参数时只打印到 stdout。

#include "mainwindow.h"
#include "style.h"

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>

namespace {

QTextStream *g_out = nullptr;

void line(const QString &s)
{
    if (g_out) *g_out << s << "\n";
}

QString rectText(const QRect &r)
{
    return QString("x=%1 y=%2 w=%3 h=%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

// 条件表: 竖向滚动条是否压在表头区域上
void reportTable(const QString &name, QTableWidget *t)
{
    if (!t) {
        line(QString("  %1: <null>").arg(name));
        return;
    }
    QScrollBar *vb = t->verticalScrollBar();
    // geometry() 是相对父控件的, 统一换算到表格自身的坐标系才不会误判
    const QRect vbRect(vb->mapTo(t, QPoint(0, 0)), vb->size());
    const QRect hhRect(t->horizontalHeader()->mapTo(t, QPoint(0, 0)),
                       t->horizontalHeader()->size());
    const QRect vpRect(t->viewport()->mapTo(t, QPoint(0, 0)), t->viewport()->size());
    const bool overlap = vb->isVisible() && vbRect.top() < hhRect.bottom();
    line(QString("  %1: table=%2 viewport=%3 header=%4 vbar=%5 visible=%6 overlapHeader=%7")
             .arg(name)
             .arg(rectText(t->geometry()))
             .arg(rectText(vpRect))
             .arg(rectText(hhRect))
             .arg(rectText(vbRect))
             .arg(vb->isVisible() ? "yes" : "no")
             .arg(overlap ? "YES" : "no"));
}

void reportWindow(MainWindow *w, const QString &tag)
{
    line(QString("--- %1: window %2x%3 ---").arg(tag).arg(w->width()).arg(w->height()));

    const QList<QTableWidget *> tables = w->findChildren<QTableWidget *>();
    for (int i = 0; i < tables.size(); ++i) {
        QTableWidget *t = tables.at(i);
        QString name = t->objectName();
        if (name.isEmpty()) {
            // 用列头文字当名字
            name = t->columnCount() > 0 && t->horizontalHeaderItem(0)
                       ? t->horizontalHeaderItem(0)->text()
                       : QString("table%1").arg(i);
        }
        reportTable(name, t);
    }

    const QList<QScrollArea *> areas = w->findChildren<QScrollArea *>();
    for (int i = 0; i < areas.size(); ++i) {
        line(QString("  scrollArea%1: %2 hbarVisible=%3")
                 .arg(i)
                 .arg(rectText(areas.at(i)->geometry()))
                 .arg(areas.at(i)->horizontalScrollBar()->isVisible() ? "yes" : "no"));
    }

    // 卡片: 用 Card 这个 objectName 找
    const QList<QWidget *> cards = w->findChildren<QWidget *>();
    int cardIdx = 0;
    for (int i = 0; i < cards.size(); ++i) {
        QWidget *c = cards.at(i);
        if (c->objectName() != QLatin1String("Card")) continue;
        line(QString("  card#%1: %2").arg(cardIdx++).arg(rectText(c->geometry())));
    }
}

// 左栏收窄的依据: 各种控件"刚好放下"需要的宽度
void reportWidths(MainWindow *w)
{
    line("--- 控件宽度 ---");
    const QList<QComboBox *> combos = w->findChildren<QComboBox *>();
    for (int i = 0; i < combos.size(); ++i) {
        QComboBox *c = combos.at(i);
        line(QString("  combo#%1: current=%2 sizeHint=%3 minimumSizeHint=%4 geometry=%5")
                 .arg(i)
                 .arg(c->currentText())
                 .arg(c->sizeHint().width())
                 .arg(c->minimumSizeHint().width())
                 .arg(rectText(c->geometry())));
    }
    const QList<QSpinBox *> spins = w->findChildren<QSpinBox *>();
    for (int i = 0; i < spins.size(); ++i) {
        QSpinBox *s = spins.at(i);
        line(QString("  spin#%1: value=%2 sizeHint=%3 minimumSizeHint=%4 geometry=%5")
                 .arg(i)
                 .arg(s->value())
                 .arg(s->sizeHint().width())
                 .arg(s->minimumSizeHint().width())
                 .arg(rectText(s->geometry())));
    }
    const QList<QLabel *> labels = w->findChildren<QLabel *>();
    int labelHint = 0;
    for (int i = 0; i < labels.size(); ++i) {
        if (labels.at(i)->text().isEmpty()) continue;
        labelHint = qMax(labelHint, labels.at(i)->sizeHint().width());
    }
    line(QString("  最宽 QLabel sizeHint = %1").arg(labelHint));
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 与 main.cpp 保持一致: 同样的字体和样式表, 否则量出来的宽度没意义
    QFont f(QString::fromUtf8("Microsoft YaHei UI"), 9);
    app.setFont(f);
    app.setStyleSheet(appStyleSheet());

    QFile file;
    QTextStream stream(stdout);
    if (argc > 1) {
        file.setFileName(QString::fromLocal8Bit(argv[1]));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            stream.setDevice(&file);
        }
    }
    // 控制台/文件统一 UTF-8: 默认按系统代码页写, 中文读出来是乱码
    stream.setCodec("UTF-8");
    g_out = &stream;

    MainWindow w;
    w.loadDemo();
    w.show();

    const QSize sizes[] = { QSize(672, 464), QSize(900, 700), QSize(1400, 1000) };
    for (const QSize &s : sizes) {
        w.resize(s);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        reportWindow(&w, QString("%1x%2").arg(s.width()).arg(s.height()));
    }

    reportWidths(&w);

    g_out = nullptr;
    if (file.isOpen()) file.close();
    return 0;
}
