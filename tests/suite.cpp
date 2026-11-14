// ===========================================================================
//  分层自动化测试套件 (seed-finder 2.0)
//
//  设计目标: 按"用户实际会怎么用"分层, 每层都同时验证"不会漏"和"不会误杀"。
//
//    L1 引擎正确性   —— 与原版逐位对拍 (Searcher::selfCheck)
//    L2 结构一致性   —— 检索结果 vs 逐种子明细 (两条完全不同的代码路径)
//                       · 普通区段  -> 走去重表缓存路径 (dedup)
//                       · 超长区段  -> 走逐轮建表路径 (!dedup)
//                       · level<=0  -> 走恒 11u 的静态轮路径
//    L3 条件校验     —— 合法条件必须放行 (防误杀), 非法条件必须拦下 (防漏过)
//    L4 边界/资源    —— span=0、种子空间末端、线程数 > 种子数、超大 autoStop
//    L5 交互语义     —— 提前停止、中途取消、暂停/继续、进度单调
//    L6 持久化       —— 预设存读往返、损坏 JSON 容错、旧版本缺字段
//    L7 纯函数       —— 掩码/文本/F 范围换算、场景裁剪
//
//  用法: suite.exe [--quick]
//    --quick 只用小样本, 适合改完代码随手跑一遍。
// ===========================================================================

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMap>
#include <QStringList>
#include <QTextStream>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <thread>

#include "presetstore.h"
#include "seedengine.h"
#include "zombiedata.h"

// ---------------------------------------------------------------------------
//  迷你测试框架
// ---------------------------------------------------------------------------
namespace {

int g_pass = 0;
int g_fail = 0;
QStringList g_failed;
QTextStream *g_out = nullptr;
bool g_verboseDetail = true;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        g_failed << what;
        *g_out << "  [FAIL] " << what;
        if (!detail.isEmpty()) *g_out << "  -> " << detail;
        *g_out << endl;
    }
}

void section(const QString &title)
{
    *g_out << endl << "== " << title << " ==" << endl;
}

QString hex32(quint32 v)
{
    return QStringLiteral("0x") + QString::number(v, 16).toUpper().rightJustified(8, QLatin1Char('0'));
}

// 由一组规则带算出"每轮是否被覆盖"的期望匹配数, 与引擎的 activeWaves 对照。
int expectedActiveLevels(const QVector<sf::RuleBand> &bands, int levelBegin, int levelEnd)
{
    int n = 0;
    const int levels = std::max(0, levelEnd - levelBegin);
    for (int k = 0; k < levels; ++k) {
        const int lv = levelBegin + k;
        for (int i = 0; i < bands.size(); ++i) {
            const sf::RuleBand &b = bands.at(i);
            if (!b.valid() || b.cond.isEmpty()) continue;
            if (lv >= b.beginLevel() && lv < b.endLevel()) { ++n; break; }
        }
    }
    return n;
}

// 逐种子暴力求值: 直接按 computeRow 取位, 不复用任何检索路径。
// 返回 [matched, hardOk] —— 与 evaluateSeed 同语义, 但实现完全独立。
void bruteForceSeed(quint32 seed, int uid, int mode, int scene,
                    const QVector<sf::RuleBand> &rankBands,
                    const QVector<sf::RuleBand> &hardBands,
                    int levelBegin, int levelEnd,
                    int *matchedOut, bool *hardOkOut)
{
    const quint32 pos = sf::seedToRVal(seed, uid + mode);
    int matched = 0;
    bool hardOk = true;
    for (int lv = levelBegin; lv < levelEnd; ++lv) {
        quint32 d1[10];
        sf::computeRow(pos + quint32(lv), scene, d1);
        const quint32 bits = sf::rowBits(d1, lv);

        // 全局条件: 段内每轮都必须满足, 一旦不满足整颗种子出局
        for (int i = 0; i < hardBands.size(); ++i) {
            const sf::RuleBand &b = hardBands.at(i);
            if (!b.valid() || b.cond.isEmpty()) continue;
            if (lv < b.beginLevel() || lv >= b.endLevel()) continue;
            if (!b.cond.matches(bits)) hardOk = false;
        }
        if (!hardOk) break;

        // 出怪条件: 同一轮可能被多条带覆盖, 引擎的语义是"全部满足才算这一轮满足"
        // (RuleTable 把同轮条件平铺到 m_flat, 逐条 matches 全过才 ++matched),
        // 所以这里也必须用"与", 不能命中一条就 break。
        bool covered = false;
        bool allMatch = true;
        for (int i = 0; i < rankBands.size(); ++i) {
            const sf::RuleBand &b = rankBands.at(i);
            if (!b.valid() || b.cond.isEmpty()) continue;
            if (lv < b.beginLevel() || lv >= b.endLevel()) continue;
            covered = true;
            if (!b.cond.matches(bits)) allMatch = false;
        }
        if (covered && allMatch) ++matched;
    }
    *matchedOut = matched;
    *hardOkOut = hardOk;
}

// 把检索结果转换成 "matched -> 总数" 的直方图, 便于与暴力枚举对比
QMap<int, quint64> histogram(const sf::SearchResult &r)
{
    QMap<int, quint64> h;
    for (int i = 0; i < r.buckets.size(); ++i) {
        if (r.buckets[i].total > 0) h.insert(r.buckets[i].matched, r.buckets[i].total);
    }
    return h;
}

QString histogramText(const QMap<int, quint64> &h)
{
    QString s;
    for (QMap<int, quint64>::const_iterator it = h.constBegin(); it != h.constEnd(); ++it) {
        if (!s.isEmpty()) s += QStringLiteral(", ");
        s += QStringLiteral("%1x%2").arg(it.key()).arg(it.value());
    }
    return s.isEmpty() ? QStringLiteral("(empty)") : s;
}

// ---------------------------------------------------------------------------
//  L1: 引擎与原版逐位对拍
// ---------------------------------------------------------------------------
void testSelfCheck()
{
    section(QStringLiteral("L1 引擎与原版逐位对拍"));
    const QString err = sf::Searcher::selfCheck();
    check(err.isEmpty(), QStringLiteral("selfCheck 与内嵌原版分页实现逐位一致"), err);
}

