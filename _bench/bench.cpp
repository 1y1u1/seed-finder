// 离线微基准: 只用于本地性能分析, 不参与 SeedFinder.pro 构建
#include "../seedfinder3/seedengine.h"
#include "../seedfinder3/zombiedata.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// 复刻 seedengine.cpp 内部的 RowRng, 用于分段计时。
// 注意: 原实现的 idx 是单调递增的, 只有 computeRow 那种少量抽取才安全;
// 这里为了连续抽取把缓冲区开到足够大。
struct BenchRng
{
    std::vector<quint32> buf;
    int idx = 0;
    explicit BenchRng(size_t cap = 0x400) : buf(cap, 0u) {}
    void rngInit(quint32 seed)
    {
        buf[0] = seed;
        for (int i = 1; i < 0x18E; ++i)
            buf[size_t(i)] = (buf[size_t(i - 1)] ^ (buf[size_t(i - 1)] >> 30)) * 0x6C078965u + quint32(i);
        idx = 0;
    }
    quint32 rngGen(int max)
    {
        quint32 tmp = (buf[size_t(idx)] & 0x80000000u) | (buf[size_t(idx) + 1] & 0x7FFFFFFFu);
        buf[size_t(idx)] = (tmp >> 1) ^ buf[size_t((idx + 0x18D) % 0x270)];
        if (tmp & 1u) buf[size_t(idx)] ^= 0x9908B0DFu;
        buf[size_t(idx) + 0x18D + 1] =
            (buf[size_t(idx) + 0x18D] ^ (buf[size_t(idx) + 0x18D] >> 30)) * 0x6C078965u +
            quint32(idx + 0x18D + 1);
        tmp = buf[size_t(idx)];
        tmp ^= (tmp >> 11);
        tmp ^= ((tmp & 0xFF3A58ADu) << 7);
        tmp ^= ((tmp & 0xFFFFDF8Cu) << 15);
        tmp ^= (tmp >> 18);
        ++idx;
        return (tmp & 0x7FFFFFFFu) % quint32(max);
    }
};

static const int kConvId[33] = {
    31, 24, 1, 2, 3, 4, 5, 6, 7, 31, 31, 8, 9, 31, 10,
    11, 12, 13, 14, 31, 15, 16, 17, 18, 31, 25, 31, 31, 31, 31, 31, 31, 19
};

double nowMs()
{
    using namespace std::chrono;
    return double(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1000.0;
}

quint32 g_sink = 0;

} // namespace

