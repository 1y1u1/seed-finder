// diag5: 定位 Searcher::run 与"直接逐种子循环"的差异
#include "../../seed-finder-qt-main1.2/worker.cpp"
#include "../seedengine.h"

#include <QCoreApplication>

#include <cstdio>
#include <map>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const int uid = 1, mode = 13, scene = 2;
    const int lvBegin = 1, lvEnd = 9;
    const quint32 seedBegin = 0x1000u;
    const quint64 span = 20000;
    const quint32 need = (1u << Z_GIGA);

    sf::WaveCondition cond;
    cond.needMask = need;
    cond.sanitize();

    // 直接循环 (已知与原版一致)
    std::map<int, quint64> hDirect;
    sf::WaveOracle oracle(scene);
    const quint32 rValBegin = sf::seedToRVal(seedBegin, uid + mode);
    for (quint64 i = 0; i < span; ++i) {
        const quint32 rVal = rValBegin + quint32(i);
        int c = 0;
        for (int lv = lvBegin; lv < lvEnd; ++lv)
            if (sf::popcnt(oracle.bits(rVal + quint32(lv), lv) & need) == 1) ++c;
        ++hDirect[c];
    }

    auto histOf = [&](int threads) {
        sf::SearchSpec spec;
        spec.uid = uid;
        spec.mode = mode;
        spec.scene = scene;
        spec.beginFlag = lvBegin * 2;
        spec.endFlag = lvEnd * 2;
        spec.seedBegin = seedBegin;
        spec.span = span;
        spec.outputSize = 8;
        spec.threads = threads;
        sf::RuleBand band;
        band.beginFlag = lvBegin * 2;
        band.endFlag = lvEnd * 2;
        band.cond = cond;
        spec.bands << band;

        const sf::SearchResult r = sf::Searcher().run(spec, nullptr, nullptr);
        std::map<int, quint64> h;
        for (const sf::CountBucket &b : r.buckets) h[b.matched] = b.total;
        std::printf("  threads=%-2d activeWaves=%d wave=[%d,%d) scanned=%llu err='%s'\n", threads,
                    r.activeWaves, r.waveBegin, r.waveEnd, (unsigned long long)r.scanned,
                    r.error.toUtf8().constData());
        return h;
    };

    auto dump = [](const char *tag, const std::map<int, quint64> &h) {
        std::printf("%s:", tag);
        for (const auto &kv : h) std::printf(" %d:%llu", kv.first, (unsigned long long)kv.second);
        std::printf("\n");
    };

    dump("direct  ", hDirect);
    const std::map<int, quint64> h1 = histOf(1);
    dump("run(t=1)", h1);
    const std::map<int, quint64> hN = histOf(0);
    dump("run(auto)", hN);

    std::printf("direct == run(t=1)  : %s\n", hDirect == h1 ? "YES" : "NO");
    std::printf("direct == run(auto) : %s\n", hDirect == hN ? "YES" : "NO");

    // 逐种子定位: run(t=1) 的桶内容 vs direct
    // 取 spec 同款, 但用 evaluateSeed 逐种子检查
    sf::RuleTable table;
    QVector<sf::RuleBand> bands;
    {
        sf::RuleBand band;
        band.beginFlag = lvBegin * 2;
        band.endFlag = lvEnd * 2;
        band.cond = cond;
        bands << band;
    }
    table.build(bands, lvBegin, lvEnd);
    int bad = 0;
    for (quint64 i = 0; i < span; ++i) {
        const quint32 seed = seedBegin + quint32(i);
        const sf::SeedEval ev = sf::evaluateSeed(seed, uid, mode, oracle, table, sf::RuleTable());
        const quint32 rVal = rValBegin + quint32(i);
        int c = 0;
        for (int lv = lvBegin; lv < lvEnd; ++lv)
            if (sf::popcnt(oracle.bits(rVal + quint32(lv), lv) & need) == 1) ++c;
        if (ev.matched != c) {
            if (++bad <= 5)
                std::printf("  eval 差异 seed=0x%08X eval=%d direct=%d\n", seed, ev.matched, c);
        }
    }
    std::printf("evaluateSeed vs direct: bad=%d\n", bad);
    return 0;
}