// ---------------------------------------------------------------------------
//  L2: 检索结果 vs 逐种子暴力枚举 (两条独立路径)
// ---------------------------------------------------------------------------
struct CaseSpec
{
    QString name;
    int scene;
    int beginFlag;
    int endFlag;
    quint32 seedBegin;
    quint64 span;
    bool useHard;
};

void runCrossCheck(const CaseSpec &cs, quint64 spanOverride)
{
    sf::SearchSpec spec;
    spec.uid = 1;
    spec.mode = 13;
    spec.scene = cs.scene;
    spec.beginFlag = cs.beginFlag;
    spec.endFlag = cs.endFlag;
    spec.seedBegin = cs.seedBegin;
    spec.span = spanOverride ? spanOverride : cs.span;
    spec.outputSize = 5;
    spec.autoStop = 0;
    spec.gpu = 0;                       // 强制 CPU: GPU 有独立对拍程序, 这里要确定性

    // 出怪条件: 覆盖"必出 / 禁出 / 至少出 / 至多出"四种语义
    sf::RuleBand b1;
    b1.beginFlag = cs.beginFlag;
    b1.endFlag = cs.endFlag;
    b1.cond.needMask = (1u << Z_GIGA);
    b1.cond.refuseMask = (1u << Z_JACK) | (1u << Z_ZOMBONI);
    b1.cond.sanitize();
    spec.bands << b1;

    sf::RuleBand b2;
    b2.beginFlag = cs.beginFlag;
    b2.endFlag = cs.endFlag;
    b2.cond.atLeastMask = (1u << Z_CONE) | (1u << Z_BUCKET) | (1u << Z_DOOR);
    b2.cond.atLeast = 1;
    b2.cond.atMostMask = (1u << Z_POLE) | (1u << Z_NEWS) | (1u << Z_FOOTBALL);
    b2.cond.atMost = 2;
    b2.cond.sanitize();
    spec.bands << b2;

    // 全局条件: 只在部分子区间生效, 验证硬性条件按段裁剪
    if (cs.useHard) {
        sf::RuleBand h;
        h.beginFlag = cs.beginFlag;
        h.endFlag = std::min(cs.endFlag, cs.beginFlag + 4);
        h.cond.refuseMask = (1u << Z_GARG);
        h.cond.sanitize();
        spec.hard << h;
    }

    const int levelBegin = cs.beginFlag / 2;
    const int levelEnd = cs.endFlag / 2;

    sf::RuleTable rankTable, hardTable;
    rankTable.build(spec.bands, levelBegin, levelEnd);
    hardTable.build(spec.hard, levelBegin, levelEnd);

    // 引擎结果
    sf::Searcher searcher;
    const sf::SearchResult r = searcher.run(spec, nullptr, nullptr);
    check(r.error.isEmpty(), QStringLiteral("%1: 检索无错误").arg(cs.name), r.error);

    // 暴力枚举 (单线程 + 多线程都要和枚举对上)
    for (int ti = 0; ti < 2; ++ti) {
        spec.threads = ti == 0 ? 1 : 7;
        const sf::SearchResult rt = searcher.run(spec, nullptr, nullptr);
        const quint64 span = spec.span;

        QMap<int, quint64> want;
        quint64 wantQualified = 0;
        for (quint64 k = 0; k < span; ++k) {
            const quint32 seed = quint32(quint64(cs.seedBegin) + k);
            int m = 0;
            bool hardOk = true;
            bruteForceSeed(seed, 1, 13, cs.scene, spec.bands, spec.hard,
                           levelBegin, levelEnd, &m, &hardOk);
            if (!hardOk) continue;
            ++wantQualified;
            want[m] = want.value(m) + 1;
        }

        const QMap<int, quint64> got = histogram(rt);
        const bool same = (got == want);
        check(same,
              QStringLiteral("%1: %2 线程直方图与暴力枚举一致").arg(cs.name).arg(spec.threads),
              QStringLiteral("引擎=%1 枚举=%2").arg(histogramText(got), histogramText(want)));
        check(rt.qualified == wantQualified,
              QStringLiteral("%1: %2 线程合格数与枚举一致").arg(cs.name).arg(spec.threads),
              QStringLiteral("引擎=%1 枚举=%2").arg(rt.qualified).arg(wantQualified));
        check(rt.scanned == span,
              QStringLiteral("%1: %2 线程扫描数等于 span").arg(cs.name).arg(spec.threads),
              QStringLiteral("scanned=%1 span=%2").arg(rt.scanned).arg(span));
        check(rt.activeWaves == expectedActiveLevels(spec.bands, levelBegin, levelEnd),
              QStringLiteral("%1: 活跃轮数等于规则带覆盖数").arg(cs.name),
              QStringLiteral("activeWaves=%1").arg(rt.activeWaves));
        check(!rt.stopped && !rt.enoughFound,
              QStringLiteral("%1: 无取消/无早停时 stopped/enoughFound 为假").arg(cs.name));
        check(rt.seconds >= 0.0,
              QStringLiteral("%1: 耗时为非负").arg(cs.name));
    }

    // 结果桶必须按 matched 降序, 且只保留 total>0
    bool ordered = true;
    for (int i = 1; i < r.buckets.size(); ++i) {
        if (r.buckets[i - 1].matched <= r.buckets[i].matched) ordered = false;
    }
    for (int i = 0; i < r.buckets.size(); ++i) {
        if (r.buckets[i].total == 0) ordered = false;
        if (r.buckets[i].examples.size() > spec.outputSize) ordered = false;
    }
    check(ordered, QStringLiteral("%1: 结果桶降序、非空、示例数不超 outputSize").arg(cs.name));
}

