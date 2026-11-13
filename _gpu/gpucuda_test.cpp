// GPU (CUDA) 检索核心的验证与基准程序。
//
//   gpucuda_test dump  <count> <scene> <r0> <out.bin>
//       用 CPU 引擎导出黄金标准选卡表 (每张 8 个 uint32 = d1[2..9])
//
//   gpucuda_test check <count> <scene> <r0>
//       GPU 算同样的表, 与 CPU 逐位对比
//
//   gpucuda_test pipe  <seeds> <waves> [scene] [uid] [mode]
//       完整流水线吞吐: 去重后的两条 kernel, 与 CPU 单线程对比
//
// 只用于本地验证, 不参与 SeedFinder.pro 构建。

#include "gpucuda.h"
#include "seedengine.h"
#include "zombiedata.h"

#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

double nowMs()
{
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch()).count();
}

// 用 CPU 引擎算一张表, 与 GPU 端的 buildTable 对齐 (只取 d1[2..9])
void cpuTable(unsigned rngSeed, int scene, std::vector<unsigned> &out, int &at)
{
    quint32 d1[10];
    if (!sf::rowTableBySeed(rngSeed, scene, d1)) {
        for (int i = 0; i < 10; ++i) d1[i] = 1u;
    }
    for (int i = 2; i <= 9; ++i) out[size_t(at++)] = d1[i];
}

int runDump(int count, int scene, unsigned r0, const char *path)
{
    std::vector<unsigned> buf;
    buf.reserve(size_t(count) * 8);
    int at = 0;
    for (int i = 0; i < count; ++i) {
        unsigned seed = r0 + unsigned(i);
        if (seed == 0u) seed = 0x1105u;
        cpuTable(seed, scene, buf, at);
    }
    FILE *f = std::fopen(path, "wb");
    if (!f) { std::printf("cannot open %s\n", path); return 2; }
    std::fwrite(buf.data(), sizeof(unsigned), buf.size(), f);
    std::fclose(f);
    std::printf("wrote %d tables (%zu words) to %s\n", count, buf.size(), path);
    return 0;
}

int runCheck(int count, int scene, unsigned r0)
{
    std::string error;
    std::vector<unsigned> gpu;
    if (!gpuDumpTables(r0, unsigned(sceneAllowMask(scene)), unsigned(count), gpu, &error)) {
        std::printf("gpu failed: %s\n", error.c_str());
        return 1;
    }

    std::vector<unsigned> cpu;
    cpu.reserve(size_t(count) * 8);
    int at = 0;
    for (int i = 0; i < count; ++i) {
        unsigned seed = r0 + unsigned(i);
        if (seed == 0u) seed = 0x1105u;
        cpuTable(seed, scene, cpu, at);
    }

    int bad = 0;
    for (int i = 0; i < count && bad < 5; ++i) {
        for (int j = 0; j < 8; ++j) {
            const unsigned a = gpu[size_t(i) * 8 + size_t(j)];
            const unsigned b = cpu[size_t(i) * 8 + size_t(j)];
            if (a != b) {
                std::printf("MISMATCH table %d word %d: gpu=0x%08X cpu=0x%08X (r=0x%08X)\n",
                            i, j, a, b, r0 + unsigned(i));
                ++bad;
                break;
            }
        }
    }
    if (bad == 0) {
        std::printf("verify: OK (%d tables, %d words compared)\n", count, count * 8);
        return 0;
    }
    std::printf("verify: FAILED (%d bad)\n", bad);
    return 1;
}

struct CondSpec
{
    unsigned needMask = 0;
    unsigned needTotal = 0;
    unsigned refuseMask = 0;
    unsigned atLeastMask = 0;
    unsigned atLeast = 0;
    unsigned atMostMask = 0;
    unsigned atMost = 0;
};

CondG toCondG(const CondSpec &s)
{
    CondG c;
    c.needMask = s.needMask;
    c.needTotal = s.needTotal;
    c.refuseMask = s.refuseMask;
    c.atLeastMask = s.atLeastMask;
    c.atLeast = s.atLeast;
    c.atMostMask = s.atMostMask;
    c.atMost = s.atMost;
    return c;
}

