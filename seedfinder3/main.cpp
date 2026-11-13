#include "banddialog.h"
#include "conddialog.h"
#include "detaildialog.h"
#include "helppages.h"
#include "mainwindow.h"
#include "presetstore.h"
#include "seedengine.h"
#include "style.h"
#include "zombiedata.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFrame>
#include <QScrollArea>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <cstdio>

int main(int argc, char *argv[])
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
#endif

    QApplication a(argc, argv);
    a.setApplicationName(QString::fromUtf8("种子查找器"));
    a.setOrganizationName(QString::fromUtf8("PvZ Seed Finder"));

    // 跨线程信号必须注册元类型, 否则 queued connection 会静默丢弃 SearchResult
    qRegisterMetaType<sf::SearchResult>("sf::SearchResult");

    QFont font(QString::fromUtf8("Microsoft YaHei UI"), 9);
    a.setFont(font);
    a.setStyleSheet(appStyleSheet());

    // 命令行辅助: --selftest 只跑引擎交叉校验; --shot <png> 截图后退出
    const QStringList args = a.arguments();
    if (args.contains(QStringLiteral("--selftest"))) {
        const QString err = sf::Searcher::selfCheck();
        std::printf("%s\n", err.isEmpty() ? "selfCheck: OK"
                                          : err.toUtf8().constData());
        return err.isEmpty() ? 0 : 1;
    }

    // --presettest : 预设存盘/读取的往返自检
    if (args.contains(QStringLiteral("--presettest"))) {
        // 自检不能碰用户真实的 presets.json (原来的实现会往里追加一条"自检预设"),
        // 用环境变量把路径重定向到临时目录。
        const QString tmpPath = QDir(QDir::tempPath())
                                    .filePath(QStringLiteral("seedfinder3-presettest.json"));
        QFile::remove(tmpPath);
        qputenv("SEEDFINDER3_PRESETS", tmpPath.toLocal8Bit());

        Preset p;
        p.name = QString::fromUtf8("自检预设");
        p.uid = 7;
        p.mode = 21;
        p.scene = 4;
        p.beginFlag = 12;
        p.endFlag = 64;
        p.outputSize = 9;

        sf::RuleBand b;
        b.beginFlag = 12;
        b.endFlag = 30;
        b.cond.needMask = (1u << Z_GIGA) | (1u << Z_GARG);
        b.cond.refuseMask = (1u << Z_JACK);
        b.cond.atLeastMask = (1u << Z_BUCKET) | (1u << Z_DOOR) | (1u << Z_FOOTBALL);
        b.cond.atLeast = 2;
        b.cond.atMostMask = (1u << Z_BUNGEE) | (1u << Z_LADDER);
        b.cond.atMost = 1;
        b.cond.sanitize();
        p.bands << b;

        sf::RuleBand h;
        h.beginFlag = 12;
        h.endFlag = 20;
        h.cond.needMask = (1u << Z_POLE) | (1u << Z_NEWS);
        h.cond.sanitize();
        p.hard << h;

        QString err;
        QVector<Preset> list = loadPresets(&err);   // 保留用户已有的预设
        list << p;
        if (!savePresets(list, &err)) {
            std::printf("presettest: save failed: %s\n", err.toUtf8().constData());
            return 1;
        }

        const QVector<Preset> back = loadPresets(&err);
        if (back.isEmpty()) {
            std::printf("presettest: load failed: %s\n", err.toUtf8().constData());
            return 1;
        }
        const Preset &q = back.last();
        const bool same = q.name == p.name && q.uid == p.uid && q.mode == p.mode
                          && q.scene == p.scene && q.beginFlag == p.beginFlag
                          && q.endFlag == p.endFlag
                          && q.outputSize == p.outputSize
                          && q.bands.size() == 1 && q.bands[0].cond == p.bands[0].cond
                          && q.bands[0].beginFlag == 12 && q.bands[0].endFlag == 30
                          && q.hard.size() == 1 && q.hard[0].cond == p.hard[0].cond
                          && q.hard[0].beginFlag == 12 && q.hard[0].endFlag == 20;
        std::printf("presettest: %s  (%s)\n", same ? "OK" : "MISMATCH",
                    presetFilePath().toUtf8().constData());
        return same ? 0 : 1;
    }

    // --dialog howto|about [--shot out.png] : 单独截一个信息窗口, 用于检查排版
    if (args.contains(QStringLiteral("--dialog"))) {
        const int di = args.indexOf(QStringLiteral("--dialog"));
        const QString which = (di + 1 < args.size()) ? args.at(di + 1) : QString();
        const int si = args.indexOf(QStringLiteral("--shot"));
        const QString path = (si >= 0 && si + 1 < args.size()) ? args.at(si + 1) : QString();

        MainWindow host;
        host.resize(200, 100);

        QDialog *dlg = nullptr;
        if (which == QLatin1String("howto")) {
            dlg = buildHowto(&host);
        } else if (which == QLatin1String("about")) {
            dlg = buildAbout(&host);
        } else if (which == QLatin1String("cond")) {
            CondDialog *cd = new CondDialog(2, &host);
            sf::WaveCondition demo;
            demo.needMask = (1u << Z_GIGA);
            demo.refuseMask = (1u << Z_JACK);
            demo.atLeastMask = (1u << Z_BUCKET) | (1u << Z_DOOR) | (1u << Z_FOOTBALL);
            demo.atLeast = 2;
            demo.atMostMask = (1u << Z_BUNGEE) | (1u << Z_LADDER) | (1u << Z_CATAPULT);
            demo.atMost = 2;
            demo.sanitize();
            cd->setCondition(demo);
            dlg = cd;
        } else if (which == QLatin1String("global")) {
            BandDialog *bd = new BandDialog(2, 2, 50, &host);
            bd->setHardMode(true);
            sf::RuleBand h;
            h.beginFlag = 10;
            h.endFlag = 22;
            h.cond.needMask = (1u << Z_POLE) | (1u << Z_NEWS);
            h.cond.sanitize();
            bd->setBand(h);
            dlg = bd;
        } else if (which == QLatin1String("band")) {
            BandDialog *bd = new BandDialog(2, 2, 50, &host);
            sf::RuleBand b;
            b.beginFlag = 10;
            b.endFlag = 22;
            b.cond.needMask = (1u << Z_GIGA);
            b.cond.refuseMask = (1u << Z_JACK);
            b.cond.sanitize();
            bd->setBand(b);
            dlg = bd;
        } else if (which == QLatin1String("detail")) {
            // 单种子详情弹窗: 用演示规则带跑一次真实的逐轮数据
            sf::RuleBand b1;
            b1.beginFlag = 10;
            b1.endFlag = 12;
            b1.cond.needMask = (1u << Z_GIGA);
            b1.cond.sanitize();
            sf::RuleBand b2;
            b2.beginFlag = 30;
            b2.endFlag = 32;
            b2.cond.atLeastMask = (1u << Z_BUCKET) | (1u << Z_DOOR) | (1u << Z_FOOTBALL);
            b2.cond.atLeast = 2;
            b2.cond.sanitize();
            QVector<sf::RuleBand> demoBands;
            demoBands << b1 << b2;

            sf::RuleBand h1;
            h1.beginFlag = 20;
            h1.endFlag = 22;
            h1.cond.needMask = (1u << Z_GARG);
            h1.cond.sanitize();
            QVector<sf::RuleBand> demoHard;
            demoHard << h1;

            sf::RuleTable rankTable;
            rankTable.build(demoBands, 1, 26);
            sf::RuleTable hardTable;
            hardTable.build(demoHard, 1, 26);

            const std::vector<sf::DetailRow> rows =
                sf::viewDetail(0x1234ABCDu, 1, 13, 2, 1, 26, rankTable, hardTable);
            DetailDialog *dd = new DetailDialog(&host);
            dd->setData(0x1234ABCDu, 2, rows, rankTable, hardTable);
            dlg = dd;
        }

        if (!dlg) {
            std::printf("unknown dialog: %s\n", which.toUtf8().constData());
            return 1;
        }
        dlg->show();
        QTimer::singleShot(500, &a, [dlg, path]() {
            const bool ok = dlg->grab().save(path);
            std::printf("shot %s -> %s  ctxHelp=%d\n", path.toUtf8().constData(),
                        ok ? "ok" : "failed",
                        int(dlg->windowFlags() & Qt::WindowContextHelpButtonHint) ? 1 : 0);
            QCoreApplication::quit();
        });
        return a.exec();
    }

    MainWindow w;

    // 窗口比屏幕可用区域大时, 标题栏会被顶到屏幕外(最小化/最大化按钮就点不到了)。
    // 先按可用区域收一收, 再居中, 并保证左上角不越界。
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        const int maxW = avail.width() - 80;
        const int maxH = avail.height() - 80;
        int ww = std::min(w.width(), maxW);
        int wh = std::min(w.height(), maxH);
        if (ww != w.width() || wh != w.height()) w.resize(ww, wh);

        int x = avail.x() + (avail.width() - ww) / 2;
        int y = avail.y() + (avail.height() - wh) / 2;
        x = std::max(avail.x(), std::min(x, avail.right() - ww + 1));
        y = std::max(avail.y(), std::min(y, avail.bottom() - wh + 1));
        w.move(x, y);
    }
    w.show();

    if (args.contains(QStringLiteral("--demo"))) w.loadDemo();
    if (args.contains(QStringLiteral("--baddemo"))) w.setBadDemo();

    // --badtest : 逐组装入非法条件, 打印每组 checkConditions 的拦截文案 (不弹窗)
    const int badIdx = args.indexOf(QStringLiteral("--badtest"));
    if (badIdx >= 0) {
        // 不跟参数 = 跑全部用例; 跟了就必须是合法编号, 否则 toInt 失败会静默变成 0
        const bool hasArg = (badIdx + 1 < args.size());
        bool numOk = true;
        const int n = hasArg ? args.at(badIdx + 1).toInt(&numOk) : -1;
        if (hasArg && (!numOk || n < 0)) {
            std::printf("--badtest 需要一个非负整数用例编号\n");
            return 2;
        }
        if (n < 0) {
            for (int i = 0; ; ++i) {
                if (!w.setBadCase(i)) break;
                QString err;
                QString bad = w.checkConditionsForTest(&err);
                std::printf("case %d: %s\n", i, bad.isEmpty() ? "(NOT REJECTED!)"
                            : bad.toUtf8().constData());
            }
        } else {
            w.setBadCase(n);
            QString err;
            QString bad = w.checkConditionsForTest(&err);
            std::printf("case %d: %s\n", n, bad.isEmpty() ? "(NOT REJECTED!)"
                        : bad.toUtf8().constData());
        }
        return 0;
    }
    if (args.contains(QStringLiteral("--span"))) {
        // 仅用于自动截图: 全量扫描要几分钟, 截个图没必要等
        const int si = args.indexOf(QStringLiteral("--span"));
        bool ok = false;
        const quint64 n = (si + 1 < args.size()) ? args.at(si + 1).toULongLong(&ok) : 0;
        if (!ok || n == 0) {
            // 之前解析失败会静默退回全量扫描, 自动截图就卡死在几个小时的查找里
            std::printf("--span 需要一个正整数\n");
            return 2;
        }
        w.setSpanForTest(n);
    }

    // --lookup <hex> : 往种子输入框里填一个种子并查看出怪 (配合 --shot 截图)
    const int lookIdx = args.indexOf(QStringLiteral("--lookup"));
    if (lookIdx >= 0 && lookIdx + 1 < args.size()) {
        QTimer::singleShot(400, &a, [&w, hex = args.at(lookIdx + 1)]() {
            w.lookupSeedForTest(hex);
        });
    }

    // --progress <n> : 把进度条停在 n%, 用于截图确认进度条样式
    const int progIdx = args.indexOf(QStringLiteral("--progress"));
    if (progIdx >= 0 && progIdx + 1 < args.size()) {
        bool ok = false;
        const int pct = args.at(progIdx + 1).toInt(&ok);
        if (!ok || pct < 0 || pct > 100) {
            std::printf("--progress 需要 0 到 100 之间的整数\n");
            return 2;
        }
        QTimer::singleShot(400, &a, [&w, pct]() { w.showProgressForTest(pct); });
    }

    const int shotIdx = args.indexOf(QStringLiteral("--shot"));
    if (shotIdx >= 0 && shotIdx + 1 < args.size()) {
        const QString path = args.at(shotIdx + 1);
        QTimer::singleShot(700, &a, [&w, path]() {
            const bool ok = w.grab().save(path);
            std::printf("shot %s -> %s\n", path.toUtf8().constData(), ok ? "ok" : "failed");
            std::printf("window %dx%d  logical dpr=%.2f\n", w.width(), w.height(),
                        w.devicePixelRatioF());
            std::printf("geometry x=%d y=%d w=%d h=%d  min=%dx%d  maxBtn=%d\n",
                        w.x(), w.y(), w.width(), w.height(),
                        w.minimumWidth(), w.minimumHeight(),
                        int(w.windowFlags() & Qt::WindowMaximizeButtonHint));
            for (QScrollArea *sw : w.findChildren<QScrollArea *>()) {
                std::printf("viewport %dx%d inner %dx%d hint=%d\n", sw->viewport()->width(),
                            sw->viewport()->height(), sw->widget()->width(), sw->widget()->height(),
                            sw->widget()->sizeHint().height());
            }
            const QList<QFrame *> cards = w.findChildren<QFrame *>();
            for (QFrame *c : cards) {
                if (c->objectName() == QLatin1String("Card"))
                    std::printf("card %dx%d minH=%d\n", c->width(), c->height(),
                                c->minimumSizeHint().height());
            }
            QCoreApplication::quit();
        });
    }

    const int runIdx = args.indexOf(QStringLiteral("--runshot"));
    if (runIdx >= 0 && runIdx + 1 < args.size()) {
        w.autoRun(args.at(runIdx + 1));
    }

    return a.exec();
}