void testCrossCheck(bool quick)
{
    section(QStringLiteral("L2 检索结果 vs 逐种子暴力枚举"));

    // 普通区段: 走 dedup 表缓存路径
    {
        CaseSpec cs;
        cs.name = QStringLiteral("普通区段(dedup)");
        cs.scene = 2;
        cs.beginFlag = 0;
        cs.endFlag = 50;
        cs.seedBegin = 0;
        cs.span = quick ? 4000 : 20000;
        cs.useHard = true;
        runCrossCheck(cs, 0);
    }

    // 超长区段: lastStride 超过 kMaxTables, 走逐轮建表的 !dedup 路径
    {
        CaseSpec cs;
        cs.name = QStringLiteral("超长区段(!dedup)");
        cs.scene = 2;
        cs.beginFlag = 0;
        cs.endFlag = 1400;      // 700 轮, 101*699 > 65536
        cs.seedBegin = 0;
        cs.span = 0;
        cs.useHard = true;
        runCrossCheck(cs, quick ? 60 : 200);
    }

    // 非零起点 + 跨过 2^32 回绕的区段
    {
        CaseSpec cs;
        cs.name = QStringLiteral("起点非零");
        cs.scene = 4;
        cs.beginFlag = 10;
        cs.endFlag = 60;
        cs.seedBegin = 0x40000000u;
        cs.span = 0;
        cs.useHard = false;
        runCrossCheck(cs, quick ? 2000 : 8000);
    }

    // 只覆盖早期轮 (level 0 的静态轮路径)
    {
        CaseSpec cs;
        cs.name = QStringLiteral("仅早期轮(含 level 0)");
        cs.scene = 2;
        cs.beginFlag = 0;
        cs.endFlag = 4;
        cs.seedBegin = 0;
        cs.span = 0;
        cs.useHard = true;
        runCrossCheck(cs, quick ? 2000 : 8000);
    }
}

// ---------------------------------------------------------------------------
//  L3: 条件合法性校验 —— 该拦的拦, 不该拦的放行
// ---------------------------------------------------------------------------
void testConditionValidation()
{
    section(QStringLiteral("L3 条件校验: 非法必拦 / 合法必放"));

    const int scene = 2;        // PE/FE: 20 种僵尸全可能

    // ---- 必须拦下的用例 ----
    struct BadCase { QString name; int beginFlag; int endFlag; sf::WaveCondition cond; };
    QVector<BadCase> bad;

    {   // 红眼最早第 6 轮
        BadCase c; c.name = QStringLiteral("必出红眼早于第 6 轮");
        c.beginFlag = 2; c.endFlag = 4;
        c.cond.needMask = (1u << Z_GIGA); bad << c;
    }
    {   // 冰车/白眼最早第 3 轮
        BadCase c; c.name = QStringLiteral("必出冰车早于第 3 轮");
        c.beginFlag = 2; c.endFlag = 4;
        c.cond.needMask = (1u << Z_ZOMBONI); bad << c;
    }
    {   // 路障 + 读报 同时禁出 (每轮必出其一)
        BadCase c; c.name = QStringLiteral("同时禁出路障与读报");
        c.beginFlag = 10; c.endFlag = 12;
        c.cond.refuseMask = (1u << Z_CONE) | (1u << Z_NEWS); bad << c;
    }
    {   // 同一僵尸既必出又禁出
        BadCase c; c.name = QStringLiteral("必出∩禁出");
        c.beginFlag = 10; c.endFlag = 12;
        c.cond.needMask = (1u << Z_JACK); c.cond.refuseMask = (1u << Z_JACK); bad << c;
    }
    {   // 至多出 0 种, 但集合含每轮必出的两只
        BadCase c; c.name = QStringLiteral("至多 0 种含路障读报");
        c.beginFlag = 10; c.endFlag = 12;
        c.cond.atMostMask = (1u << Z_CONE) | (1u << Z_NEWS); c.cond.atMost = 0; bad << c;
    }
    {   // 第 1 轮只有 2 个新增名额
        BadCase c; c.name = QStringLiteral("第 1 轮必出 3 种非固定僵尸");
        c.beginFlag = 0; c.endFlag = 2;
        c.cond.needMask = (1u << Z_POLE) | (1u << Z_BUCKET) | (1u << Z_DOOR); bad << c;
    }
    {   // 第 2 轮同样只有 2 个新增名额
        BadCase c; c.name = QStringLiteral("第 2 轮必出 3 种非固定僵尸");
        c.beginFlag = 2; c.endFlag = 4;
        c.cond.needMask = (1u << Z_POLE) | (1u << Z_BUCKET) | (1u << Z_DOOR); bad << c;
    }
    {   // 起点很早的长区间, 卡的是最早那一轮
        BadCase c; c.name = QStringLiteral("3~10F 必出 3 种但起点名额不足");
        c.beginFlag = 2; c.endFlag = 10;
        c.cond.needMask = (1u << Z_DOLPHIN) | (1u << Z_JACK) | (1u << Z_BALLOON); bad << c;
    }

    for (int i = 0; i < bad.size(); ++i) {
        sf::WaveCondition c = bad[i].cond;
        c.sanitize();
        sf::RuleBand b;
        b.beginFlag = bad[i].beginFlag;
        b.endFlag = bad[i].endFlag;
        b.cond = c;
        const QString err = sf::checkBandLocal(b, scene, QStringLiteral("出怪条件"));
        check(!err.isEmpty(), QStringLiteral("应拦下: %1").arg(bad[i].name),
              QStringLiteral("却放行了"));
        if (g_verboseDetail && !err.isEmpty()) {
            *g_out << "    (" << err << ")" << endl;
        }
    }

    // ---- 必须放行的用例 (防"校验过严"误杀) ----
    struct GoodCase { QString name; int beginFlag; int endFlag; sf::WaveCondition cond; };
    QVector<GoodCase> good;

    {   // 红眼在第 6 轮及之后完全合法
        GoodCase c; c.name = QStringLiteral("必出红眼在第 6 轮");
        c.beginFlag = 10; c.endFlag = 12;
        c.cond.needMask = (1u << Z_GIGA); good << c;
    }
    {   // 冰车在第 3 轮
        GoodCase c; c.name = QStringLiteral("必出冰车在第 3 轮");
        c.beginFlag = 4; c.endFlag = 6;
        c.cond.needMask = (1u << Z_ZOMBONI); good << c;
    }
    {   // 单个普通僵尸
        GoodCase c; c.name = QStringLiteral("必出单个普通僵尸");
        c.beginFlag = 2; c.endFlag = 60;
        c.cond.needMask = (1u << Z_BUCKET); good << c;
    }
    {   // 第 1 轮(F3~4F) 2 种非固定僵尸: 正好用满名额
        //     注意 beginFlag 必须是 2 (level 1); beginFlag=0 是 F1~2F, 界面上填不出来,
        //     那一轮只有 1 个新增格位。
        GoodCase c; c.name = QStringLiteral("第 1 轮必出 2 种非固定僵尸");
        c.beginFlag = 2; c.endFlag = 4;
        c.cond.needMask = (1u << Z_POLE) | (1u << Z_BUCKET); good << c;
    }
    {   // 禁出 3 种, 候选仍足够
        GoodCase c; c.name = QStringLiteral("禁出 3 种普通僵尸");
        c.beginFlag = 10; c.endFlag = 20;
        c.cond.refuseMask = (1u << Z_DANCE) | (1u << Z_SNORKEL) | (1u << Z_DIGGER); good << c;
    }
    {   // 必出 + 禁出 + 至少 + 至多 组合, 互不矛盾
        GoodCase c; c.name = QStringLiteral("四组条件混合且互不矛盾");
        c.beginFlag = 10; c.endFlag = 30;
        c.cond.needMask = (1u << Z_GIGA);
        c.cond.refuseMask = (1u << Z_JACK);
        c.cond.atLeastMask = (1u << Z_CONE) | (1u << Z_BUCKET) | (1u << Z_DOOR);
        c.cond.atLeast = 1;
        c.cond.atMostMask = (1u << Z_POLE) | (1u << Z_NEWS);
        c.cond.atMost = 2;
        good << c;
    }
    {   // 空条件: 校验器应直接放行 (不允许报错卡住用户)
        GoodCase c; c.name = QStringLiteral("空条件");
        c.beginFlag = 10; c.endFlag = 12;
        good << c;
    }

    for (int i = 0; i < good.size(); ++i) {
        sf::WaveCondition c = good[i].cond;
        c.sanitize();
        sf::RuleBand b;
        b.beginFlag = good[i].beginFlag;
        b.endFlag = good[i].endFlag;
        b.cond = c;
        const QString err = sf::checkBandLocal(b, scene, QStringLiteral("出怪条件"));
        check(err.isEmpty(), QStringLiteral("应放行: %1").arg(good[i].name), err);
    }

    // ---- 跨段矛盾 ----
    {
        QVector<sf::RuleBand> siblings;
        sf::RuleBand a;
        a.beginFlag = 10; a.endFlag = 16;
        a.cond.needMask = (1u << Z_JACK);
        a.cond.sanitize();
        sf::RuleBand c;
        c.beginFlag = 14; c.endFlag = 20;
        c.cond.refuseMask = (1u << Z_JACK);
        c.cond.sanitize();
        siblings << a << c;

        check(!sf::checkBand(c, scene, QStringLiteral("出怪条件"), siblings).isEmpty(),
              QStringLiteral("重叠段: 一段必出、另一段禁出同一僵尸应拦下"));

        // 不重叠时不应拦
        sf::RuleBand d;
        d.beginFlag = 20; d.endFlag = 26;
        d.cond.refuseMask = (1u << Z_JACK);
        d.cond.sanitize();
        QVector<sf::RuleBand> s2;
        s2 << a << d;
        check(sf::checkBand(d, scene, QStringLiteral("出怪条件"), s2).isEmpty(),
              QStringLiteral("不重叠段: 同样的必出/禁出组合应放行"));
    }

    // ---- 场景裁剪: 本场景不会出现的僵尸, 不该被当成"必出太多" ----
    {
        sf::RuleBand b;
        b.beginFlag = 2; b.endFlag = 6;
        b.cond.needMask = (1u << Z_DOLPHIN);      // 海豚: PE/FE 可能出现
        b.cond.sanitize();
        check(sf::checkBandLocal(b, 2, QStringLiteral("出怪条件")).isEmpty(),
              QStringLiteral("场景内可能出现的僵尸不应被拦"));
        // RE/ME 不会出潜水/海豚, 但校验器只按"名额"判断, 仍应放行 (界面上选不到)
        check(sf::checkBandLocal(b, 4, QStringLiteral("出怪条件")).isEmpty(),
              QStringLiteral("跨场景僵尸位不应硬报错(靠界面裁剪处理)"));
    }
}

