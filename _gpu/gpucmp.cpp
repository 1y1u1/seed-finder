// 对比 GPU 路径与 CPU 路径的结果是否逐位一致。
//
// 用法: gpucmp [种子数] [波数]
// 没接上 GPU 后端时直接报 no gpu backend 并退出。
#include "gpuhook.h"
#include "seedengine.h"
#include "zombiedata.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// 恒真条件: 至多出 19 个冰冻僵尸 (每波最多 10 张卡, 抽屉原理下恒成立)。
// 用它做 autoStop 对拍, 保证"找到第 N 个全满足的种子"真的会发生。
sf::RuleBand alwaysTrue(int beginFlag, int endFlag)
{
    sf::RuleBand b;
    b.beginFlag = beginFlag;
    b.endFlag = endFlag;
    b.cond.atMostMask = 1u << Z_CONE;
    b.cond.atMost = 19;
    b.cond.sanitize();
    return b;
}

sf::RuleBand band(int beginFlag, int endFlag, quint32 need, quint32 refuse)
{
    sf::RuleBand b;
    b.beginFlag = beginFlag;
    b.endFlag = endFlag;
    b.cond.needMask = need;
    b.cond.refuseMask = refuse;
    b.cond.sanitize();
    return b;
}

bool sameBuckets(const sf::SearchResult &a, const sf::SearchResult &b, const char *tag)
{
    bool ok = true;
    if (a.scanned != b.scanned) {
        std::printf("  [%s] scanned differs: cpu=%llu gpu=%llu\n", tag,
                    (unsigned long long)a.scanned, (unsigned long long)b.scanned);
        ok = false;
    }
    if (a.qualified != b.qualified) {
        std::printf("  [%s] qualified differs: cpu=%llu gpu=%llu\n", tag,
                    (unsigned long long)a.qualified, (unsigned long long)b.qualified);
        ok = false;
    }
    if (a.buckets.size() != b.buckets.size()) {
        std::printf("  [%s] bucket count differs: cpu=%d gpu=%d\n", tag,
                    int(a.buckets.size()), int(b.buckets.size()));
        return false;
    }
    for (int i = 0; i < a.buckets.size(); ++i) {
        const sf::CountBucket &x = a.buckets.at(i);
        const sf::CountBucket &y = b.buckets.at(i);
        if (x.matched != y.matched || x.total != y.total) {
            std::printf("  [%s] bucket m=%d differs: cpu total=%llu gpu total=%llu\n",
                        tag, x.matched,
                        (unsigned long long)x.total, (unsigned long long)y.total);
            ok = false;
            continue;
        }
        // 不用 QVector::operator== —— Qt 5.12 的实现在 MSVC 14.52 下会去调
        // stdext::make_checked_array_iterator, 那个 API 已被删除, 实例化就编不过。
        if (x.examples.size() != y.examples.size()) {
            std::printf("  [%s] bucket m=%d example count differs\n", tag, x.matched);
            ok = false;
            continue;
        }
        for (int k = 0; k < x.examples.size(); ++k) {
            if (x.examples.at(k) != y.examples.at(k)) {
                std::printf("  [%s] bucket m=%d example %d differs: cpu=%08X gpu=%08X\n",
                            tag, x.matched, k, x.examples.at(k), y.examples.at(k));
                ok = false;
            }
        }
    }
    return ok;
}

} // namespace

