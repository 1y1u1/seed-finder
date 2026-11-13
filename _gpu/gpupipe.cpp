// GPU 加速可行性原型 (D3D11 compute shader) —— 贴近实际的整条流水线
// 只用于本地评估, 不参与 SeedFinder.pro 构建。
//
// 目的: 一个 GPU 线程负责一个种子的全部轮次 (含条件判定 + 硬性条件提前淘汰),
//       输出每个种子的"匹配轮数", 主机只回读一个 uint/种子去做分档统计。
//       与 CPU 参考实现逐种子对比, 并测端到端吞吐。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>

#include "../seedfinder3/src/core/seedengine.cpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

typedef uint32_t u32;

// ---------------------------------------------------------------------------
//  HLSL
// ---------------------------------------------------------------------------
static const char *kShader = R"HLSL(
#define Z_CONE 1
#define Z_NEWS 4
#define Z_ZOMBONI 9
#define Z_GARG 18
#define Z_GIGA 19
#define KMAX 130
#define MAXLEVELS 64

struct CondGpu
{
    uint needMask;
    uint needTotal;
    uint refuseMask;
    uint atLeastMask;
    uint atLeast;
    uint atMostMask;
    uint atMost;
    uint present;      // 0 = 这一轮没有条件
};

cbuffer Params : register(b0)
{
    uint g_base;
    uint g_off32;
    uint g_levelBegin;
    uint g_levelCount;
    uint g_count;
    uint g_allow;
    uint g_hasHard;
    uint g_pad0;
};

StructuredBuffer<CondGpu> g_rank : register(t0);
StructuredBuffer<CondGpu> g_hard : register(t1);
RWStructuredBuffer<uint> g_out : register(u0);

static const int kConvId[33] = {
    31, 24, 1, 2, 3,
    4, 5, 6, 7, 31,
    31, 8, 9, 31, 10,
    11, 12, 13, 14, 31,
    15, 16, 17, 18, 31, 25,
    31, 31, 31, 31, 31, 31,
    19
};

uint mtTemper(uint tmp)
{
    tmp ^= (tmp >> 11);
    tmp ^= ((tmp & 0xFF3A58ADu) << 7);
    tmp ^= ((tmp & 0xFFFFDF8Cu) << 15);
    tmp ^= (tmp >> 18);
    return tmp & 0x7FFFFFFFu;
}

uint mtNext(uint prev, uint i)
{
    return (prev ^ (prev >> 30)) * 0x6C078965u + i;
}

bool condMatches(CondGpu c, uint bits)
{
    if (countbits(bits & c.needMask) != c.needTotal) return false;
    if (bits & c.refuseMask) return false;
    if (c.atLeastMask && countbits(bits & c.atLeastMask) < c.atLeast) return false;
    if (c.atMostMask && countbits(bits & c.atMostMask) > c.atMost) return false;
    return true;
}

