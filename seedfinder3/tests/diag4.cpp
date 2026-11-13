// diag4:
//   A) 从 SeedCalc 逐种子还原直方图, 与 v3 直方图直接对比 (同一进程, 排除构建差异)
//   B) 定点验证原版 d_page 的"后退一页"(t == cur - 1)分支:
//      先把 cur 推到偶数页, 再请求 cur-1 页, 看返回的数据是否等于该页真实数据
#include "ref12/worker.cpp"
#include "../seedengine.h"

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

    // ---------------- A) 直方图对比 ----------------
    SeedCalc calc(uid, mode, scene, lvBegin, lvEnd, seedBegin, seedBegin + span, needMask, 0, 0, 0,
                  0, 0, int(span));
    std::vector<SeedInfo> out;
    QObject::connect(&calc, &SeedCalc::output_result,
                     [&out](std::vector<SeedInfo> r) { out = std::move(r); });
    calc.calc();

    std::map<int, quint64> hOrig, hNew;
    for (const SeedInfo &info : out) hOrig[int(info.count)] += info.seed_count;

    sf::WaveOracle oracle(scene);
    const quint32 rValBegin = sf::seedToRVal(seedBegin, uid + mode);
    for (quint64 i = 0; i < span; ++i) {
        const quint32 rVal = rValBegin + quint32(i);
        int c = 0;
        for (int lv = lvBegin; lv < lvEnd; ++lv)
            if (sf::popcnt(oracle.bits(rVal + quint32(lv), lv) & quint32(needMask)) == 1) ++c;
        ++hNew[c];
    }

    auto dump = [](const char *tag, const std::map<int, quint64> &h) {
        std::printf("%s:", tag);
        for (const auto &kv : h) std::printf(" %d:%llu", kv.first, (unsigned long long)kv.second);
        std::printf("\n");
    };
    dump("A-orig(SeedCalc)", hOrig);
    dump("A-new(v3)        ", hNew);
    std::printf("A 直方图 %s\n\n", hOrig == hNew ? "一致" : "不一致");

    // ---------------- B) 定点验证后退一页 ----------------
    // 取一个偶数页 t, 先请求 t 页的某个 pos 让 cur = t, 再请求 t-1 页
    // 期望: 返回的数据 == 直接按该 pos 算出的真实数据
    for (int evenPage = 2; evenPage <= 8; evenPage += 2) {
        const quint32 posA = (quint32(evenPage) << 10) + 5;   // 偶数页 t
        const quint32 posB = (quint32(evenPage - 1) << 10) + 5; // 后退一页 t-1

        d_page p(scene);
        const int lv = 3;
        const int gotA = p.get(posA, lv);   // cur 变成 t
        const int gotB = p.get(posB, lv);   // 走 t == cur - 1 分支

        const int wantB = int(oracle.bits(posB, lv));
        std::printf("B page %d -> %d : 原版 get=%d  真实=%d  %s\n", evenPage, evenPage - 1, gotB,
                    wantB, (gotB == wantB) ? "OK" : "**不符**");
        (void)gotA;
    }
    return 0;
}
