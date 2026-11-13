// GPU 加速可行性原型 (D3D11 compute shader)
// 只用于本地评估, 不参与 SeedFinder.pro 构建。
//
// 目的: 用 GPU 算 "一批 (种子, 轮次) 组合的上场表", 与 CPU 的 rowBitsBySeed
//       逐位对比, 并测吞吐。
//
// 关键发现 (直接影响 GPU 实现): seedfinder3/seedengine.cpp 的 LaneBatch 保留了
// head[0..kLaneHead-1] 整段 MT 初始化前缀, 但抽卡时 k 是逐次 +1 前进的, 每次只用
// head[k] 和 head[k+1]。所以改用"滚动两词"(a=head[k], b=head[k+1])就能算, 寄存器
// 从 132 个词降到 2 个 —— 这对 GPU 很重要, 因为大数组会溢到 local memory。

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

// 直接包含引擎实现, 以便调用匿名命名空间里的 rowBitsBySeed
#include "../seedfinder3/seedengine.cpp"

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

cbuffer Params : register(b0)
{
    uint g_base;
    uint g_off32;
    uint g_levelBegin;
    uint g_nLevels;
    uint g_allow;
    uint g_needMask;
    uint g_needTotal;
    uint g_refuseMask;
    uint g_atLeastMask;
    uint g_atMostMask;
    uint g_atLeast;
    uint g_atMost;
    uint g_count;
    uint g_doCond;      // 0 = 只输出 bits (用来比 CPU), 1 = 输出条件判定
    uint g_pad[2];
};

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

// head[i] = (head[i-1] ^ (head[i-1] >> 30)) * 0x6C078965 + i
uint mtNext(uint prev, uint i)
{
    return (prev ^ (prev >> 30)) * 0x6C078965u + i;
}

[numthreads(128, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    const uint gid = tid.x;
    if (gid >= g_count) return;

    const uint n = gid / g_nLevels;
    const uint li = gid - n * g_nLevels;
    const uint lv = g_levelBegin + li;
    const uint seed = g_base + n;

    // 与 CPU 端 rs = quint32(s + off32 + 101*lv) 一致
    const uint rngSeedRaw = seed + g_off32 + 101u * lv;
    const uint rngSeed = (rngSeedRaw == 0u) ? 0x1105u : rngSeedRaw;

    // --- MT 初始化: 只要最后一个词 (下标 0x18D) ---
    uint tail = rngSeed;
    [loop]
    for (uint i = 1; i < 0x18E; ++i) tail = mtNext(tail, i);

    // --- 滚动两词: a = head[k], b = head[k+1] ---
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

    // --- 红眼前移 + 冰车/白眼顺序修正 ---
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

    const uint bits = (lv >= 8u) ? d1[9] : d1[lv + 1u];

    if (g_doCond == 0u) {
        g_out[gid] = bits;
        return;
    }

    uint ok = 1u;
    if (countbits(bits & g_needMask) != g_needTotal) ok = 0u;
    if (bits & g_refuseMask) ok = 0u;
    if (g_atLeastMask && countbits(bits & g_atLeastMask) < g_atLeast) ok = 0u;
    if (g_atMostMask && countbits(bits & g_atMostMask) > g_atMost) ok = 0u;
    g_out[gid] = ok ? 1u : 0u;
}
)HLSL";

