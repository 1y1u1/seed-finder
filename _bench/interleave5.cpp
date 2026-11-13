// 批处理原型 v5: 链 = (种子, level) 任意组合, 跨种子成批填满 K 条 lane。
// 目标: 无论 F 区间多短 (1~2 波), 都能用满 K 条 lane 摊薄 398 步初始化。
// 附带验证恒等式 rngSeed(seed, L) = (off + seed + 101*L) mod 2^32。
// 仅用于本地评估, 不参与 SeedFinder.pro 构建。
#include "../seedfinder3/zombiedata.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

typedef unsigned int u32;

static const int kConvId[33] = {
    31, 24, 1, 2, 3, 4, 5, 6, 7, 31, 31, 8, 9, 31, 10,
    11, 12, 13, 14, 31, 15, 16, 17, 18, 31, 25, 31, 31, 31, 31, 31, 31, 19
};
static const u32 kA = 0x6C078965u;
static const u32 kINV = 2083697005u;

double nowMs()
{
    using namespace std::chrono;
    return double(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1000.0;
}

inline u32 temper(u32 tmp)
{
    tmp ^= (tmp >> 11);
    tmp ^= ((tmp & 0xFF3A58ADu) << 7);
    tmp ^= ((tmp & 0xFFFFDF8Cu) << 15);
    tmp ^= (tmp >> 18);
    return tmp & 0x7FFFFFFFu;
}

struct RowRng
{
    u32 buf[0x400];
    int idx;
    void init(u32 seed)
    {
        buf[0] = seed;
        for (int i = 1; i < 0x18E; ++i)
            buf[i] = (buf[i - 1] ^ (buf[i - 1] >> 30)) * kA + u32(i);
        idx = 0;
    }
    u32 gen(u32 max)
    {
        u32 tmp = (buf[idx] & 0x80000000u) | (buf[idx + 1] & 0x7FFFFFFFu);
        buf[idx] = (tmp >> 1) ^ buf[(idx + 0x18D) % 0x270];
        if (tmp & 1u) buf[idx] ^= 0x9908B0DFu;
        buf[idx + 0x18D + 1] =
            (buf[idx + 0x18D] ^ (buf[idx + 0x18D] >> 30)) * kA + u32(idx + 0x18D + 1);
        const u32 r = temper(buf[idx]);
        ++idx;
        return r % max;
    }
};

int g_maxDraws = 0;

// 参考实现: 直接给 rngSeed, 返回该 level 的 rowBits
u32 refBits(u32 rngSeed, int scene, int level)
{
    const u32 allow = u32(sceneAllowMask(scene));
    RowRng r;
    r.init(rngSeed ? rngSeed : 0x1105u);
    u32 has = 1u, d1[10];
    d1[0] = 1u;
    if (r.gen(5u) == 0u) { d1[0] |= 1u << Z_NEWS; has |= 1u << Z_NEWS; }
    else                 { d1[0] |= 1u << Z_CONE; has |= 1u << Z_CONE; }
    int j = 1;
    while (j < 10) {
        const int nid = kConvId[r.gen(33u)];
        if (has & (1u << nid)) continue;
        if (allow & (1u << nid)) {
            has |= 1u << nid;
            d1[j] = d1[j - 1];
            if (nid != 24 && nid != 25) d1[j] |= 1u << nid;
            ++j;
        }
    }
    if (r.idx > g_maxDraws) g_maxDraws = r.idx;
    for (j = 2; j < 6; ++j)
        if (d1[j] & (1u << Z_GIGA)) d1[j] = d1[j + 1] ^ (1u << Z_GIGA);
    if (d1[2] & (1u << Z_ZOMBONI)) {
        if (d1[3] & (1u << Z_GARG)) d1[2] = d1[4] ^ (1u << Z_ZOMBONI) ^ (1u << Z_GARG);
        else d1[2] = d1[3] ^ (1u << Z_ZOMBONI);
    } else if (d1[2] & (1u << Z_GARG)) {
        if (d1[3] & (1u << Z_ZOMBONI)) d1[2] = d1[4] ^ (1u << Z_ZOMBONI) ^ (1u << Z_GARG);
        else d1[2] = d1[3] ^ (1u << Z_GARG);
    }
    if (level <= 0) return 11u;
    if (level >= 8) return d1[9];
    return d1[level + 1];
}

template <int K, int kHead>
struct Batch
{
    u32 head[kHead][K];
    u32 tail[K];

    void init(const u32 rs[K])
    {
        for (int l = 0; l < K; ++l) {
            head[0][l] = rs[l] ? rs[l] : 0x1105u;
            tail[l] = head[0][l];
        }
        for (u32 i = 1; i < 0x18Eu; ++i) {
            const u32 ii = i;
            if (int(i) < kHead) {
                u32 *h = head[i];
                for (int l = 0; l < K; ++l) {
                    const u32 x = tail[l];
                    h[l] = (x ^ (x >> 30)) * kA + ii;
                    tail[l] = h[l];
                }
            } else {
                for (int l = 0; l < K; ++l) {
                    const u32 x = tail[l];
                    tail[l] = (x ^ (x >> 30)) * kA + ii;
                }
            }
        }
    }

    // 返回 false 表示有 lane 抽卡超出前缀, 调用方应回退到串行
    bool draw(int scene, const int lv[K], u32 out[K]) const
    {
        const u32 allow = u32(sceneAllowMask(scene));
        for (int l = 0; l < K; ++l) {
            const int L = lv[l];
            if (L <= 0) { out[l] = 11u; continue; }
            u32 t = tail[l];
            int k = 0;
            u32 has = 1u, d1[10];
            d1[0] = 1u;
            {
                const u32 tmp = (head[0][l] & 0x80000000u) | (head[1][l] & 0x7FFFFFFFu);
                const u32 v = ((tmp >> 1) ^ t) ^ ((tmp & 1u) ? 0x9908B0DFu : 0u);
                t = (t ^ (t >> 30)) * kA + u32(k + 0x18E);
                ++k;
                if (temper(v) % 5u == 0u) { d1[0] |= 1u << Z_NEWS; has |= 1u << Z_NEWS; }
                else                      { d1[0] |= 1u << Z_CONE; has |= 1u << Z_CONE; }
            }
            int j = 1;
            while (j < 10) {
                if (k + 1 >= kHead) return false;
                const u32 tmp = (head[k][l] & 0x80000000u) | (head[k + 1][l] & 0x7FFFFFFFu);
                const u32 v = ((tmp >> 1) ^ t) ^ ((tmp & 1u) ? 0x9908B0DFu : 0u);
                t = (t ^ (t >> 30)) * kA + u32(k + 0x18E);
                ++k;
                const int nid = kConvId[temper(v) % 33u];
                if (has & (1u << nid)) continue;
                if (allow & (1u << nid)) {
                    has |= 1u << nid;
                    d1[j] = d1[j - 1];
                    if (nid != 24 && nid != 25) d1[j] |= 1u << nid;
                    ++j;
                }
            }
            for (j = 2; j < 6; ++j)
                if (d1[j] & (1u << Z_GIGA)) d1[j] = d1[j + 1] ^ (1u << Z_GIGA);
            if (d1[2] & (1u << Z_ZOMBONI)) {
                if (d1[3] & (1u << Z_GARG)) d1[2] = d1[4] ^ (1u << Z_ZOMBONI) ^ (1u << Z_GARG);
                else d1[2] = d1[3] ^ (1u << Z_ZOMBONI);
            } else if (d1[2] & (1u << Z_GARG)) {
                if (d1[3] & (1u << Z_ZOMBONI)) d1[2] = d1[4] ^ (1u << Z_ZOMBONI) ^ (1u << Z_GARG);
                else d1[2] = d1[3] ^ (1u << Z_GARG);
            }
            out[l] = (L >= 8) ? d1[9] : d1[L + 1];
        }
        return true;
    }
};

u32 g_sink = 0;

// 串行基准: n 个种子 x nLevels 波, 场景 2, offset 14
double benchSerial(int n, int nLevels, u32 &acc)
{
    acc = 0;
    const double t0 = nowMs();
    for (int s = 0; s < n; ++s) {
        const u32 pos = (u32(s) + 14u) * kINV;
        for (int i = 0; i < nLevels; ++i) {
            const int lvl = 1 + i;
            acc ^= refBits((pos + u32(lvl)) * 101u, 2, lvl);
        }
    }
    return nowMs() - t0;
}

// 批处理基准: 一次 K 条链, 链 = (seed, level) 线性编号
template <int K>
double benchBatch(int n, int nLevels, u32 &acc, long &bails)
{
    acc = 0;
    bails = 0;
    Batch<K, 128> bt;
    u32 rs[K], out[K];
    int lv[K];
    const double t0 = nowMs();
    const int total = n * nLevels;
    for (int idx = 0; idx < total; idx += K) {
        const int cnt = (idx + K <= total) ? K : (total - idx);
        for (int l = 0; l < K; ++l) {
            const int t = (l < cnt) ? (idx + l) : idx;
            const int si = t / nLevels, li = t % nLevels;
            const int lvl = 1 + li;
            lv[l] = lvl;
            rs[l] = u32(si) + 14u + 101u * u32(lvl);
        }
        bt.init(rs);
        if (!bt.draw(2, lv, out)) {
            ++bails;
            for (int l = 0; l < cnt; ++l) {
                const int t = idx + l;
                const int si = t / nLevels, li = t % nLevels;
                const int lvl = 1 + li;
                const u32 pos = (u32(si) + 14u) * kINV;
                out[l] = refBits((pos + u32(lvl)) * 101u, 2, lvl);
            }
        }
        for (int l = 0; l < cnt; ++l) acc ^= out[l];
    }
    return nowMs() - t0;
}

} // namespace