// 算某一轮的 bits (lv 已知), 内部完成一整轮 MT 递推
// 注意: 相邻轮次的 rngSeed 相差 101, 前缀完全不同, 所以每轮都要重算 398 步。
// 这正是 CPU 端的瓶颈, 也是 GPU 最能摊薄的地方。
uint rowBitsFor(uint seed, uint lv)
{
    const uint rngSeedRaw = seed + g_off32 + 101u * lv;
    const uint rngSeed = (rngSeedRaw == 0u) ? 0x1105u : rngSeedRaw;

    uint tail = rngSeed;
    [loop]
    for (uint i = 1; i < 0x18E; ++i) tail = mtNext(tail, i);

    uint a = rngSeed;
    uint b = mtNext(rngSeed, 1u);
    uint k = 0;

    uint d1[10];
    uint has = 1u;
    d1[0] = 1u;

    uint t = tail;
    {
        const uint tmp = (a & 0x80000000u) | (b & 0x7FFFFFFFu);
        const uint v = ((tmp >> 1) ^ t) ^ ((tmp & 1u) ? 0x9908B0DFu : 0u);
        t = mtNext(t, k + 0x18E);
        if (mtTemper(v) % 5u == 0u) {
            d1[0] |= (1u << Z_NEWS);
            has |= (1u << Z_NEWS);
        } else {
            d1[0] |= (1u << Z_CONE);
            has |= (1u << Z_CONE);
        }
    }

    int j = 1;
    [loop]
    while (j < 10 && k + 2 < KMAX) {
        ++k;
        a = b;
        b = mtNext(b, k + 1u);

        const uint tmp = (a & 0x80000000u) | (b & 0x7FFFFFFFu);
        const uint v = ((tmp >> 1) ^ t) ^ ((tmp & 1u) ? 0x9908B0DFu : 0u);
        t = mtNext(t, k + 0x18E);
        const int nid = kConvId[mtTemper(v) % 33u];
        if (has & (1u << nid)) continue;
        if (g_allow & (1u << nid)) {
            has |= (1u << nid);
            d1[j] = d1[j - 1];
            if (nid != 24 && nid != 25) d1[j] |= (1u << nid);
            ++j;
        }
    }

    for (int m = 2; m < 6; ++m) {
        if (d1[m] & (1u << Z_GIGA)) d1[m] = d1[m + 1] ^ (1u << Z_GIGA);
    }
    if (d1[2] & (1u << Z_ZOMBONI)) {
        if (d1[3] & (1u << Z_GARG)) {
            d1[2] = d1[4] ^ (1u << Z_ZOMBONI) ^ (1u << Z_GARG);
        } else {
            d1[2] = d1[3] ^ (1u << Z_ZOMBONI);
        }
    } else if (d1[2] & (1u << Z_GARG)) {
        if (d1[3] & (1u << Z_ZOMBONI)) {
            d1[2] = d1[4] ^ (1u << Z_ZOMBONI) ^ (1u << Z_GARG);
        } else {
            d1[2] = d1[3] ^ (1u << Z_GARG);
        }
    }

    return (lv >= 8u) ? d1[9] : d1[lv + 1u];
}

[numthreads(128, 1, 1)]
void CSSeed(uint3 tid : SV_DispatchThreadID)
{
    const uint gid = tid.x;
    if (gid >= g_count) return;

    const uint seed = g_base + gid;
    uint matched = 0u;

    [loop]
    for (uint i = 0; i < g_levelCount; ++i) {
        const uint lv = g_levelBegin + i;
        const uint bits = rowBitsFor(seed, lv);

        const CondGpu hc = g_hard[i];
        if (hc.present) {
            if (!condMatches(hc, bits)) {
                g_out[gid] = 0u;      // 硬性条件不满足 -> 这个种子死亡
                return;
            }
        }
        const CondGpu rc = g_rank[i];
        if (rc.present) {
            if (condMatches(rc, bits)) ++matched;
        }
    }

    g_out[gid] = matched + 1u;        // +1 用来把"死亡(0)"和"匹配 0 轮"区分开
}
)HLSL";