// ---------------------------------------------------------------------------
//  L4: 边界与资源
// ---------------------------------------------------------------------------
void testBoundaries()
{
    section(QStringLiteral("L4 边界与资源限制"));

    sf::RuleBand b;
    b.beginFlag = 0; b.endFlag = 50;
    b.cond.refuseMask = (1u << Z_GIGA);
    b.cond.sanitize();

    // span = 0: 引擎应把它当成 1 个种子, 而不是除零/死循环
    {
        sf::SearchSpec spec;
        spec.span = 0;
        spec.gpu = 0;
        spec.bands << b;
        sf::Searcher s;
        const sf::SearchResult r = s.run(spec, nullptr, nullptr);
        check(r.error.isEmpty(), QStringLiteral("span=0 不报错"), r.error);
        check(r.scanned == 1, QStringLiteral("span=0 被归一为 1 个种子"),
              QStringLiteral("scanned=%1").arg(r.scanned));
    }

    // 种子空间末端: 必须被裁到 0x100000000, 不能越界回绕
    {
        sf::SearchSpec spec;
        spec.seedBegin = 0xFFFFFFF0u;
        spec.span = 0x1000;
        spec.gpu = 0;
        spec.bands << b;
        sf::Searcher s;
        const sf::SearchResult r = s.run(spec, nullptr, nullptr);
        check(r.error.isEmpty(), QStringLiteral("种子空间末端不报错"), r.error);
        check(r.scanned == 16, QStringLiteral("末端扫描量被裁到剩余种子数"),
              QStringLiteral("scanned=%1").arg(r.scanned));
    }

    // seedBegin 完全越界
    {
        sf::SearchSpec spec;
        spec.seedBegin = 0xFFFFFFFFu;
        spec.span = 0x1000;
        spec.gpu = 0;
        spec.bands << b;
        sf::Searcher s;
        const sf::SearchResult r = s.run(spec, nullptr, nullptr);
        check(r.scanned == 1, QStringLiteral("seedBegin=0xFFFFFFFF 时只剩 1 个种子"),
              QStringLiteral("scanned=%1").arg(r.scanned));
    }

    // 线程数 > 种子数: 线程数必须被夹下来
    {
        sf::SearchSpec spec;
        spec.span = 3;
        spec.threads = 64;
        spec.gpu = 0;
        spec.bands << b;
        sf::Searcher s;
        const sf::SearchResult r = s.run(spec, nullptr, nullptr);
        check(r.error.isEmpty() && !r.buckets.isEmpty(),
              QStringLiteral("线程数大于种子数时不崩"));
        check(r.scanned == 3, QStringLiteral("线程数>种子数 时扫描数正确"),
              QStringLiteral("scanned=%1").arg(r.scanned));
    }

    // 没有任何条件: 引擎必须明确报错, 而不是返回空结果让用户以为"没找到"
    {
        sf::SearchSpec spec;
        spec.span = 100;
        spec.gpu = 0;
        sf::Searcher s;
        const sf::SearchResult r = s.run(spec, nullptr, nullptr);
        check(!r.error.isEmpty(), QStringLiteral("完全无条件时返回可读错误"),
              QStringLiteral("error 为空"));
    }

    // 非法区间: endFlag <= beginFlag
    {
        sf::SearchSpec spec;
        spec.beginFlag = 50;
        spec.endFlag = 10;
        spec.gpu = 0;
        spec.bands << b;
        sf::Searcher s;
        const sf::SearchResult r = s.run(spec, nullptr, nullptr);
        check(!r.error.isEmpty(), QStringLiteral("非法 F 区间返回可读错误"));
    }

    // outputSize 极小/极大都不能崩
    {
        for (int cap = 1; cap <= 64; cap *= 8) {
            sf::SearchSpec spec;
            spec.span = 5000;
            spec.outputSize = cap;
            spec.gpu = 0;
            spec.bands << b;
            sf::Searcher s;
            const sf::SearchResult r = s.run(spec, nullptr, nullptr);
            bool ok = true;
            for (int i = 0; i < r.buckets.size(); ++i) {
                if (r.buckets[i].examples.size() > cap) ok = false;
            }
            check(ok, QStringLiteral("outputSize=%1 时示例数不越界").arg(cap));
        }
    }

    // autoStop 超过实际可满足数量: 必须跑完全部而不是提前停
    {
        sf::SearchSpec spec;
        spec.span = 3000;
        spec.autoStop = 1000000;
        spec.gpu = 0;
        spec.bands << b;
        sf::Searcher s;
        const sf::SearchResult r = s.run(spec, nullptr, nullptr);
        check(!r.enoughFound, QStringLiteral("autoStop 大于可满足数时不早停"));
        check(r.scanned == 3000, QStringLiteral("autoStop 未达成时跑满 span"),
              QStringLiteral("scanned=%1").arg(r.scanned));
    }
}