int main(int argc, char *argv[])
{
    int n = 100000;
    for (int i = 1; i + 1 < argc; i += 2)
        if (!std::strcmp(argv[i], "--n")) n = std::atoi(argv[i + 1]);

    // ---- 恒等式: (pos+L)*101 == (off + seed) + 101*L ----
    {
        int bad = 0;
        for (u32 seed = 0; seed < 200000u; seed += 7u) {
            const u32 pos = (seed + 23u) * kINV;
            for (int L = 0; L < 25; ++L) {
                const u32 a = (pos + u32(L)) * 101u;
                const u32 b = 23u + seed + 101u * u32(L);
                if (a != b && ++bad < 4)
                    std::printf("IDENT MISMATCH seed=%u L=%d %08X vs %08X\n", seed, L, a, b);
            }
        }
        std::printf("identity        : %s\n", bad ? "FAIL" : "OK");
        std::fflush(stdout);
    }

    // ---- 正确性 ----
    {
        int bad = 0;
        long bails = 0;
        for (int nLevels : {1, 2, 3, 7, 13, 25}) {
            for (int scene : {0, 1, 2, 4, 6, 7}) {
                Batch<8, 128> bt;
                u32 rs[8], out[8];
                int lv[8];
                for (u32 s0 = 0; s0 < 40000u; s0 += 8u) {
                    for (int l = 0; l < 8; ++l) {
                        const u32 si = s0 + u32(l % 8);
                        const int lvl = 1 + int((l / 8) % nLevels);
                        lv[l] = lvl;
                        rs[l] = si + 14u + 101u * u32(lvl);
                    }
                    bt.init(rs);
                    if (!bt.draw(scene, lv, out)) {
                        ++bails;
                        for (int l = 0; l < 8; ++l) {
                            const u32 si = s0 + u32(l % 8);
                            out[l] = refBits(si + 14u + 101u * u32(lv[l]), scene, lv[l]);
                        }
                    }
                    for (int l = 0; l < 8; ++l) {
                        const u32 si = s0 + u32(l % 8);
                        const u32 ref = refBits(si + 14u + 101u * u32(lv[l]), scene, lv[l]);
                        if (ref != out[l] && ++bad < 5)
                            std::printf("MISMATCH scene=%d seed=%u lv=%d %08X vs %08X\n", scene,
                                        si, lv[l], ref, out[l]);
                    }
                }
            }
        }
        std::printf("correctness     : %s  bails=%ld maxDraws=%d\n", bad ? "FAIL" : "OK", bails,
                    g_maxDraws);
        std::fflush(stdout);
    }

    // ---- 速度 ----
    std::printf("\n%-5s %-8s %-10s %-10s %-9s %-11s %s\n", "K", "nLevels", "serial", "batch",
                "speedup", "ns/row", "bails");
    for (int nLevels : {1, 2, 3, 5, 25}) {
        u32 accS = 0;
        const double msS = benchSerial(n, nLevels, accS);
        const double rows = double(n) * nLevels;
        long b8 = 0, b16 = 0, b32 = 0;
        u32 a8 = 0, a16 = 0, a32 = 0;
        const double ms8 = benchBatch<8>(n, nLevels, a8, b8);
        const double ms16 = benchBatch<16>(n, nLevels, a16, b16);
        const double ms32 = benchBatch<32>(n, nLevels, a32, b32);
        std::printf("K=8   %-8d %8.0fms %8.0fms %8.2fx %10.1f %ld\n", nLevels, msS, ms8, msS / ms8,
                    ms8 * 1e6 / rows, b8);
        std::printf("K=16  %-8d %8.0fms %8.0fms %8.2fx %10.1f %ld\n", nLevels, msS, ms16,
                    msS / ms16, ms16 * 1e6 / rows, b16);
        std::printf("K=32  %-8d %8.0fms %8.0fms %8.2fx %10.1f %ld\n\n", nLevels, msS, ms32,
                    msS / ms32, ms32 * 1e6 / rows, b32);
        g_sink ^= accS ^ a8 ^ a16 ^ a32;
    }
    std::fflush(stdout);

    // ---- 多线程吞吐 ----
    {
        const unsigned nt = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1u;
        for (int nLevels : {1, 2, 3, 25}) {
            const int per = 100000;
            std::vector<std::thread> pool;
            const double t0 = nowMs();
            for (unsigned t = 0; t < nt; ++t) {
                pool.emplace_back([t, per, nLevels]() {
                    u32 acc = 0;
                    long bails = 0;
                    u32 rs[16], out[16];
                    int lv[16];
                    Batch<16, 128> bt;
                    const int total = per * nLevels;
                    (void)t;
                    for (int idx = 0; idx < total; idx += 16) {
                        const int cnt = (idx + 16 <= total) ? 16 : (total - idx);
                        for (int l = 0; l < 16; ++l) {
                            const int q = (l < cnt) ? (idx + l) : idx;
                            const int si = q / nLevels, li = q % nLevels;
                            const int lvl = 1 + li;
                            lv[l] = lvl;
                            rs[l] = u32(si) + 14u + 101u * u32(lvl);
                        }
                        bt.init(rs);
                        if (!bt.draw(2, lv, out))
                            for (int l = 0; l < cnt; ++l) {
                                const int q = idx + l;
                                const int si = q / nLevels, li = q % nLevels;
                                const int lvl = 1 + li;
                                out[l] = refBits(u32(si) + 14u + 101u * u32(lvl), 2, lvl);
                            }
                        for (int l = 0; l < cnt; ++l) acc ^= out[l];
                    }
                    g_sink ^= acc;
                });
            }
            for (auto &th : pool) th.join();
            const double ms = nowMs() - t0;
            std::printf("MT nLevels=%-2d : %8.0f ms  %.2f M seeds/s (%u threads)\n", nLevels, ms,
                        double(nt) * per / (ms * 1000.0), nt);
            std::fflush(stdout);
        }
    }
    return 0;
}