// ---- 主机侧黄金标准的条件判定 (与 WaveCondition::matches 同义) ----
inline int popcnt32(unsigned x)
{
    int n = 0;
    while (x) { x &= (x - 1u); ++n; }
    return n;
}

bool condMatchesCpu(const CondG &c, unsigned bits)
{
    if (unsigned(popcnt32(bits & c.needMask)) != c.needTotal) return false;
    if (bits & c.refuseMask) return false;
    if (c.atLeastMask && popcnt32(bits & c.atLeastMask) < int(c.atLeast)) return false;
    if (c.atMostMask && popcnt32(bits & c.atMostMask) > int(c.atMost)) return false;
    return true;
}

int runPipe(int seeds, int waves, int scene, int uid, int mode)
{
    const unsigned off32 = unsigned(uid + mode);
    const unsigned allow = unsigned(sceneAllowMask(scene));
    const int lvFirst = 1;
    const int lvLast = waves;          // 查 1..waves 轮

    // 造两条条件: 出怪条件必出红眼, 全局条件必出撑杆。
    // 都从中间的轮次开始起作用 —— 第 1 轮只有普僵 + 路障/读报, 条件从第 1 轮
    // 生效的话所有种子一进来就被淘汰, 结果清一色 0, 逐种子对比就没意义了。
    CondSpec s;
    s.needMask = 1u << Z_GIGA;
    s.needTotal = 1;

    CondSpec h;
    h.needMask = 1u << Z_POLE;
    h.needTotal = 1;

    // 平铺成 LevelDesc + CondG
    std::vector<CondG> conds;
    std::vector<LevelDesc> levels;
    for (int lv = lvFirst; lv <= lvLast; ++lv) {
        LevelDesc d;
        d.level = lv;
        d.tableStride = unsigned(101 * (lv - lvFirst));
        // 条件只挂在个别轮上。若从第 5 轮起每轮都要求"必出撑杆", 而撑杆
        // 每波最多出现一次, 那所有种子都会被淘汰, 结果清一色 0。
        const bool inRank = (lv == 7);     // 第 7 轮必出红眼
        const bool inHard = (lv == 23);    // 第 23 轮必出撑杆
        if (inRank) {
            d.rankOff = unsigned(conds.size());
            d.rankCnt = 1;
            conds.push_back(toCondG(s));
        }
        if (inHard) {
            d.hardOff = unsigned(conds.size());
            d.hardCnt = 1;
            conds.push_back(toCondG(h));
        }
        levels.push_back(d);
    }

    GpuSearcher gs;
    std::string error;
    if (!gs.prepare(levels, conds, allow, off32, lvFirst, &error)) {
        std::printf("prepare failed: %s\n", error.c_str());
        return 1;
    }

    std::vector<unsigned short> out;
    // 预热
    if (!gs.scan(0u, unsigned(seeds), out, &error)) {
        std::printf("scan failed: %s\n", error.c_str());
        return 1;
    }

    const double g0 = nowMs();
    if (!gs.scan(0u, unsigned(seeds), out, &error)) {
        std::printf("scan failed: %s\n", error.c_str());
        return 1;
    }
    const double gMs = nowMs() - g0;

    // CPU 单线程黄金标准
    const double c0 = nowMs();
    std::vector<unsigned short> cpu(size_t(seeds), 0);
    int full = 0;
    int rankedTotal = 0;
    for (size_t li = 0; li < levels.size(); ++li) {
        if (levels[li].rankCnt > 0) ++rankedTotal;
    }
    for (int i = 0; i < seeds; ++i) {
        const unsigned s0 = unsigned(i);
        unsigned matched = 0;
        bool alive = true;
        for (size_t li = 0; li < levels.size() && alive; ++li) {
            const LevelDesc &d = levels[li];
            const int lv = d.level;
            unsigned bits;
            if (lv <= 0) {
                bits = 11u;
            } else {
                quint32 d1[10];
                const unsigned r = unsigned(s0 + off32 + 101u * unsigned(lv));
                if (!sf::rowTableBySeed(r, scene, d1)) {
                    for (int j = 0; j < 10; ++j) d1[j] = 1u;
                }
                bits = (lv >= 8) ? d1[9] : d1[lv + 1];
            }
            for (unsigned c = 0; c < d.hardCnt; ++c) {
                if (!condMatchesCpu(conds[d.hardOff + c], bits)) { alive = false; break; }
            }
            if (!alive) break;
            if (d.rankCnt > 0) {
                bool ok = true;
                for (unsigned c = 0; c < d.rankCnt; ++c) {
                    if (!condMatchesCpu(conds[d.rankOff + c], bits)) { ok = false; break; }
                }
                if (ok) ++matched;
            }
        }
        cpu[size_t(i)] = alive ? (unsigned short)(matched + 1u) : 0u;
        if (alive && int(matched) == rankedTotal) ++full;
    }
    const double cMs = nowMs() - c0;

    // 逐种子对比
    int bad = 0;
    for (int i = 0; i < seeds && bad < 5; ++i) {
        if (cpu[size_t(i)] != out[size_t(i)]) {
            std::printf("SEED MISMATCH seed=%u cpu=%u gpu=%u\n",
                        unsigned(i), unsigned(cpu[size_t(i)]), unsigned(out[size_t(i)]));
            ++bad;
        }
    }

    std::printf("seeds=%d levels=%zu tables=%u\n", seeds, levels.size(),
                unsigned(seeds) + levels.back().tableStride);
    std::printf("gpu: %.1f ms  %.2f M seeds/s\n", gMs, double(seeds) / gMs * 1000.0 / 1e6);
    std::printf("cpu1: %.1f ms  %.2f M seeds/s\n", cMs, double(seeds) / cMs * 1000.0 / 1e6);
    std::printf("gpu / 1 cpu-thread = %.1fx\n", cMs / gMs);

    // 结果分布: 0 = 被硬性条件淘汰, k = 匹配 k-1 轮。全是 0 的话说明条件
    // 造得太严, 逐种子对比其实没验证到任何有效路径。
    int dead = 0;
    int hist[8] = {0};
    for (int i = 0; i < seeds; ++i) {
        const unsigned v = out[size_t(i)];
        if (v == 0u) ++dead;
        else if (v < 8u) ++hist[v];
    }
    std::printf("dist: dead=%d", dead);
    for (int k = 1; k < 8; ++k) std::printf("  m%d=%d", k, hist[k]);
    std::printf("\n");

    if (bad == 0) {
        std::printf("verify: OK (%d seeds compared, full=%d)\n", seeds, full);
        return 0;
    }
    std::printf("verify: FAILED (%d bad)\n", bad);
    return 1;
}

} // namespace