int main(int argc, char *argv[])
{
    sf::GpuBackend *backend = sf::createGpuBackend();
    if (!backend) {
        std::printf("no gpu backend\n");
        return 2;
    }
    sf::setGpuBackend(backend);

    const quint64 span = (argc > 1) ? quint64(std::strtoull(argv[1], nullptr, 10)) : 3000000ull;
    const int waves = (argc > 2) ? std::atoi(argv[2]) : 25;

    int bad = 0;
    int cases = 0;

    for (int scenePass = 0; scenePass < 3; ++scenePass) {
        const int scene = (scenePass == 0) ? 2 : (scenePass == 1 ? 4 : 0);

        for (int condPass = 0; condPass < 4; ++condPass) {
            sf::SearchSpec spec;
            spec.uid = 1;
            spec.mode = 13;
            spec.scene = scene;
            spec.beginFlag = 0;
            spec.endFlag = 2 * waves;
            spec.span = span;
            spec.outputSize = 20;
            spec.threads = 0;

            switch (condPass) {
            case 0:     // 只有排名条件, 覆盖多半波
                spec.bands << band(0, 2 * waves, 1u << Z_GIGA, 0);
                break;
            case 1:     // 排名 + 硬性
                spec.bands << band(0, 2 * waves, 1u << Z_GIGA, 0);
                spec.hard << band(0, 2 * waves, (1u << Z_POLE) | (1u << Z_NEWS), 0);
                break;
            case 2:     // 只有硬性 (activeWaves == 0, 走 matched==0 桶)
                spec.hard << band(0, 2 * waves, (1u << Z_POLE) | (1u << Z_NEWS), 0);
                break;
            default:    // 稀疏条件: 只查第 7 轮, 制造少量命中
                spec.bands << band(2 * (waves - 1), 2 * waves, 1u << Z_GIGA, 0);
                break;
            }

            ++cases;
            spec.gpu = 0;
            const sf::SearchResult cpu = sf::Searcher().run(spec, nullptr, nullptr);
            spec.gpu = 1;
            const sf::SearchResult gpu = sf::Searcher().run(spec, nullptr, nullptr);

            if (!cpu.error.isEmpty() || !gpu.error.isEmpty()) {
                std::printf("  [s%d c%d] error cpu=%s gpu=%s\n", scene, condPass,
                            cpu.error.toUtf8().constData(), gpu.error.toUtf8().constData());
                ++bad;
                continue;
            }

            char tag[64];
            std::snprintf(tag, sizeof(tag), "s%d c%d", scene, condPass);
            const bool ok = sameBuckets(cpu, gpu, tag);
            std::printf("  %-8s cpu: %8.1f ms %6.2f M/s  gpu: %8.1f ms %6.2f M/s  %s\n",
                        tag,
                        cpu.seconds * 1000.0, double(cpu.scanned) / cpu.seconds / 1e6,
                        gpu.seconds * 1000.0, double(gpu.scanned) / gpu.seconds / 1e6,
                        ok ? "OK" : "MISMATCH");
            if (!ok) ++bad;
        }
    }

    // 提前停止: autoStop 生效时两端都必须停在"同一个种子"上。
    // CPU 侧强制单线程, 否则各线程的块边界不同, 停下来时扫过的种子集天然不一样,
    // 拿它跟单执行流的 GPU 比没有意义。
    for (int scenePass = 0; scenePass < 3; ++scenePass) {
        const int scene = (scenePass == 0) ? 2 : (scenePass == 1 ? 4 : 0);
        for (int condPass = 0; condPass < 2; ++condPass) {
            sf::SearchSpec spec;
            spec.uid = 1;
            spec.mode = 13;
            spec.scene = scene;
            spec.beginFlag = 0;
            spec.endFlag = 2 * waves;
            spec.span = span;
            spec.outputSize = 20;
            spec.threads = 1;          // 与 GPU 的单执行流对齐
            spec.autoStop = 20;        // 找到 20 个"全部满足"就停
            // 条件必须"几乎所有种子都满足", 提前停止才会真的触发 ——
            // 用"必出红眼"覆盖全区间的话, 25 轮同时有红眼的种子实际上一个都没有,
            // 两端都会扫满整个区间, 这样比出来的 scanned 相等毫无意义。
            // ("禁出普通僵尸"也不行: bits 里带 d1[0] 传下来的 Z_REGULAR 位, 那会全判死。)
            // 用"至多出 19 个冰冻僵尸"这种恒真条件。
            spec.bands << alwaysTrue(0, 2 * waves);
            if (condPass == 1)
                spec.hard << alwaysTrue(0, 2 * waves);

            ++cases;
            spec.gpu = 0;
            const sf::SearchResult cpu = sf::Searcher().run(spec, nullptr, nullptr);
            spec.gpu = 1;
            const sf::SearchResult gpu = sf::Searcher().run(spec, nullptr, nullptr);

            char tag[64];
            std::snprintf(tag, sizeof(tag), "s%d stop%d", scene, condPass);
            if (!cpu.error.isEmpty() || !gpu.error.isEmpty()) {
                std::printf("  [%s] error cpu=%s gpu=%s\n", tag,
                            cpu.error.toUtf8().constData(), gpu.error.toUtf8().constData());
                ++bad;
                continue;
            }
            bool ok = sameBuckets(cpu, gpu, tag);
            if (cpu.scanned != gpu.scanned) {
                std::printf("  [%s] StopAt differs: cpu=%llu gpu=%llu\n", tag,
                            (unsigned long long)cpu.scanned, (unsigned long long)gpu.scanned);
                ok = false;
            }
            if (cpu.enoughFound != gpu.enoughFound) {
                std::printf("  [%s] enoughFound differs: cpu=%d gpu=%d\n", tag,
                            int(cpu.enoughFound), int(gpu.enoughFound));
                ok = false;
            }
            std::printf("  %-10s stopAt cpu=%llu gpu=%llu  %s\n", tag,
                        (unsigned long long)cpu.scanned, (unsigned long long)gpu.scanned,
                        ok ? "OK" : "MISMATCH");
            if (!ok) ++bad;
        }
    }

    std::printf("\n%d cases, %d bad -> %s\n", cases, bad, bad == 0 ? "ALL OK" : "FAILED");
    return bad == 0 ? 0 : 1;
}
