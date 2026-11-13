// 诊断: 原版 d_page 的页缓存复用是否影响结果
//   对每个 r_val, 比较
//     A) 复用同一个 d_page 连续扫描 (原版 work() 的做法)
//     B) 每个种子新建 d_page (等价于"无缓存")
//     C) v3 WaveOracle
#include "ref12/worker.cpp"
#include "../seedengine.h"

#include <QCoreApplication>
#include <QRandomGenerator>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const int scene = 2;
    const quint32 rValBegin = sf::seedToRVal(0x1000u, 14);
    const int span = 20000;
    const int needMask = (1 << GIGA);
    const int refuseMask = 0, atLeastMask = 0, atMostMask = 0;

    // A: 复用
    d_page reuse(scene);
    // C
    sf::WaveOracle oracle(scene);

    int diffAB = 0, diffAC = 0, firstAB = -1, firstAC = -1;
    for (int i = 0; i < span; ++i) {
        const quint32 rVal = rValBegin + quint32(i);
        const int a = single_seed(rVal, reuse, needMask, refuseMask, atLeastMask, atMostMask, 0, 0, 1, 9);

        d_page fresh(scene);
        const int b = single_seed(rVal, fresh, needMask, refuseMask, atLeastMask, atMostMask, 0, 0, 1, 9);

        int c = 0;
        for (int lv = 1; lv < 9; ++lv) {
            const quint32 bits = oracle.bits(rVal + quint32(lv), lv);
            if (sf::popcnt(bits & quint32(needMask)) == 1 && !(bits & quint32(refuseMask))) ++c;
        }

        if (a != b) { ++diffAB; if (firstAB < 0) firstAB = i; }
        if (a != c) { ++diffAC; if (firstAC < 0) firstAC = i; }
    }

    std::printf("reuse vs fresh : diff=%d first=%d\n", diffAB, firstAB);
    std::printf("reuse vs v3    : diff=%d first=%d\n", diffAC, firstAC);

    if (firstAB >= 0) {
        const quint32 rVal = rValBegin + quint32(firstAB);
        d_page fresh(scene);
        std::printf("  seed=0x%08X  reuse=%d fresh=%d\n", sf::rValToSeed(rVal, 14),
                    single_seed(rVal, reuse, needMask, refuseMask, atLeastMask, atMostMask, 0, 0, 1, 9),
                    single_seed(rVal, fresh, needMask, refuseMask, atLeastMask, atMostMask, 0, 0, 1, 9));
    }
    return 0;
}