// 扫参数: blockSize x chains x strided, 找出最快的组合。
// 只跑 build kernel 的耗时 (eval 与参数无关), 用 lastTiming 拆出来的值。
int runTune(int seeds, int waves, int scene)
{
    const unsigned off32 = 14u;     // uid 1 + mode 13
    const unsigned allow = unsigned(sceneAllowMask(scene));

    CondSpec s;
    s.needMask = 1u << Z_GIGA;
    s.needTotal = 1;
    CondSpec h;
    h.needMask = 1u << Z_POLE;
    h.needTotal = 1;

    std::vector<CondG> conds;
    std::vector<LevelDesc> levels;
    for (int lv = 1; lv <= waves; ++lv) {
        LevelDesc d;
        d.level = lv;
        d.tableStride = unsigned(101 * (lv - 1));
        if (lv == 7) { d.rankOff = unsigned(conds.size()); d.rankCnt = 1; conds.push_back(toCondG(s)); }
        if (lv == 23) { d.hardOff = unsigned(conds.size()); d.hardCnt = 1; conds.push_back(toCondG(h)); }
        levels.push_back(d);
    }

    GpuSearcher gs;
    std::string error;
    if (!gs.prepare(levels, conds, allow, off32, 1, &error)) {
        std::printf("prepare failed: %s\n", error.c_str());
        return 1;
    }

    std::printf("%8s %6s %8s %10s %10s %12s\n", "block", "chains", "strided", "build ms", "eval ms", "M seeds/s");

    const int blocks[] = {64, 128, 256, 512, 1024};
    const int chains[] = {1, 2, 3, 4, 5, 6, 7, 8};
    double bestMs = 1e30;
    int bestB = 0, bestC = 0, bestS = 0;

    for (int si = 0; si < 2; ++si) {
        for (int bi = 0; bi < int(sizeof(blocks) / sizeof(blocks[0])); ++bi) {
            for (int ci = 0; ci < int(sizeof(chains) / sizeof(chains[0])); ++ci) {
                gpuSetTuning(blocks[bi], chains[ci], si);
                std::vector<unsigned short> out;
                // 预热一次 (表缓冲已分配好), 之后取 3 次里最快的一次 ——
                // 单次只有 1~2 ms, 不重复取最优的话全被调度抖动淹掉。
                if (!gs.scan(0u, unsigned(seeds), out, &error)) {
                    std::printf("scan failed: %s\n", error.c_str());
                    return 1;
                }
                double bMs = 1e30, eMs = 0.0;
                for (int rep = 0; rep < 3; ++rep) {
                    if (!gs.scan(0u, unsigned(seeds), out, &error)) {
                        std::printf("scan failed: %s\n", error.c_str());
                        return 1;
                    }
                    double b = 0.0, e = 0.0;
                    gs.lastTiming(&b, &e);
                    if (b + e < bMs + eMs) { bMs = b; eMs = e; }
                }
                const double total = bMs + eMs;
                std::printf("%8d %6d %8d %10.2f %10.2f %12.2f\n",
                            blocks[bi], chains[ci], si, bMs, eMs,
                            double(seeds) / total * 1000.0 / 1e6);
                if (total < bestMs) { bestMs = total; bestB = blocks[bi]; bestC = chains[ci]; bestS = si; }
            }
        }
    }

    std::printf("best: block=%d chains=%d strided=%d  %.2f ms  %.2f M seeds/s\n",
                bestB, bestC, bestS, bestMs, double(seeds) / bestMs * 1000.0 / 1e6);
    return 0;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::printf("usage:\n"
                    "  gpucuda_test dump  <count> <scene> <r0> <out.bin>\n"
                    "  gpucuda_test check <count> <scene> <r0>\n"
                    "  gpucuda_test pipe  <seeds> <waves> [scene] [uid] [mode]\n");
        return 2;
    }

    char name[256] = {0};
    int sms = 0;
    double gbps = 0;
    gpuDeviceInfo(name, int(sizeof(name)), &sms, &gbps);
    std::printf("gpu: %s  SMs=%d  %.1f GB/s\n", name, sms, gbps);

    const char *cmd = argv[1];

    if (std::strcmp(cmd, "dump") == 0 && argc >= 6) {
        return runDump(std::atoi(argv[2]), std::atoi(argv[3]),
                       unsigned(std::strtoul(argv[4], nullptr, 0)), argv[5]);
    }
    if (std::strcmp(cmd, "check") == 0 && argc >= 5) {
        return runCheck(std::atoi(argv[2]), std::atoi(argv[3]),
                        unsigned(std::strtoul(argv[4], nullptr, 0)));
    }
    if (std::strcmp(cmd, "pipe") == 0 && argc >= 4) {
        const int scene = (argc >= 5) ? std::atoi(argv[4]) : 2;
        const int uid = (argc >= 6) ? std::atoi(argv[5]) : 1;
        const int mode = (argc >= 7) ? std::atoi(argv[6]) : 13;
        return runPipe(std::atoi(argv[2]), std::atoi(argv[3]), scene, uid, mode);
    }

    // tune <seeds> <waves> [scene]: sweep blockSize x chains and report the best
    if (std::strcmp(cmd, "tune") == 0 && argc >= 4) {
        const int seeds = std::atoi(argv[2]);
        const int waves = std::atoi(argv[3]);
        const int scene = (argc >= 5) ? std::atoi(argv[4]) : 2;
        return runTune(seeds, waves, scene);
    }

    std::printf("bad arguments\n");
    return 2;
}
