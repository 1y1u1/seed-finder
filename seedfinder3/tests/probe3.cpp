// 探针: 实测每波最多同时出现多少种僵尸, 用于 checkConditions 的容量上限。
// 用法: probe3.exe [scene]
#include "seedengine.h"

#include <QCoreApplication>
#include <QRandomGenerator>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    QCoreApplication a(argc, argv);
    const int scene = argc > 1 ? atoi(argv[1]) : 2;   // 默认 PE/FE (全僵尸)

    sf::WaveOracle oracle(scene);
    int maxKinds[11];
    int minKinds[11];
    for (int i = 0; i <= 10; ++i) { maxKinds[i] = 0; minKinds[i] = 99; }

    const int kSamples = 200000;
    for (int k = 0; k < kSamples; ++k) {
        const quint32 pos = quint32(QRandomGenerator::global()->generate());
        for (int lv = 0; lv <= 9; ++lv) {
            const quint32 bits = oracle.bits(pos + quint32(lv), lv);
            const int n = sf::popcnt(bits);
            const int wave = lv + 1;
            if (n > maxKinds[wave]) maxKinds[wave] = n;
            if (n < minKinds[wave]) minKinds[wave] = n;
        }
    }

    std::printf("scene=%d  samples=%d\n", scene, kSamples);
    for (int w = 1; w <= 10; ++w) {
        std::printf("wave %2d: kinds min=%d max=%d\n", w, minKinds[w], maxKinds[w]);
    }
    return 0;
}
