// 结构探针: 各场景 / 各轮的出怪位不变量
//
// 用途: 给条件校验器提供"真值"。校验器里有若干断言式的话术 (例如
// "红眼最早要到第 6 轮才会出现"、"本轮必出的僵尸太多"), 这些断言成立与否
// 可以直接用抽样统计来核对 —— 只要在某一轮里真的抽到了某种位组合, 就说明
// "这一轮不可能出现这种组合"之类的结论是错的。
//
// 输出:
//   · 每轮 (level 1..9) 出现过的不同位模式数量
//   · 每轮种类数 (popcount) 的最小/最大值
//   · 从未出现过的僵尸 (本场景下根本抽不到)
//   · level 1 的全部位模式 (用来核对 "第 1 轮固定是什么")
#include <algorithm>
#include <cstdio>
#include <vector>

#include <QCoreApplication>

#include "seedengine.h"
#include "zombiedata.h"

namespace {

struct Stat
{
    int minKinds = 99;
    int maxKinds = -1;
    std::vector<quint32> patterns;   // 去重后的位模式
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const int samples = (argc > 1) ? qMax(1, QString::fromUtf8(argv[1]).toInt()) : 20000;

    const QVector<int> scenes = sceneIds();
    const QStringList displayNames = sceneDisplayNames();
    for (int si = 0; si < scenes.size(); ++si) {
        const int scene = scenes.at(si);

        // level 0..9: bits 按 computeRow 的结果逐轮取
        Stat st[10];
        quint32 everSeen = 0;
        for (int i = 0; i < samples; ++i) {
            const quint32 pos = quint32(i) * 2654435761u + 12345u;
            quint32 d1[10];
            sf::computeRow(pos, scene, d1);
            for (int lv = 0; lv <= 9; ++lv) {
                const quint32 bits = sf::rowBits(d1, lv);
                st[lv].patterns.push_back(bits);
                everSeen |= bits;
                const int n = sf::popcnt(bits);
                if (n < st[lv].minKinds) st[lv].minKinds = n;
                if (n > st[lv].maxKinds) st[lv].maxKinds = n;
            }
        }

        std::printf("\nscene=%d  (%s)  samples=%d\n", scene,
                    si < displayNames.size() ? displayNames.at(si).toUtf8().constData() : "?",
                    samples);

        for (int lv = 0; lv <= 9; ++lv) {
            std::vector<quint32> &p = st[lv].patterns;
            std::sort(p.begin(), p.end());
            p.erase(std::unique(p.begin(), p.end()), p.end());
            std::printf("  level %d: kinds %d..%d, %d 种不同位模式\n", lv,
                        st[lv].minKinds, st[lv].maxKinds, int(p.size()));
        }

        // 从未出现过的僵尸
        QStringList missing;
        for (int z = 0; z < Z_COUNT; ++z) {
            if (!(everSeen & (1u << z))) missing << zombieName(z);
        }
        std::printf("  从未出现: %s\n",
                    missing.isEmpty() ? "(无)" : missing.join(QString::fromUtf8("、")).toUtf8().constData());

        // level 1 的全部模式
        std::printf("  level 1 模式:");
        for (size_t k = 0; k < st[1].patterns.size(); ++k) {
            if (k < 12) std::printf(" 0x%03X", st[1].patterns[k]);
        }
        if (st[1].patterns.size() > 12) std::printf(" ...");
        std::printf("\n");
    }
    return 0;
}