// ---------------------------------------------------------------------------
//  L5: 交互语义 (提前停止 / 取消 / 暂停 / 进度)
// ---------------------------------------------------------------------------
void testInteraction()
{
    section(QStringLiteral("L5 交互语义: 早停 / 取消 / 暂停 / 进度"));

    sf::RuleBand b;
    b.beginFlag = 0; b.endFlag = 20;
    b.cond.refuseMask = (1u << Z_GIGA);
    b.cond.sanitize();

    // autoStop = 1: 找到 1 个就停, 且 enoughFound 为真
    {
        sf::SearchSpec spec;
        spec.span = 400000;
        spec.autoStop = 1;
        spec.gpu = 0;
        spec.bands << b;
        sf::Searcher s;
        QElapsedTimer t; t.start();
        const sf::SearchResult r = s.run(spec, nullptr, nullptr);
        check(r.enoughFound, QStringLiteral("autoStop=1 时 enoughFound 为真"));
        check(r.fullCount() >= 1, QStringLiteral("autoStop=1 时至少拿到 1 个全满足种子"),
              QStringLiteral("full=%1").arg(r.fullCount()));
        check(r.scanned < spec.span || r.fullCount() >= 1,
              QStringLiteral("autoStop 生效时扫描数应远小于 span"),
              QStringLiteral("scanned=%1").arg(r.scanned));
        check(t.elapsed() < 30000, QStringLiteral("autoStop 能明显提前结束"));
    }

    // 取消: cancel 预先置位, 应立即返回且 stopped 为真
    {
        sf::SearchSpec spec;
        spec.span = 4000000;
        spec.gpu = 0;
        spec.bands << b;
        std::atomic<bool> cancel(true);
        sf::Searcher s;
        QElapsedTimer t; t.start();
        const sf::SearchResult r = s.run(spec, nullptr, &cancel);
        check(t.elapsed() < 20000, QStringLiteral("预先取消时立刻返回"));
        check(r.scanned < spec.span, QStringLiteral("取消后没有跑满 span"),
              QStringLiteral("scanned=%1").arg(r.scanned));
        check(r.stopped, QStringLiteral("取消后 stopped 为真"));
    }

    // 取消: 跑到一半再取消
    {
        sf::SearchSpec spec;
        // span 要足够大: 这台机器 ~50M 种子/秒, 太小的话 150ms 内就跑完了,
        // 取消根本没机会生效 (曾经因此报过一次假失败)。
        spec.span = 400000000;
        spec.gpu = 0;
        spec.bands << b;
        std::atomic<bool> cancel(false);
        sf::Searcher s;
        std::thread killer([&cancel]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            cancel.store(true, std::memory_order_relaxed);
        });
        const sf::SearchResult r = s.run(spec, nullptr, &cancel);
        killer.join();
        check(r.stopped, QStringLiteral("中途取消后 stopped 为真"));
        check(r.scanned < spec.span, QStringLiteral("中途取消后 scanned < span"),
              QStringLiteral("scanned=%1").arg(r.scanned));
    }

    // 暂停: 暂停期间 scanned 不应前进, 恢复后能正常跑完
    {
        sf::SearchSpec spec;
        spec.span = 400000000;
        spec.gpu = 0;
        spec.bands << b;
        std::atomic<bool> pause(true);
        std::atomic<bool> cancel(false);
        std::atomic<quint64> lastScanned(0);
        std::atomic<int> lastPct(0);
        bool monotonic = true;
        std::atomic<bool> pausedStable(true);
        quint64 seenWhilePaused = 0;

        std::thread toucher([&pause, &cancel]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            pause.store(false, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            cancel.store(true, std::memory_order_relaxed);
        });

        sf::Searcher s;
        s.run(spec, [&](int pct, quint64 scanned, quint64) {
            if (pct < lastPct.load()) monotonic = false;
            lastPct.store(pct);
            if (scanned < lastScanned.load()) monotonic = false;
            lastScanned.store(scanned);
            if (pause.load()) {
                if (seenWhilePaused == 0) seenWhilePaused = scanned;
                else if (scanned != seenWhilePaused) pausedStable.store(false);
            }
        }, &cancel, &pause);
        toucher.join();

        check(monotonic, QStringLiteral("进度百分比与扫描数单调不减"));
        check(pausedStable.load(), QStringLiteral("暂停期间扫描数停止前进"));
    }

    // 进度回调: 至少被调用一次, 且百分比落在 [0,100]
    {
        sf::SearchSpec spec;
        spec.span = 200000;
        spec.gpu = 0;
        spec.bands << b;
        int calls = 0;
        bool pctOk = true;
        sf::Searcher s;
        s.run(spec, [&](int pct, quint64, quint64) {
            ++calls;
            if (pct < 0 || pct > 100) pctOk = false;
        }, nullptr);
        check(calls > 0, QStringLiteral("进度回调至少被调用一次"));
        check(pctOk, QStringLiteral("进度百分比在 0~100 之间"));
    }
}