int main(int argc, char *argv[])
{
    int n = 400000;
    int only = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--n")) n = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--only")) only = std::atoi(argv[i + 1]);
    }
    const auto want = [only](int id) { return only == 0 || only == id; };

    if (want(1)) {
        const double t0 = nowMs();
        quint32 acc = 0;
        for (int i = 0; i < n; ++i) {
            quint32 d1[10];
            sf::computeRow(0x1000u + quint32(i), 2, d1);
            acc ^= d1[9];
        }
        const double ms = nowMs() - t0;
        g_sink = acc;
        std::printf("computeRow      : %8.1f ms  %6.1f ns/call\n", ms, ms * 1e6 / n);
        std::fflush(stdout);
    }

    if (want(2)) {
        BenchRng r;
        const double t0 = nowMs();
        for (int i = 0; i < n; ++i) r.rngInit(quint32(i) * 101u + 1u);
        const double ms = nowMs() - t0;
        g_sink = r.buf[5];
        std::printf("rngInit only    : %8.1f ms  %6.1f ns/call\n", ms, ms * 1e6 / n);
        std::fflush(stdout);
    }

    if (want(3)) {
        BenchRng r;
        const double t0 = nowMs();
        quint32 acc = 0;
        long draws = 0;
        for (int i = 0; i < n; ++i) {
            const quint32 pos = 0x1000u + quint32(i);
            quint32 rs = pos * 101u;
            if (rs == 0) rs = 0x1105u;
            r.rngInit(rs);
            quint32 has = 1u, d1[10];
            d1[0] = 1u;
            if (r.rngGen(5) == 0) { d1[0] |= 1u << Z_NEWS; has |= 1u << Z_NEWS; }
            else                  { d1[0] |= 1u << Z_CONE; has |= 1u << Z_CONE; }
            const quint32 allow = quint32(::sceneAllowMask(2));
            int j = 1, d = 0;
            while (j < 10) {
                ++d;
                const int type = int(r.rngGen(33));
                const int nid = kConvId[type];
                if (has & (1u << nid)) continue;
                if (allow & (1u << nid)) {
                    has |= 1u << nid;
                    d1[j] = d1[j - 1];
                    if (nid != 24 && nid != 25) d1[j] |= 1u << nid;
                    ++j;
                }
            }
            draws += d;
            acc ^= d1[9];
        }
        const double ms = nowMs() - t0;
        g_sink = acc;
        std::printf("draws only      : %8.1f ms  %6.1f ns/call  (avg %.1f draws)\n", ms,
                    ms * 1e6 / n, double(draws) / n);
        std::fflush(stdout);
    }

    if (want(4)) {
        const int m = n * 8;
        BenchRng r(size_t(m) + 0x400);
        r.rngInit(12345u);
        const double t0 = nowMs();
        quint32 acc = 0;
        for (int i = 0; i < m; ++i) acc += r.rngGen(33);
        const double ms = nowMs() - t0;
        g_sink = acc;
        std::printf("rngGen          : %8.1f ms  %6.2f ns/call\n", ms, ms * 1e6 / m);
        std::fflush(stdout);
    }

    if (want(5)) {
        const unsigned nt = (std::thread::hardware_concurrency() > 0u) ? std::thread::hardware_concurrency() : 1u;
        const int per = 200000;
        std::vector<std::thread> pool;
        const double t0 = nowMs();
        for (unsigned t = 0; t < nt; ++t) {
            pool.emplace_back([t, per]() {
                quint32 acc = 0;
                for (int i = 0; i < per; ++i) {
                    const quint32 pos = quint32(t) * 7919u + quint32(i) * 101u;
                    for (int lv = 0; lv < 25; ++lv) {
                        quint32 d1[10];
                        sf::computeRow(pos + quint32(lv), 2, d1);
                        acc ^= sf::rowBits(d1, lv);
                    }
                }
                g_sink ^= acc;
            });
        }
        for (auto &th : pool) th.join();
        const double ms = nowMs() - t0;
        const double seeds = double(nt) * per;
        std::printf("multithread     : %8.1f ms  %.2f M seeds/s (25 waves/seed, %u threads)\n", ms,
                    seeds / (ms * 1000.0), nt);
        std::fflush(stdout);
    }

    // 真实 Searcher 端到端吞吐 (批处理路径)
    if (want(6)) {
        const int waves[] = {1, 2, 3, 5, 10, 25};
        for (int wi = 0; wi < 6; ++wi) {
            const int nw = waves[wi];
            sf::SearchSpec s;
            s.scene = 2;
            s.beginFlag = 0;
            s.endFlag = 2 * nw;
            s.span = 4000000;
            s.outputSize = 20;
            s.threads = 0;
            sf::RuleBand b;
            b.beginFlag = 0;
            b.endFlag = 2 * nw;
            b.cond.needMask = 1u << Z_GARG;
            b.cond.sanitize();
            s.bands.append(b);
            const double t0 = nowMs();
            const sf::SearchResult r = sf::Searcher().run(s, nullptr, nullptr);
            const double ms = nowMs() - t0;
            std::printf("searcher %2d waves: %8.1f ms  %6.2f M seeds/s  (scanned=%llu)\n", nw, ms,
                        double(r.scanned) / (ms * 1000.0), (unsigned long long)r.scanned);
            std::fflush(stdout);
        }
    }

    return 0;
}
