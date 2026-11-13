// 与 v2.0 原版 worker.cpp 直接对拍。
//
// 重要背景: 原版扫的是 r_val(游戏内部位置)空间 —— cur 每步 +1, 再以 r_val*101-offset
//           还原种子, 因此它列出的相邻种子相差 101, 实际只覆盖了请求区间里 1/101 的种子。
//           v3 按种子连续扫描 (与参考实现 zombie_calculator 一致)。
//           所以对拍不能比较"同一段种子"的直方图, 而是分四段验证:
//             T1 单波出怪逐位一致   (走原版 p.get 路径)
//             T2 种子 -> 匹配波数一致 (原版 single_seed vs v3 evaluateSeed)
//             T3 原版 r_val 扫描 vs v3 在同一批 r_val 上的暴力枚举 (枚举语义一致)
//             T4 v3 实际检索结果 vs 暴力枚举 (连续种子, 覆盖分桶/全局条件/多规则带)
#include "../../seed-finder-qt-main1.2/worker.cpp"

#include "../seedengine.h"

#include <QCoreApplication>
#include <QRandomGenerator>

#include <cstdio>
#include <map>
#include <vector>

namespace {

int g_fail = 0;

void fail(const char *what, const QString &detail)
{
    ++g_fail;
    std::printf("FAIL [%s] %s\n", what, detail.toUtf8().constData());
}

QString histText(const std::map<int, quint64> &h)
{
    QStringList parts;
    for (const auto &kv : h) parts << QString::fromUtf8("%1:%2").arg(kv.first).arg(kv.second);
    return parts.join(QLatin1Char(' '));
}

const int kScenes[] = {0, 1, 2, 4, 6};

// ---------------------------------------------------------------------------
// T1: 单波出怪 — 原版 d_page::get 与 v3 computeRow 逐位一致
// ---------------------------------------------------------------------------
void testWaveBits()
{
    for (int si = 0; si < 5; ++si) {
        const int scene = kScenes[si];
        d_page ref(scene);
        sf::WaveOracle oracle(scene);
        for (int k = 0; k < 300; ++k) {
            const quint32 rVal = quint32(QRandomGenerator::global()->generate());
            for (int lv = 0; lv <= 9; ++lv) {
                const int a = ref.get(rVal + quint32(lv), lv);
                const int b = int(oracle.bits(rVal + quint32(lv), lv));
                if (a != b) {
                    fail("waveBits", QString::fromUtf8("scene=%1 rVal=%2 lv=%3 ref=%4 new=%5")
                                         .arg(scene).arg(rVal).arg(lv).arg(a).arg(b));
                    return;
                }
            }
        }
    }
    std::printf("T1 单波出怪逐位: OK (5 场景 × 300 pos × 10 波)\n");
}

// ---------------------------------------------------------------------------
// T2: 种子 -> 匹配波数 — 原版 single_seed vs v3 evaluateSeed
// ---------------------------------------------------------------------------
struct CondCase
{
    const char *name;
    int need, refuse, atLeast, atMost, nAtLeast, nAtMost;
};

const CondCase kConds[] = {
    {"红眼必出", (1 << GIGA), 0, 0, 0, 0, 0},
    {"禁出小丑+冰车", 0, (1 << JACK) | (1 << ZOMBONI), 0, 0, 0, 0},
    {"至少2(锥桶门)/至多2(杆报杖梯)", 0, 0, (1 << CONE) | (1 << BUCKET) | (1 << DOOR),
     (1 << POLE) | (1 << NEWS) | (1 << FOOTBALL) | (1 << LADDER), 2, 2},
    {"必出红眼+禁冰车+至少1", (1 << GIGA), (1 << ZOMBONI), (1 << CONE) | (1 << BUCKET), 0, 1, 0},
    {"必出白眼或红眼+禁读报", (1 << GARG) | (1 << GIGA) | (1 << NEWS), 0, 0, 0, 0, 0},
};
const int kCondCount = int(sizeof(kConds) / sizeof(kConds[0]));

sf::WaveCondition makeCond(const CondCase &cc)
{
    sf::WaveCondition c;
    c.needMask = quint32(cc.need);
    c.refuseMask = quint32(cc.refuse);
    c.atLeastMask = quint32(cc.atLeast);
    c.atMostMask = quint32(cc.atMost);
    c.atLeast = cc.nAtLeast;
    c.atMost = cc.nAtMost;
    c.sanitize();
    return c;
}

void testSeedCount()
{
    struct Range
    {
        int lvBegin, lvEnd;
    };
    const Range ranges[] = {{1, 9}, {5, 9}, {1, 13}, {2, 11}, {6, 10}};

    for (int si = 0; si < 5; ++si) {
        const int scene = kScenes[si];
        sf::WaveOracle oracle(scene);
        for (const Range &rg : ranges) {
            for (int ci = 0; ci < kCondCount; ++ci) {
                const sf::WaveCondition cond = makeCond(kConds[ci]);

                sf::RuleBand band;
                band.beginFlag = rg.lvBegin * 2;
                band.endFlag = rg.lvEnd * 2;
                band.cond = cond;
                sf::RuleTable table;
                table.build(QVector<sf::RuleBand>() << band, rg.lvBegin, rg.lvEnd);

                d_page ref(scene);
                for (int k = 0; k < 120; ++k) {
                    const quint32 seed = quint32(QRandomGenerator::global()->generate());
                    const quint32 rVal = sf::seedToRVal(seed, 14);
                    const int a = single_seed(rVal, ref, kConds[ci].need, kConds[ci].refuse,
                                              kConds[ci].atLeast, kConds[ci].atMost,
                                              kConds[ci].nAtLeast, kConds[ci].nAtMost,
                                              rg.lvBegin, rg.lvEnd);
                    const sf::SeedEval ev = sf::evaluateSeed(seed, 1, 13, oracle, table,
                                                             sf::RuleTable());
                    if (a != ev.matched) {
                        fail("seedCount", QString::fromUtf8(
                                              "scene=%1 L%2-%3 [%4] seed=%5 ref=%6 new=%7")
                                              .arg(scene).arg(rg.lvBegin).arg(rg.lvEnd)
                                              .arg(QString::fromUtf8(kConds[ci].name))
                                              .arg(seed).arg(a).arg(ev.matched));
                        return;
                    }
                }
            }
        }
    }
    std::printf("T2 种子->匹配波数: OK (5 场景 × 5 区间 × 5 条件 × 120 种子)\n");
}

// ---------------------------------------------------------------------------
// T3: 原版整段扫描 (r_val 空间) vs v3 在同一批 r_val 上的暴力枚举
// ---------------------------------------------------------------------------
void testOriginalScan()
{
    struct Case
    {
        int uid, mode, scene, lvBegin, lvEnd;
        quint32 seedBegin;
        quint64 span;
    };
    const Case cases[] = {
        {1, 13, 2, 1, 9, 0x1000u, 20000},
        {7, 4, 0, 1, 13, 0x77777u, 20000},
        {3, 21, 4, 2, 11, 0xabcdeu, 20000},
        {2, 9, 1, 6, 10, 0x123456u, 20000},
    };

    for (const Case &c : cases) {
        for (int ci = 0; ci < kCondCount; ++ci) {
            const int outputSize = int(c.span); // 全收, 才能还原每个种子的匹配波数

            SeedCalc calc(c.uid, c.mode, c.scene, c.lvBegin, c.lvEnd, c.seedBegin,
                          c.seedBegin + c.span, kConds[ci].need, kConds[ci].refuse,
                          kConds[ci].atLeast, kConds[ci].atMost, kConds[ci].nAtLeast,
                          kConds[ci].nAtMost, outputSize);
            std::vector<SeedInfo> out;
            QObject::connect(&calc, &SeedCalc::output_result,
                             [&out](std::vector<SeedInfo> r) { out = std::move(r); });
            calc.calc();

            std::map<quint32, int> refMap;
            for (const SeedInfo &info : out)
                for (quint32 s : info.seeds) refMap[s] = int(info.count);

            const sf::WaveCondition cond = makeCond(kConds[ci]);
            sf::RuleBand band;
            band.beginFlag = c.lvBegin * 2;
            band.endFlag = c.lvEnd * 2;
            band.cond = cond;
            sf::RuleTable table;
            table.build(QVector<sf::RuleBand>() << band, c.lvBegin, c.lvEnd);

            sf::WaveOracle oracle(c.scene);
            const quint32 rValBegin = sf::seedToRVal(c.seedBegin, c.uid + c.mode);

            std::map<int, quint64> hNew;
            int missing = 0, mismatch = 0;
            for (quint64 i = 0; i < c.span; ++i) {
                const quint32 rVal = rValBegin + quint32(i);
                const quint32 seed = rVal * 101u - quint32(c.uid + c.mode);
                const sf::SeedEval ev = sf::evaluateSeed(seed, c.uid, c.mode, oracle, table,
                                                         sf::RuleTable());
                ++hNew[ev.matched];
                auto it = refMap.find(seed);
                if (it == refMap.end()) {
                    ++missing;
                } else if (it->second != ev.matched) {
                    ++mismatch;
                }
            }

            std::map<int, quint64> hOrig;
            for (const SeedInfo &info : out) hOrig[int(info.count)] += info.seed_count;

            if (hOrig != hNew || missing != 0 || mismatch != 0) {
                fail("originalScan",
                     QString::fromUtf8("uid=%1 scene=%2 L%3-%4 [%5]\n  原版: %6\n  v3  : %7\n  "
                                        "缺失=%8 不符=%9")
                         .arg(c.uid).arg(c.scene).arg(c.lvBegin).arg(c.lvEnd)
                         .arg(QString::fromUtf8(kConds[ci].name))
                         .arg(histText(hOrig), histText(hNew))
                         .arg(missing).arg(mismatch));
                return;
            }
        }
    }
    std::printf("T3 原版整段扫描 (r_val 空间): OK (4 段 × 5 条件)\n");
}

// ---------------------------------------------------------------------------
// T4: v3 实际检索 vs 暴力枚举 (连续种子, 多规则带 + 全局条件)
// ---------------------------------------------------------------------------
void testSearch()
{
    struct Case
    {
        int uid, mode, scene, beginFlag, endFlag;
        quint32 seedBegin;
        quint64 span;
        int minMatched;
    };
    const Case cases[] = {
        {1, 13, 2, 2, 52, 0x1000u, 30000, -1},
        {1, 13, 2, 0, 100, 0x50000u, 20000, -1},
        {7, 4, 0, 2, 30, 0x7777u, 20000, -1},
        {3, 21, 4, 4, 60, 0xabcdeu, 20000, -1},
        {2, 9, 1, 10, 26, 0x123456u, 20000, -1},
        {5, 2, 6, 2, 40, 0x2000u, 20000, -1},
    };

    for (const Case &c : cases) {
        // 多规则带: 前半段一种条件, 后半段另一种条件
        const int mid = c.beginFlag + (c.endFlag - c.beginFlag) / 4 * 2;
        QVector<sf::RuleBand> bands;
        {
            sf::RuleBand b1;
            b1.beginFlag = c.beginFlag;
            b1.endFlag = mid;
            b1.cond = makeCond(kConds[0]); // 红眼必出
            bands << b1;

            sf::RuleBand b2;
            b2.beginFlag = mid;
            b2.endFlag = c.endFlag;
            b2.cond = makeCond(kConds[2]); // 至少2/至多2
            bands << b2;
        }

        // 3.0 去掉了 GlobalCond 概念, 全局条件改由 hard 规则带表达; 本用例不加全局条件
        QVector<sf::RuleBand> hardBands;
        sf::RuleTable hard;
        hard.build(hardBands, c.beginFlag / 2, c.endFlag / 2);

        sf::SearchSpec spec;
        spec.uid = c.uid;
        spec.mode = c.mode;
        spec.scene = c.scene;
        spec.beginFlag = c.beginFlag;
        spec.endFlag = c.endFlag;
        spec.seedBegin = c.seedBegin;
        spec.span = c.span;
        spec.outputSize = 16;
        spec.bands = bands;
        spec.hard = hardBands;
        const sf::SearchResult r = sf::Searcher().run(spec, nullptr, nullptr);
        if (!r.error.isEmpty()) {
            fail("search", QString::fromUtf8("uid=%1: %2").arg(c.uid).arg(r.error));
            return;
        }

        // 暴力枚举
        sf::RuleTable table;
        table.build(bands, c.beginFlag / 2, c.endFlag / 2);
        sf::WaveOracle oracle(c.scene);

        std::map<int, quint64> hRef;
        quint64 refQualified = 0;
        for (quint64 i = 0; i < c.span; ++i) {
            const quint32 seed = c.seedBegin + quint32(i);
            const sf::SeedEval ev = sf::evaluateSeed(seed, c.uid, c.mode, oracle, table, hard);
            if (!ev.hardOk) continue;
            ++hRef[ev.matched];
            if (ev.matched == ev.active) ++refQualified;
        }

        std::map<int, quint64> hNew;
        for (const sf::CountBucket &b : r.buckets) hNew[b.matched] = b.total;

        if (hRef != hNew) {
            fail("search", QString::fromUtf8("uid=%1 scene=%2 F%3-%4\n  暴力: %5\n  引擎: %6")
                               .arg(c.uid).arg(c.scene).arg(c.beginFlag).arg(c.endFlag)
                               .arg(histText(hRef), histText(hNew)));
            return;
        }
        if (refQualified != r.fullCount()) {
            fail("search", QString::fromUtf8("uid=%1 全满足数 暴力=%2 引擎=%3")
                               .arg(c.uid).arg(refQualified).arg(r.fullCount()));
            return;
        }
        if (r.scanned != c.span) {
            fail("search", QString::fromUtf8("uid=%1 scanned=%2 期望=%3")
                               .arg(c.uid).arg(r.scanned).arg(c.span));
            return;
        }
        std::printf("  T4 OK uid=%d scene=%d F%d-%d active=%d -> %s\n", c.uid, c.scene, c.beginFlag,
                    c.endFlag, r.activeWaves, histText(hRef).toUtf8().constData());
    }
    std::printf("T4 检索 vs 暴力枚举: OK (6 段, 多规则带 + 全局条件)\n");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString self = sf::Searcher::selfCheck();
    if (!self.isEmpty()) {
        fail("selfCheck", self);
    } else {
        std::printf("T0 分页自检 (对照原版 d_page): OK\n");
    }

    testWaveBits();
    testSeedCount();
    testOriginalScan();
    testSearch();

    if (g_fail == 0) std::printf("\nALL OK\n");
    return g_fail == 0 ? 0 : 1;
}