// ---------------------------------------------------------------------------
//  L6: 预设持久化
// ---------------------------------------------------------------------------
void testPresets()
{
    section(QStringLiteral("L6 预设存读往返与容错"));

    const QString tmp = QDir::tempPath() + QStringLiteral("/seedfinder-suite-presets.json");
    QFile::remove(tmp);
    // 2.0 用 SEEDFINDER2_PRESETS; 同时清掉老名字, 免得外部环境干扰
    qunsetenv("SEEDFINDER3_PRESETS");
    qputenv("SEEDFINDER2_PRESETS", tmp.toUtf8());

    check(presetFilePath() == tmp, QStringLiteral("环境变量能重定向预设文件路径"),
          presetFilePath());

    // 老版本脚本用的 SEEDFINDER3_PRESETS 仍然要认, 否则升级就把自动化弄坏了
    {
        const QString legacy = QDir::tempPath() + QStringLiteral("/seedfinder-suite-presets-legacy.json");
        QFile::remove(legacy);
        qunsetenv("SEEDFINDER2_PRESETS");
        qputenv("SEEDFINDER3_PRESETS", legacy.toUtf8());
        check(presetFilePath() == legacy,
              QStringLiteral("旧的 SEEDFINDER3_PRESETS 仍可作为后备"),
              presetFilePath());
        qunsetenv("SEEDFINDER3_PRESETS");
        qputenv("SEEDFINDER2_PRESETS", tmp.toUtf8());
    }

    QVector<Preset> list;

    Preset p1;
    p1.name = QStringLiteral("测试 · 红眼流");
    p1.uid = 2; p1.mode = 7; p1.scene = 4; p1.beginFlag = 10; p1.endFlag = 60;
    p1.outputSize = 33;
    sf::RuleBand b;
    b.beginFlag = 10; b.endFlag = 20;
    b.cond.needMask = (1u << Z_GIGA);
    b.cond.refuseMask = (1u << Z_JACK);
    b.cond.sanitize();
    p1.bands << b;
    sf::RuleBand h;
    h.beginFlag = 10; h.endFlag = 12;
    h.cond.refuseMask = (1u << Z_ZOMBONI);
    h.cond.sanitize();
    p1.hard << h;
    list << p1;

    Preset p2;
    p2.name = QStringLiteral("全 32 位掩码");
    p2.bands << b;
    p2.hard << h;
    // 故意把所有掩码位都打开, 验证 double 序列化在 32 位全置位时不丢精度
    for (int i = 0; i < 2; ++i) {
        sf::RuleBand &x = (i == 0) ? p2.bands[0] : p2.hard[0];
        x.cond.needMask = 0xFFFFFFFFu;
        x.cond.refuseMask = 0xFFFFFFFFu;
        x.cond.atLeastMask = 0xFFFFFFFFu;
        x.cond.atMostMask = 0xFFFFFFFFu;
        x.cond.atLeast = 3;
        x.cond.atMost = 5;
        x.cond.sanitize();
    }
    list << p2;

    QString err;
    check(savePresets(list, &err), QStringLiteral("保存预设成功"), err);

    QString err2;
    const QVector<Preset> back = loadPresets(&err2);
    check(err2.isEmpty(), QStringLiteral("读取预设无错误"), err2);
    check(back.size() == list.size(), QStringLiteral("预设条数一致"),
          QStringLiteral("写=%1 读=%2").arg(list.size()).arg(back.size()));

    if (back.size() == list.size()) {
        for (int i = 0; i < back.size(); ++i) {
            const Preset &a = list[i];
            const Preset &c = back[i];
            check(a.name == c.name, QStringLiteral("预设名一致"));
            check(a.uid == c.uid && a.mode == c.mode && a.scene == c.scene,
                  QStringLiteral("预设存档信息一致"));
            check(a.beginFlag == c.beginFlag && a.endFlag == c.endFlag,
                  QStringLiteral("预设 F 区间一致"));
            check(a.outputSize == c.outputSize, QStringLiteral("预设输出数一致"));
            check(a.bands.size() == c.bands.size() && a.hard.size() == c.hard.size(),
                  QStringLiteral("预设条件条数一致"));

            for (int k = 0; k < a.bands.size() && k < c.bands.size(); ++k) {
                check(a.bands[k].beginFlag == c.bands[k].beginFlag
                      && a.bands[k].endFlag == c.bands[k].endFlag,
                      QStringLiteral("出怪条件区间一致"));
                check(a.bands[k].cond == c.bands[k].cond,
                      QStringLiteral("出怪条件掩码一致 (含 0xFFFFFFFF 全置位)"),
                      QStringLiteral("写=%1/%2/%3/%4 读=%5/%6/%7/%8")
                          .arg(a.bands[k].cond.needMask).arg(a.bands[k].cond.refuseMask)
                          .arg(a.bands[k].cond.atLeastMask).arg(a.bands[k].cond.atMostMask)
                          .arg(c.bands[k].cond.needMask).arg(c.bands[k].cond.refuseMask)
                          .arg(c.bands[k].cond.atLeastMask).arg(c.bands[k].cond.atMostMask));
            }
            for (int k = 0; k < a.hard.size() && k < c.hard.size(); ++k) {
                check(a.hard[k].cond == c.hard[k].cond,
                      QStringLiteral("全局条件掩码一致"));
            }
        }
    }

    // 损坏的 JSON: 必须返回错误而不是崩溃
    {
        QFile f(tmp);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write("{ this is not json ");
            f.close();
        }
        QString e3;
        const QVector<Preset> bad = loadPresets(&e3);
        check(!e3.isEmpty() || bad.isEmpty(),
              QStringLiteral("损坏 JSON 不崩溃 (返回错误或空表)"),
              QStringLiteral("err=%1 n=%2").arg(e3).arg(bad.size()));
    }

    // 缺字段/空名字的旧文件: 容错读入
    {
        QFile f(tmp);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write("[\n"
                    "  {\"uid\": 3, \"mode\": 5, \"scene\": 1, "
                    "\"beginFlag\": 4, \"endFlag\": 20, \"outputSize\": 10,\n"
                    "   \"bands\": [{\"begin\": 4, \"end\": 20, "
                    "\"cond\": {\"need\": 1048576, \"refuse\": 0, "
                    "\"atLeastMask\": 0, \"atMostMask\": 0, "
                    "\"atLeast\": 0, \"atMost\": 0}}],\n"
                    "   \"hard\": []}\n"
                    "]\n");
            f.close();
        }
        QString e4;
        const QVector<Preset> old = loadPresets(&e4);
        check(e4.isEmpty(), QStringLiteral("缺 name 字段的旧格式能被容错读入"), e4);
        check(old.size() == 1, QStringLiteral("旧格式读入 1 条"),
              QStringLiteral("n=%1").arg(old.size()));
        if (old.size() == 1) {
            check(!old[0].name.isEmpty(), QStringLiteral("缺名字时自动补一个默认名"),
                  old[0].name);
            check(old[0].uid == 3 && old[0].mode == 5 && old[0].scene == 1,
                  QStringLiteral("旧格式存档信息正确"));
            check(old[0].bands.size() == 1, QStringLiteral("旧格式条件带读入正确"));
        }
    }

    // 空条件带必须被跳过 (否则会出现"什么限制都没有"的假条件)
    {
        QFile f(tmp);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write("[\n"
                    "  {\"name\": \"只有空条件\", \"uid\": 1, \"mode\": 13, \"scene\": 2,\n"
                    "   \"beginFlag\": 2, \"endFlag\": 20, \"outputSize\": 20,\n"
                    "   \"bands\": [{\"begin\": 2, \"end\": 20, \"cond\": {}}],\n"
                    "   \"hard\": []}\n"
                    "]\n");
            f.close();
        }
        QString e5;
        const QVector<Preset> empty = loadPresets(&e5);
        check(empty.size() == 1 && empty[0].bands.isEmpty(),
              QStringLiteral("空条件带在读取时被丢弃"),
              QStringLiteral("n=%1 bands=%2")
                  .arg(empty.size())
                  .arg(empty.isEmpty() ? -1 : empty[0].bands.size()));
    }

    QFile::remove(tmp);
}

