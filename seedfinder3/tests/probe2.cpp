// 探针: 实测各类僵尸在每波出现的最早波号, 用于确定 checkConditions 的拦截阈值。
// 用法: probe2.exe [scene]
#include "../seedengine.h"

#include <QCoreApplication>
#include <QRandomGenerator>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    QCoreApplication a(argc, argv);
    const int scene = argc > 1 ? atoi(argv[1]) : 2;   // 默认 PE/FE (全僵尸)

    sf::WaveOracle oracle(scene);

    // earliest[z] = 在采样范围内最早出现该僵尸的波号 (1-based)
    int earliest[20];
    for (int i = 0; i < 20; ++i) earliest[i] = 999;

    const int kSamples = 30000;
    for (int k = 0; k < kSamples; ++k) {
        const quint32 pos = quint32(QRandomGenerator::global()->generate());
        for (int lv = 0; lv <= 9; ++lv) {
            const quint32 bits = oracle.bits(pos + quint32(lv), lv);
            const int wave = lv + 1;
            for (int z = 0; z < 20; ++z) {
                if ((bits & (1u << z)) && wave < earliest[z]) earliest[z] = wave;
            }
        }
    }

    std::printf("scene=%d  samples=%d\n", scene, kSamples);
    for (int z = 0; z < 20; ++z) {
        std::printf("wave-earliest %2d %s = %d\n", z,
                    zombieName(z).toUtf8().constData(), earliest[z]);
    }
    return 0;
}
