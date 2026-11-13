// 临时探针: 统计每个波次出现各种僵尸的种子比例, 用来挑合理的默认条件
#include "../seedengine.h"

#include <cstdio>

int main()
{
    const int scenes[] = {0, 1, 2, 4, 6};
    const char *sceneNames[] = {"DE", "NE", "PE", "RE", "MGE"};
    const int N = 200000;

    for (int si = 0; si < 5; ++si) {
        const int scene = scenes[si];
        sf::WaveOracle oracle(scene);
        const quint32 mask = quint32(sceneAllowMask(scene));

        // [level 1..10][zombie] 计数
        static quint64 cnt[11][20];
        for (int lv = 0; lv <= 10; ++lv)
            for (int z = 0; z < 20; ++z) cnt[lv][z] = 0;

        for (int i = 0; i < N; ++i) {
            const quint32 seed = quint32(i) * 7919u + 12345u;
            for (int lv = 1; lv <= 10; ++lv) {
                const quint32 bits = oracle.bitsOfSeed(seed, 14, lv);
                for (int z = 0; z < 20; ++z) {
                    if (bits & (1u << z)) ++cnt[lv][z];
                }
            }
        }

        std::printf("=== scene %s (allow=0x%08X) ===\n", sceneNames[si], mask);
        std::printf("wave ");
        for (int z = 0; z < 20; ++z) std::printf("%5s ", zombieName(z).toUtf8().constData());
        std::printf("\n");
        for (int lv = 1; lv <= 10; ++lv) {
            std::printf("%4d ", lv);
            for (int z = 0; z < 20; ++z) {
                std::printf("%5.1f ", 100.0 * double(cnt[lv][z]) / double(N));
            }
            std::printf("\n");
        }
        std::printf("\n");
    }
    return 0;
}
