// 精确定位: 原版整段扫描 (SeedCalc, 多线程) 与 v3 的逐种子差异
//   技巧: outputSize 设成 >= 段长, 原版就会把每个 count 桶里的全部种子都存下来,
//         于是可以还原出"每个种子 -> 匹配波数", 做逐种子比对
#include "ref12/worker.cpp"
#include "seedengine.h"

#include <QCoreApplication>

#include <cstdio>
#include <map>
#include <vector>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const int uid = 1, mode = 13, scene = 2;
    const int lvBegin = 1, lvEnd = 9;
    const quint32 seedBegin = 0x1000u;
    const quint64 span = 20000;
    const int needMask = (1 << GIGA);

    // ---- 原版 ----
    SeedCalc calc(uid, mode, scene, lvBegin, lvEnd, seedBegin, seedBegin + span, needMask, 0, 0, 0,
                  0, 0, int(span));
    std::vector<SeedInfo> out;
    QObject::connect(&calc, &SeedCalc::output_result,
                     [&out](std::vector<SeedInfo> r) { out = std::move(r); });
    calc.calc();

    std::map<quint32, int> refCount;   // seed -> matched
    for (const SeedInfo &info : out)
        for (quint32 s : info.seeds) refCount[s] = int(info.count);
    std::printf("原版收集到 %zu 个种子 (期望 %llu)\n", refCount.size(),
                static_cast<unsigned long long>(span));

    // ---- v3 ----
    sf::WaveOracle oracle(scene);
    const quint32 rValBegin = sf::seedToRVal(seedBegin, uid + mode);

    // ---- 直接复用 d_page 的正确参考 (逐种子) ----
    d_page reuse(scene);

    int diff = 0;
    std::map<quint32, int> badSeeds;
    for (quint64 i = 0; i < span; ++i) {
        const quint32 rVal = rValBegin + quint32(i);
        const quint32 seed = sf::rValToSeed(rVal, uid + mode);

        int v3 = 0;
        for (int lv = lvBegin; lv < lvEnd; ++lv) {
            const quint32 bits = oracle.bits(rVal + quint32(lv), lv);
            if (sf::popcnt(bits & quint32(needMask)) == 1) ++v3;
        }

        auto it = refCount.find(seed);
        const int ref = (it == refCount.end()) ? -1 : it->second;
        if (ref != v3) {
            ++diff;
            badSeeds[seed] = v3;
            if (diff <= 5) {
                const int direct = single_seed(rVal, reuse, needMask, 0, 0, 0, 0, 0, lvBegin, lvEnd);
                std::printf("  seed=0x%08X page=%u  原版=%d v3=%d 直连d_page=%d\n", seed,
                            unsigned(rVal >> 10), ref, v3, direct);
            }
        }
    }
    std::printf("逐种子差异 %d / %llu\n", diff, static_cast<unsigned long long>(span));

    // 差异种子的页分布
    if (!badSeeds.empty()) {
        std::map<unsigned, int> pages;
        for (const auto &kv : badSeeds) pages[unsigned(sf::seedToRVal(kv.first, uid + mode) >> 10)]++;
        std::printf("差异集中页: ");
        for (const auto &kv : pages) std::printf("%u(%d) ", kv.first, kv.second);
        std::printf("\n");
    }
    return 0;
}