// ---------------------------------------------------------------------------
static ID3DBlob *compileShader()
{
    ID3DBlob *code = nullptr, *err = nullptr;
    const HRESULT hr = D3DCompile(kShader, strlen(kShader), "seed.hlsl", nullptr, nullptr,
                                  "CSMain", "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        std::printf("shader compile failed: %s\n",
                    err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

int main(int argc, char **argv)
{
    int span = 200000;
    if (argc > 1) span = atoi(argv[1]);
    const unsigned nLevels = 25;
    const int scene = 2;
    const unsigned uid = 1, mode = 13;
    const unsigned off32 = uid + mode;

    // ---- CPU 侧参考实现: 直接调引擎的 rowBitsBySeed ----
    // rowBitsBySeed 在 seedengine.cpp 的匿名命名空间里, 但要带 sf:: 限定符才能引用
    auto cpuRow = [&](unsigned n, unsigned lv, quint32 &out) {
        const quint32 rngSeed = quint32(quint32(n) + off32 + 101u * lv);
        return sf::rowBitsBySeed(rngSeed, scene, int(lv), out);
    };

    // ---- D3D11 设备 ----
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

    ID3DBlob *code = compileShader();
    if (!code) return 1;
    ID3D11ComputeShader *cs = nullptr;
    if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                        nullptr, &cs))) {
        std::printf("CreateComputeShader failed\n");
        return 1;
    }
    code->Release();

    const unsigned total = (unsigned)span * nLevels;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = total * 4;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 4;
    ID3D11Buffer *buf = nullptr;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &buf))) {
        std::printf("CreateBuffer failed (%.1f MB)\n", total * 4.0 / 1048576.0);
        return 1;
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format = DXGI_FORMAT_UNKNOWN;
    uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavd.Buffer.NumElements = total;
    ID3D11UnorderedAccessView *uav = nullptr;
    if (FAILED(dev->CreateUnorderedAccessView(buf, &uavd, &uav))) {
        std::printf("CreateUAV failed\n");
        return 1;
    }

    D3D11_BUFFER_DESC sbd = bd;
    sbd.Usage = D3D11_USAGE_STAGING;
    sbd.BindFlags = 0;
    sbd.MiscFlags = 0;
    sbd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer *staging = nullptr;
    if (FAILED(dev->CreateBuffer(&sbd, nullptr, &staging))) {
        std::printf("CreateStaging failed\n");
        return 1;
    }

    struct Params {
        u32 base, off32, levelBegin, nLevels;
        u32 allow, needMask, needTotal, refuseMask;
        u32 atLeastMask, atMostMask, atLeast, atMost;
        u32 count, doCond, pad[2];
    } p{};
    p.base = 0;
    p.off32 = off32;
    p.levelBegin = 1;
    p.nLevels = nLevels;
    p.allow = (u32)sceneAllowMask(scene);
    p.needMask = 1u << Z_GARG;
    p.needTotal = 1;
    p.count = total;
    p.doCond = 0;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(Params);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cdata{ &p, 0, 0 };
    ID3D11Buffer *cbuf = nullptr;
    if (FAILED(dev->CreateBuffer(&cbd, &cdata, &cbuf))) {
        std::printf("CreateCB failed\n");
        return 1;
    }

    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cbuf);
    ID3D11UnorderedAccessView *uavs[1] = { uav };
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    // 预热一次, 避免首次的驱动/时钟抖动算进成绩
    const UINT groups = (UINT)((total + 127) / 128);
    ctx->Dispatch(groups, 1, 1);
    ctx->CopyResource(staging, buf);
    ctx->Map(staging, 0, D3D11_MAP_READ, 0, nullptr);
    ctx->Unmap(staging, 0);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int rep = 0; rep < 3; ++rep) {
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        ctx->Dispatch(groups, 1, 1);
    }
    ctx->CopyResource(staging, buf);
    D3D11_MAPPED_SUBRESOURCE map{};
    ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map);
    auto t1 = std::chrono::high_resolution_clock::now();
    const double gpuMs = std::chrono::duration<double, std::milli>(t1 - t0).count() / 3.0;
    const u32 *res = (const u32 *)map.pData;

    const double units = double(total) / 1e6;
    const double gpuRate = units / (gpuMs / 1000.0);
    std::printf("gpu:  %8.1f ms  %7.2f M (seed,level)/s\n", gpuMs, gpuRate);

    // ---- 逐位对比 (用全部, 不只抽样) ----
    {
        unsigned bad = 0;
        unsigned firstBad = 0;
        for (unsigned i = 0; i < total; ++i) {
            const unsigned n = i / nLevels;
            const unsigned lv = 1 + (i - n * nLevels);
            quint32 b = 0;
            cpuRow(n, lv, b);
            if (res[i] != b) {
                if (bad == 0) firstBad = i;
                if (bad < 5)
                    std::printf("  mismatch i=%u seed=%u lv=%u gpu=%08X cpu=%08X\n",
                                i, n, lv, res[i], b);
                ++bad;
            }
        }
        std::printf("verify: %s  (%u compared, %u bad%s)\n",
                    bad == 0 ? "OK" : "FAILED", total, bad,
                    bad ? "" : "");
        (void)firstBad;
    }
    ctx->Unmap(staging, 0);

    // ---- CPU 单线程同规模耗时 ----
    {
        const unsigned unitsCpu = total;
        auto c0 = std::chrono::high_resolution_clock::now();
        unsigned long long sink = 0;
        for (unsigned i = 0; i < unitsCpu; ++i) {
            const unsigned n = i / nLevels;
            const unsigned lv = 1 + (i - n * nLevels);
            quint32 b = 0;
            cpuRow(n, lv, b);
            sink += b;
        }
        auto c1 = std::chrono::high_resolution_clock::now();
        const double cpuMs = std::chrono::duration<double, std::milli>(c1 - c0).count();
        const double cpuRate = units / (cpuMs / 1000.0);
        std::printf("cpu1: %8.1f ms  %7.2f M (seed,level)/s  (single thread, sink=%llu)\n",
                    cpuMs, cpuRate, sink);
        std::printf("gpu / 1 cpu-thread = %.1fx   (24 核实测全机约 %.1f M seeds/s @25 轮)\n",
                    gpuRate / cpuRate, 2.2);
    }

    staging->Release();
    cbuf->Release();
    uav->Release();
    buf->Release();
    cs->Release();
    ctx->Release();
    dev->Release();
    return 0;
}