// ---------------------------------------------------------------------------
static ID3DBlob *compileShader(const char *entry)
{
    ID3DBlob *code = nullptr, *err = nullptr;
    const HRESULT hr = D3DCompile(kShader, strlen(kShader), "seed.hlsl", nullptr, nullptr,
                                  entry, "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        std::printf("shader compile failed (%s): %s\n", entry,
                    err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

struct CondGpu {
    u32 needMask, needTotal, refuseMask, atLeastMask;
    u32 atLeast, atMostMask, atMost, present;
};

struct Params {
    u32 base, off32, levelBegin, levelCount;
    u32 count, allow, hasHard, pad0;
};

int main(int argc, char **argv)
{
    unsigned span = 4000000;
    if (argc > 1) span = (unsigned)strtoul(argv[1], nullptr, 10);

    const int scene = 2;
    const unsigned uid = 1, mode = 13;
    const unsigned off32 = uid + mode;
    const unsigned levelBegin = 1;
    const unsigned levelCount = 25;

    // ---- 构造一组典型条件: 4 条排名带 + 1 条硬性带 ----
    // 排名带落在 5/10/15/20 轮上, 硬性带覆盖 5..11 轮 (每轮都要满足)
    sf::RuleTable rankTable, hardTable;
    QVector<sf::RuleBand> ranks, hards;
    {
        const int bandAt[4] = { 5, 10, 15, 20 };
        const u32 bandNeed[4] = {
            (1u << Z_GARG),                        // 必有白眼
            (1u << Z_CONE) | (1u << Z_NEWS),       // 路障或读报
            (1u << Z_BUCKET),                      // 铁桶
            (1u << Z_DOOR)                         // 铁门
        };
        for (int b = 0; b < 4; ++b) {
            sf::RuleBand rb;
            rb.beginFlag = bandAt[b] * 2;
            rb.endFlag = bandAt[b] * 2 + 2;
            rb.cond.needMask = bandNeed[b];
            rb.cond.sanitize();
            ranks << rb;
        }
        sf::RuleBand hb;
        hb.beginFlag = 10;      // 5 轮
        hb.endFlag = 22;        // 11 轮
        hb.cond.atLeastMask = (1u << Z_CONE) | (1u << Z_NEWS) | (1u << Z_BUCKET);
        hb.cond.atLeast = 2;
        hb.cond.sanitize();
        hards << hb;
    }
    rankTable.build(ranks, levelBegin, levelBegin + levelCount);
    hardTable.build(hards, levelBegin, levelBegin + levelCount);

    // ---- 摊平成 GPU 用的按轮次条件表 ----
    std::vector<CondGpu> rankLv(levelCount), hardLv(levelCount);
    memset(rankLv.data(), 0, sizeof(CondGpu) * levelCount);
    memset(hardLv.data(), 0, sizeof(CondGpu) * levelCount);
    auto fill = [](std::vector<CondGpu> &dst, const sf::RuleTable &t, unsigned lb, unsigned cnt) {
        for (unsigned i = 0; i < cnt; ++i) {
            int n = 0;
            const sf::WaveCondition *c = t.condsAt(int(lb + i), &n);
            if (!c || n <= 0) continue;
            // 本原型的用例里各规则带互不重叠, 每轮最多只有一条条件。
            // 真做进引擎时这里要改成"每轮一组条件, 全部满足才算命中"。
            CondGpu &g = dst[i];
            g.needMask = c->needMask;
            g.needTotal = (u32)c->needTotal;
            g.refuseMask = c->refuseMask;
            g.atLeastMask = c->atLeastMask;
            g.atLeast = (u32)c->atLeast;
            g.atMostMask = c->atMostMask;
            g.atMost = (u32)c->atMost;
            g.present = 1;
        }
    };
    fill(rankLv, rankTable, levelBegin, levelCount);
    fill(hardLv, hardTable, levelBegin, levelCount);

    // 第二个参数: 0 = 用硬性条件 (大部分种子早早淘汰, 接近"常见查询")
    //             1 = 关掉硬性条件 (每个种子都要把 25 轮全算完, 真实上界)
    const bool noHard = (argc > 2) && (atoi(argv[2]) != 0);
    if (noHard) {
        for (unsigned i = 0; i < levelCount; ++i) hardLv[i].present = 0;
    }

    // ---- CPU 参考: 逐种子算匹配轮数 ----
    auto condOk = [](const sf::WaveCondition *c, int n, quint32 bits) {
        for (int i = 0; i < n; ++i) {
            if (!c[i].matches(bits)) return false;
        }
        return true;
    };
    auto cpuSeed = [&](unsigned n, quint32 &outCode) {
        const quint32 seed = n;
        int matched = 0;
        for (unsigned i = 0; i < levelCount; ++i) {
            const unsigned lv = levelBegin + i;
            const quint32 rngSeed = quint32(seed + off32 + 101u * lv);
            quint32 bits = 0;
            sf::rowBitsBySeed(rngSeed, scene, int(lv), bits);
            int hn = 0, rn = 0;
            if (!noHard) {
                const sf::WaveCondition *hc = hardTable.condsAt(int(lv), &hn);
                if (hn > 0 && !condOk(hc, hn, bits)) { outCode = 0; return; }
            }
            const sf::WaveCondition *rc = rankTable.condsAt(int(lv), &rn);
            if (rn > 0 && condOk(rc, rn, bits)) ++matched;
        }
        outCode = quint32(matched + 1);
    };

    // ---- D3D11 ----
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        std::printf("D3D11CreateDevice failed 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    {
        IDXGIDevice *dxdev = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxdev))) {
            IDXGIAdapter *ad = nullptr;
            if (SUCCEEDED(dxdev->GetAdapter(&ad))) {
                DXGI_ADAPTER_DESC d{};
                ad->GetDesc(&d);
                std::wprintf(L"adapter: %s  VRAM %.1f GB\n", d.Description,
                             double(d.DedicatedVideoMemory) / (1024.0 * 1024.0 * 1024.0));
                ad->Release();
            }
            dxdev->Release();
        }
    }

    ID3DBlob *code = compileShader("CSSeed");
    if (!code) return 1;
    ID3D11ComputeShader *cs = nullptr;
    if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                        nullptr, &cs))) {
        std::printf("CreateComputeShader failed\n");
        return 1;
    }
    code->Release();

    // 每批种子数: 控制在 TDR (2 秒) 以内
    const unsigned chunk = 4000000u;
    const unsigned batch = span < chunk ? span : chunk;

    auto makeBuf = [&](UINT bytes, UINT stride, D3D11_BIND_FLAG bind, D3D11_USAGE usage,
                       UINT cpu, ID3D11Buffer **out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = bytes;
        bd.Usage = usage;
        bd.BindFlags = bind;
        bd.CPUAccessFlags = cpu;
        if (bind == D3D11_BIND_UNORDERED_ACCESS || bind == 0) {
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = stride;
        }
        return dev->CreateBuffer(&bd, nullptr, out);
    };

    ID3D11Buffer *outBuf = nullptr, *outStage = nullptr;
    ID3D11Buffer *rankBuf = nullptr, *hardBuf = nullptr;
    if (FAILED(makeBuf(batch * 4, 4, D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, &outBuf))) {
        std::printf("CreateBuffer(out) failed\n");
        return 1;
    }
    if (FAILED(makeBuf(batch * 4, 4, (D3D11_BIND_FLAG)0, D3D11_USAGE_STAGING,
                       D3D11_CPU_ACCESS_READ, &outStage))) {
        std::printf("CreateBuffer(stage) failed\n");
        return 1;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format = DXGI_FORMAT_UNKNOWN;
    uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavd.Buffer.NumElements = batch;
    ID3D11UnorderedAccessView *uav = nullptr;
    if (FAILED(dev->CreateUnorderedAccessView(outBuf, &uavd, &uav))) {
        std::printf("CreateUAV failed\n");
        return 1;
    }

    // 条件表: 上传一次
    auto makeSrv = [&](const std::vector<CondGpu> &v, ID3D11ShaderResourceView **srv,
                       ID3D11Buffer **buf) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT(v.size() * sizeof(CondGpu));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(CondGpu);
        D3D11_SUBRESOURCE_DATA sd{ v.data(), 0, 0 };
        dev->CreateBuffer(&bd, &sd, buf);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd2{};
        sd2.Format = DXGI_FORMAT_UNKNOWN;
        sd2.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd2.Buffer.NumElements = UINT(v.size());
        dev->CreateShaderResourceView(*buf, &sd2, srv);
    };
    ID3D11ShaderResourceView *rankSrv = nullptr, *hardSrv = nullptr;
    makeSrv(rankLv, &rankSrv, &rankBuf);
    makeSrv(hardLv, &hardSrv, &hardBuf);

    Params p{};
    p.off32 = off32;
    p.levelBegin = levelBegin;
    p.levelCount = levelCount;
    p.count = batch;
    p.allow = (u32)sceneAllowMask(scene);
    p.hasHard = 1;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(Params);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cd{ &p, 0, 0 };
    ID3D11Buffer *cbuf = nullptr;
    dev->CreateBuffer(&cbd, &cd, &cbuf);

    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cbuf);
    ID3D11ShaderResourceView *srvs[2] = { rankSrv, hardSrv };
    ctx->CSSetShaderResources(0, 2, srvs);

    const UINT groups = (batch + 127) / 128;

    // 预热
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->Dispatch(groups, 1, 1);
    ctx->CopyResource(outStage, outBuf);
    ctx->Map(outStage, 0, D3D11_MAP_READ, 0, nullptr);
    ctx->Unmap(outStage, 0);

    // ---- 计时: 跑 n 批, 每批都要回读 ----
    const unsigned nBatch = (span + batch - 1) / batch;
    std::vector<u32> matched(span, 0);
    auto t0 = std::chrono::high_resolution_clock::now();
    for (unsigned b = 0; b < nBatch; ++b) {
        const unsigned base = b * batch;
        const unsigned cnt = (span - base) < batch ? (span - base) : batch;
        p.base = base;
        p.count = cnt;
        ctx->UpdateSubresource(cbuf, 0, nullptr, &p, 0, 0);

        D3D11_UNORDERED_ACCESS_VIEW_DESC d2 = uavd;
        d2.Buffer.NumElements = cnt;
        ID3D11UnorderedAccessView *u2 = nullptr;
        dev->CreateUnorderedAccessView(outBuf, &d2, &u2);
        ctx->CSSetUnorderedAccessViews(0, 1, &u2, nullptr);
        ctx->Dispatch((cnt + 127) / 128, 1, 1);
        u2->Release();

        // 只回读 cnt 个 uint
        D3D11_BOX box{ 0, 0, 0, cnt * 4, 1, 1 };
        ctx->CopySubresourceRegion(outStage, 0, 0, 0, 0, outBuf, 0, &box);
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (SUCCEEDED(ctx->Map(outStage, 0, D3D11_MAP_READ, 0, &ms))) {
            memcpy(&matched[base], ms.pData, cnt * 4);
            ctx->Unmap(outStage, 0);
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    const double gpuMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // ---- 分档统计 (主机) ----
    const int activeWaves = rankTable.activeLevels();
    std::vector<unsigned long long> hist(size_t(activeWaves + 2), 0);
    std::vector<unsigned> example;
    unsigned full = 0, dead = 0;
    for (unsigned s = 0; s < span; ++s) {
        const u32 m = matched[s];
        if (m == 0) { ++dead; continue; }
        const int k = int(m) - 1;
        ++hist[size_t(k)];
        if (k == activeWaves) {
            ++full;
            if (example.size() < 8) example.push_back(s);
        }
    }

    std::printf("gpu pipeline: %8.1f ms for %u seeds x %u levels  (%.2f M seeds/s, %.2f M unit/s)\n",
                gpuMs, span, levelCount, double(span) / (gpuMs / 1000.0) / 1e6,
                double(span) * levelCount / (gpuMs / 1000.0) / 1e6);
    std::printf("  activeWaves=%d  full=%u  dead=%u\n", activeWaves, full, dead);
    std::printf("  hist:");
    for (int k = 0; k <= activeWaves; ++k) std::printf(" %llu", hist[size_t(k)]);
    std::printf("\n  examples:");
    for (size_t i = 0; i < example.size(); ++i) std::printf(" %08X", example[i]);
    std::printf("\n");

    // ---- 与 CPU 逐种子对比 ----
    {
        unsigned bad = 0;
        for (unsigned s = 0; s < span; ++s) {
            quint32 code = 0;
            cpuSeed(s, code);
            if (code != matched[s]) {
                if (bad < 5)
                    std::printf("  mismatch seed=%u gpu=%u cpu=%u\n", s, matched[s], code);
                ++bad;
            }
        }
        std::printf("verify: %s  (%u seeds compared, %u bad)\n",
                    bad == 0 ? "OK" : "FAILED", span, bad);
    }

    // ---- CPU 单线程参考耗时 ----
    {
        auto c0 = std::chrono::high_resolution_clock::now();
        quint32 sink = 0, code = 0;
        for (unsigned s = 0; s < span; ++s) { cpuSeed(s, code); sink += code; }
        auto c1 = std::chrono::high_resolution_clock::now();
        const double cpuMs = std::chrono::duration<double, std::milli>(c1 - c0).count();
        std::printf("cpu single thread: %8.1f ms  (%.3f M seeds/s)  sink=%u\n",
                    cpuMs, double(span) / (cpuMs / 1000.0) / 1e6, sink);
        std::printf("gpu / 1 thread = %.1fx\n", gpuMs > 0 ? cpuMs / gpuMs : 0.0);
    }

    cbuf->Release();
    hardSrv->Release();
    rankSrv->Release();
    hardBuf->Release();
    rankBuf->Release();
    uav->Release();
    outStage->Release();
    outBuf->Release();
    cs->Release();
    ctx->Release();
    dev->Release();
    return 0;
}