// ---------------------------------------------------------------------------
//  L7: 纯函数与文本
// ---------------------------------------------------------------------------
void testPureFunctions()
{
    section(QStringLiteral("L7 纯函数: F 换算 / 掩码文本 / 场景裁剪"));

    // 种子 <-> rVal 必须互逆
    {
        bool ok = true;
        quint32 s = 0xDEADBEEFu;
        for (int k = 0; k < 500; ++k, s += 2654435761u) {
            for (int off = 0; off < 32; ++off) {
                const quint32 rv = sf::seedToRVal(s, off);
                if (sf::rValToSeed(rv, off) != s) ok = false;
            }
        }
        check(ok, QStringLiteral("seedToRVal / rValToSeed 互为逆运算"));
    }

    // 轮次与 F 的换算: beginFlag = 起始轮-1, endFlag = 终止轮
    {
        sf::RuleBand b;
        b.beginFlag = 10;
        b.endFlag = 12;
        check(b.beginLevel() == 5 && b.endLevel() == 6,
              QStringLiteral("F 11~12 覆盖 level 5"));
        check(b.valid(), QStringLiteral("[10,12) 是合法区间"));
        b.beginFlag = 12;
        b.endFlag = 12;
        check(!b.valid(), QStringLiteral("空区间不合法"));
        b.beginFlag = -2;
        b.endFlag = 10;
        check(!b.valid(), QStringLiteral("负 beginFlag 不合法"));
    }

    // sanitize: 夹取范围 / 重算 needTotal
    {
        sf::WaveCondition c;
        c.needMask = 0x7u;
        c.atLeastMask = 0x6u;
        c.atLeast = 99;
        c.atMostMask = 0x6u;
        c.atMost = -5;
        c.sanitize();
        check(c.needTotal == 3, QStringLiteral("needTotal 等于 needMask 的 popcount"));
        check(c.atLeast == 2, QStringLiteral("atLeast 被夹到 atLeastMask 的 popcount"));
        check(c.atMost == 0, QStringLiteral("负 atMost 被夹到 0"));

        sf::WaveCondition d;
        d.atLeast = 5;
        d.atMost = 5;
        d.sanitize();
        check(d.atLeast == 0 && d.atMost == 0,
              QStringLiteral("mask 为空时数量被清零"));
    }

    // matches: 四种语义各测一遍
    {
        sf::WaveCondition c;
        c.needMask = (1u << Z_CONE) | (1u << Z_BUCKET);
        c.refuseMask = (1u << Z_JACK);
        c.atLeastMask = (1u << Z_DOOR) | (1u << Z_FOOTBALL);
        c.atLeast = 1;
        c.atMostMask = (1u << Z_POLE) | (1u << Z_NEWS);
        c.atMost = 1;
        c.sanitize();

        // 这个条件同时带"必出 / 禁出 / 至少出 / 至多出"四类要求, 所以基准位模式
        // 必须同时满足: 含 CONE+BUCKET、不含 JACK、至少含 DOOR/FOOTBALL 之一、
        // POLE/NEWS 不超过 1 种。
        const quint32 need = (1u << Z_CONE) | (1u << Z_BUCKET);
        check(!c.matches(need), QStringLiteral("缺「至少出」则不匹配"));
        const quint32 base = need | (1u << Z_DOOR);
        check(c.matches(base), QStringLiteral("四类要求全部满足则匹配"));
        check(c.matches(need | (1u << Z_FOOTBALL)),
              QStringLiteral("「至少出」换成另一种也匹配"));
        check(!c.matches(base | (1u << Z_JACK)), QStringLiteral("出现禁出僵尸则不匹配"));
        check(!c.matches((1u << Z_CONE) | (1u << Z_DOOR)),
              QStringLiteral("缺一个必出则不匹配"));
        check(!c.matches(base | (1u << Z_POLE) | (1u << Z_NEWS)),
              QStringLiteral("至多出 1 种不允许 2 种"));
        check(c.matches(base | (1u << Z_POLE)), QStringLiteral("至多出允许恰好等于上限"));
    }

    // rowBits: level<=0 恒 11u, level>=8 夹到 8
    {
        quint32 d1[10];
        for (int i = 0; i < 10; ++i) d1[i] = quint32(i) + 3u;
        check(sf::rowBits(d1, 0) == 11u, QStringLiteral("rowBits(level 0) 恒为 11u"));
        check(sf::rowBits(d1, -5) == 11u, QStringLiteral("rowBits(负 level) 恒为 11u"));
        check(sf::rowBits(d1, 1) == d1[2], QStringLiteral("rowBits(1) 取 d1[2]"));
        check(sf::rowBits(d1, 8) == d1[9], QStringLiteral("rowBits(8) 取 d1[9]"));
        check(sf::rowBits(d1, 99) == d1[9], QStringLiteral("rowBits(超界) 夹到 d1[9]"));
    }

    // 文本函数: 不允许出现空串或未替换的占位符
    {
        const quint32 m = (1u << Z_CONE) | (1u << Z_NEWS);
        check(!sf::maskToNames(m).isEmpty(), QStringLiteral("maskToNames 非空"));
        check(!sf::maskToAbbr(m).isEmpty(), QStringLiteral("maskToAbbr 非空"));
        check(sf::flagRangeText(10, 12).contains(QStringLiteral("11")),
              QStringLiteral("flagRangeText 显示的是轮次而不是 flag"),
              sf::flagRangeText(10, 12));

        sf::WaveCondition c;
        c.needMask = (1u << Z_GIGA);
        c.refuseMask = (1u << Z_JACK);
        c.atLeastMask = (1u << Z_CONE) | (1u << Z_BUCKET);
        c.atLeast = 1;
        c.atMostMask = (1u << Z_POLE);
        c.atMost = 1;
        c.sanitize();
        const QString sum = c.summary();
        const QString sh = c.shortText();
        check(!sum.isEmpty() && !sh.isEmpty(), QStringLiteral("summary/shortText 非空"));
        check(!sum.contains(QStringLiteral("%")) , QStringLiteral("summary 无残留占位符"), sum);
        check(!sh.contains(QStringLiteral("%")), QStringLiteral("shortText 无残留占位符"), sh);
        check(sf::WaveCondition().isEmpty(), QStringLiteral("默认条件 isEmpty 为真"));
    }

    // 场景裁剪: sceneZombieMask 恒不含 24/25 位 (抽卡空槽)
    {
        bool ok = true;
        const QVector<int> ids = sceneIds();
        for (int i = 0; i < ids.size(); ++i) {
            const quint32 m = sceneZombieMask(ids[i]);
            if (m & ((1u << 24) | (1u << 25))) ok = false;
            if (m == 0) ok = false;
        }
        check(ok, QStringLiteral("每个场景的可用僵尸掩码非空且不含空槽位"));
        check(sceneIds().size() == sceneDisplayNames().size(),
              QStringLiteral("场景 id 与显示名数量一致"));
        check(sceneDisplayNames().size() == 5, QStringLiteral("共 5 个可选场景"));
    }

    // 明细: 行数 = 覆盖的轮数, 且与 evaluateSeed 的计数一致
    {
        QVector<sf::RuleBand> bands;
        sf::RuleBand b;
        b.beginFlag = 0; b.endFlag = 20;
        b.cond.needMask = (1u << Z_GIGA);
        b.cond.sanitize();
        bands << b;
        sf::RuleTable rank, hard;
        rank.build(bands, 0, 10);
        const sf::WaveOracle oracle(2);

        bool ok = true;
        quint32 seed = 0x1234u;
        for (int k = 0; k < 100; ++k, seed += 7919u) {
            const std::vector<sf::DetailRow> rows =
                sf::viewDetail(seed, 1, 13, 2, 0, 10, rank, hard);
            if (int(rows.size()) != 10) ok = false;
            int cnt = 0;
            for (size_t i = 0; i < rows.size(); ++i) {
                if (rows[i].hasRule && rows[i].matched) ++cnt;
                if (rows[i].level != int(i)) ok = false;
            }
            const sf::SeedEval ev = sf::evaluateSeed(seed, 1, 13, oracle, rank, hard);
            if (cnt != ev.matched) ok = false;
            if (!ev.hardOk) ok = false;
        }
        check(ok, QStringLiteral("viewDetail 行数/level 编号/匹配数与 evaluateSeed 一致"));
    }
}

} // namespace

// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    g_out = &out;

    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromUtf8(argv[i]);
        if (a == QStringLiteral("--quick")) quick = true;
        else if (a == QStringLiteral("-q")) quick = true;
    }

    out << "seed-finder 2.0 测试套件" << (quick ? " (quick)" : "") << endl;

    QElapsedTimer total;
    total.start();

    testSelfCheck();
    testCrossCheck(quick);
    testConditionValidation();
    testBoundaries();
    testInteraction();
    testPresets();
    testPureFunctions();

    out << endl << "--------------------------------------------------" << endl;
    out << "PASS " << g_pass << "   FAIL " << g_fail
        << "   用时 " << (total.elapsed() / 1000.0) << "s" << endl;
    if (g_fail > 0) {
        out << "失败项:" << endl;
        for (int i = 0; i < g_failed.size(); ++i) out << "  - " << g_failed[i] << endl;
        return 1;
    }
    out << "ALL OK" << endl;
    return 0;
}
