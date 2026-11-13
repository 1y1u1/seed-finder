// GPU search core (CUDA) -- equivalent to the CPU Searcher, but using the
// dedup identity.
//
// Design notes (see _gpu/README.md for the speed model):
//  * The card table of one (seed s, wave lv) only depends on
//    R = s + off32 + 101*lv. So (s, lv) and (s+101, lv-1) share the same
//    table, and a contiguous seed range needs only "seeds + 101*(span-1)"
//    MT chains instead of seeds * waves.
//  * One CUDA thread computes several chains at once (kChains). The 398-step
//    MT prefix is a serial dependency chain of multiply-adds, so a single
//    chain leaves the pipeline idle; interleaving N independent chains fills
//    those slots without costing an order of magnitude more registers.
//  * Results are 8 words (d1[2..9]) per table, 32 B each, so 1M seeds is
//    ~32 MB and stays inside the 48 MB L2: each table is read 25 times but
//    fetched from HBM once.
//  * Matching happens in a second kernel which writes back only the matched
//    wave count as u16, so the host reads seeds*2 bytes regardless of the
//    number of conditions.
//
// NOTE: this file must stay pure ASCII. nvcc's cudafe++ parses sources in the
// system code page (936 here) and a UTF-8 Chinese comment can end with a byte
// that GBK decodes as 0x5C (backslash), which then swallows the next line.

#include "gpucuda.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
//  Device constants (identical to src/core/seedengine.cpp)
// ---------------------------------------------------------------------------
__device__ __constant__ int kConvIdDev[33] = {
    31, 24, 1, 2, 3,
    4, 5, 6, 7, 31,
    31, 8, 9, 31, 10,
    11, 12, 13, 14, 31,
    15, 16, 17, 18, 31, 25,
    31, 31, 31, 31, 31, 31,
    19
};

// Upper bound on the number of chains interleaved in one thread.
#define SF_MAX_CHAINS 8

__device__ __forceinline__ unsigned mtTemper(unsigned tmp)
{
    tmp ^= (tmp >> 11);
    tmp ^= ((tmp & 0xFF3A58ADu) << 7);
    tmp ^= ((tmp & 0xFFFFDF8Cu) << 15);
    tmp ^= (tmp >> 18);
    return tmp & 0x7FFFFFFFu;
}

__device__ __forceinline__ unsigned mtNext(unsigned prev, unsigned i)
{
    return (prev ^ (prev >> 30)) * 0x6C078965u + i;
}

// One card table = 8 words (d1[2..9]). d1[0] is always 1u and d1[1] is only
// used when lv <= 0.
struct Table
{
    unsigned w[8];      // w[i] == d1[i + 2]
};

// Advance a chain to word 397 (the last word the draws need to look back on).
__device__ __forceinline__ unsigned mtPrefix(unsigned seed)
{
    unsigned tail = seed;
    // Unrolled by 4 so the multiply-add latency overlaps within one chain too.
    #pragma unroll 4
    for (unsigned i = 1; i < 0x18Eu; ++i) tail = mtNext(tail, i);
    return tail;
}

// Draw phase. Bit-identical to LaneBatch::oneRow, with head[132] replaced by a
// rolling window of two words: a 132-word local array spills to local memory
// on the GPU, which costs an order of magnitude.
__device__ __forceinline__ bool mtDraw(unsigned seed, unsigned tail, unsigned allow, Table &out)
{
    unsigned a = seed;                      // head[0]
    unsigned b = mtNext(seed, 1u);          // head[1]
    unsigned k = 0u;

    unsigned d1[10];
    unsigned has = 1u;
    d1[0] = 1u;

    // tail holds word 397; the first update turns it into word 398.
    unsigned t = tail;
    {
        const unsigned tmp = (a & 0x80000000u) | (b & 0x7FFFFFFFu);
        const unsigned v = ((tmp >> 1) ^ t) ^ ((tmp & 1u) ? 0x9908B0DFu : 0u);
        t = mtNext(t, k + 0x18Eu);
        if (mtTemper(v) % 5u == 0u) {
            d1[0] |= (1u << 4);             // Z_NEWS
            has |= (1u << 4);
        } else {
            d1[0] |= (1u << 1);             // Z_CONE
            has |= (1u << 1);
        }
    }

    // Draws: at most 10 slots. The rolling window only reaches k ~ 80 in
    // practice; 130 is a hard guard that mirrors the CPU bail-out.
    int j = 1;
    #pragma unroll 1
    while (j < 10) {
        if (k + 2u >= 130u) return false;
        ++k;
        a = b;
        b = mtNext(b, k + 1u);

        const unsigned tmp = (a & 0x80000000u) | (b & 0x7FFFFFFFu);
        const unsigned v = ((tmp >> 1) ^ t) ^ ((tmp & 1u) ? 0x9908B0DFu : 0u);
        t = mtNext(t, k + 0x18Eu);
        const int nid = kConvIdDev[mtTemper(v) % 33u];
        if (has & (1u << nid)) continue;
        if (allow & (1u << nid)) {
            has |= (1u << nid);
            d1[j] = d1[j - 1];
            if (nid != 24 && nid != 25) d1[j] |= (1u << nid);
            ++j;
        }
    }

    // finishRow: move Z_GIGA forward, then fix the zomboni / garg order.
    for (int m = 2; m < 6; ++m) {
        if (d1[m] & (1u << 19)) d1[m] = d1[m + 1] ^ (1u << 19);
    }
    if (d1[2] & (1u << 9)) {
        if (d1[3] & (1u << 18)) {
            d1[2] = d1[4] ^ (1u << 9) ^ (1u << 18);
        } else {
            d1[2] = d1[3] ^ (1u << 9);
        }
    } else if (d1[2] & (1u << 18)) {
        if (d1[3] & (1u << 9)) {
            d1[2] = d1[4] ^ (1u << 9) ^ (1u << 18);
        } else {
            d1[2] = d1[3] ^ (1u << 18);
        }
    }

    for (int i = 0; i < 8; ++i) out.w[i] = d1[i + 2];
    return true;
}

// A draw never exhausts the prefix in practice; if it does, fall back to the
// engine's conservative value (plain zombie only), matching the CPU path.
__device__ __forceinline__ void fillDeadTable(Table &out)
{
    for (int i = 0; i < 8; ++i) out.w[i] = 1u;
}

// ---------------------------------------------------------------------------
//  Kernel 1: CHAINS tables per thread
//
//  CHAINS is a template parameter so the interleaving unrolls at compile time,
//  and so the common values can be compiled once each instead of being a
//  function pointer or a switch inside the hot loop.
// ---------------------------------------------------------------------------
template <int CHAINS>
__global__ void kBuildTables(unsigned r0, unsigned allow, unsigned count, Table *tables)
{
    const unsigned lane = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned stride = gridDim.x * blockDim.x;
    const unsigned first = lane * unsigned(CHAINS);

    if (first >= count) return;

    // Interleaved prefix phase: CHAINS independent dependency chains. This is
    // the whole point of the batching -- one chain alone stalls on the
    // multiply-add latency, CHAINS chains keep the pipeline busy.
    unsigned tails[CHAINS];
    #pragma unroll
    for (int c = 0; c < CHAINS; ++c) {
        const unsigned idx = first + unsigned(c);
        tails[c] = (idx < count) ? (r0 + idx) : 0u;
        if (idx < count && tails[c] == 0u) tails[c] = 0x1105u;
    }

    #pragma unroll 4
    for (unsigned i = 1; i < 0x18Eu; ++i) {
        #pragma unroll
        for (int c = 0; c < CHAINS; ++c) {
            if (first + unsigned(c) < count) tails[c] = mtNext(tails[c], i);
        }
    }

    // Draw phase: d1[10] is 10 registers, so run the chains one after another
    // instead of keeping CHAINS tables live at once.
    #pragma unroll
    for (int c = 0; c < CHAINS; ++c) {
        const unsigned idx = first + unsigned(c);
        if (idx >= count) continue;
        const unsigned seed = (r0 + idx) ? (r0 + idx) : 0x1105u;
        Table tb;
        if (!mtDraw(seed, tails[c], allow, tb)) fillDeadTable(tb);
        tables[idx] = tb;
    }
    (void)stride;
}

// Same shape, but the chains are strided instead of adjacent. Kept for
// experiments; the adjacent layout wins because the store phase is coalesced
// across the CHAINS slot rather than across threads.
template <int CHAINS>
__global__ void kBuildTablesStrided(unsigned r0, unsigned allow, unsigned count, Table *tables)
{
    const unsigned lane = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned stride = gridDim.x * blockDim.x;

    unsigned tails[CHAINS];
    #pragma unroll
    for (int c = 0; c < CHAINS; ++c) {
        const unsigned idx = lane + unsigned(c) * stride;
        tails[c] = (idx < count) ? (r0 + idx) : 0u;
        if (idx < count && tails[c] == 0u) tails[c] = 0x1105u;
    }

    #pragma unroll 4
    for (unsigned i = 1; i < 0x18Eu; ++i) {
        #pragma unroll
        for (int c = 0; c < CHAINS; ++c) {
            if (lane + unsigned(c) * stride < count) tails[c] = mtNext(tails[c], i);
        }
    }

    #pragma unroll
    for (int c = 0; c < CHAINS; ++c) {
        const unsigned idx = lane + unsigned(c) * stride;
        if (idx >= count) continue;
        const unsigned seed = (r0 + idx) ? (r0 + idx) : 0x1105u;
        Table tb;
        if (!mtDraw(seed, tails[c], allow, tb)) fillDeadTable(tb);
        tables[idx] = tb;
    }
}

__device__ __forceinline__ bool condMatches(const CondG &c, unsigned bits)
{
    if (__popc(bits & c.needMask) != c.needTotal) return false;
    if (bits & c.refuseMask) return false;
    if (c.atLeastMask && __popc(bits & c.atLeastMask) < c.atLeast) return false;
    if (c.atMostMask && __popc(bits & c.atMostMask) > c.atMost) return false;
    return true;
}

// ---------------------------------------------------------------------------
//  Kernel 2: one seed per thread. Tables come from L2, no MT recomputation.
// ---------------------------------------------------------------------------
__global__ void kEvalSeeds(unsigned seedCount, unsigned levelCount,
                           const LevelDesc *levels, const CondG *conds,
                           const Table *tables, unsigned short *out)
{
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= seedCount) return;

    unsigned matched = 0u;
    bool alive = true;

    #pragma unroll 1
    for (unsigned li = 0; li < levelCount && alive; ++li) {
        const LevelDesc d = levels[li];
        const int lv = d.level;

        // Wave lv reads d1[lv+1], i.e. table slot lv-1; lv >= 8 always reads
        // d1[9], which is slot 7.
        unsigned bits;
        if (lv <= 0) {
            bits = 11u;                     // F 1..2: plain zombie only
        } else {
            const unsigned col = (lv >= 8) ? 7u : unsigned(lv - 1);
            bits = tables[i + d.tableStride].w[col];
        }

        for (unsigned c = 0; c < d.hardCnt; ++c) {
            if (!condMatches(conds[d.hardOff + c], bits)) { alive = false; break; }
        }
        if (!alive) break;

        if (d.rankCnt > 0) {
            bool ok = true;
            for (unsigned c = 0; c < d.rankCnt; ++c) {
                if (!condMatches(conds[d.rankOff + c], bits)) { ok = false; break; }
            }
            if (ok) ++matched;
        }
    }

    out[i] = alive ? (unsigned short)(matched + 1u) : 0u;
}

int gBlockSize = 256;
int gChains = 4;
int gStrided = 1;

// Blocks needed when each thread owns CHAINS tables.
inline unsigned blocksFor(unsigned total, unsigned threads, int chains)
{
    if (chains < 1) chains = 1;
    const unsigned per = (total + unsigned(chains) - 1) / unsigned(chains);
    return (per + threads - 1) / threads;
}

// Launch kBuildTables with the compile-time chain count. CHAINS unrolls at
// compile time so the interleaved prefix loop uses plain registers.
void launchBuild(unsigned r0, unsigned allow, unsigned count, Table *tables,
                 int blockSize, int chains, int strided)
{
    const unsigned threads = unsigned(blockSize);
#define SF_LAUNCH(N)                                                          \
    do {                                                                      \
        const unsigned blocks = blocksFor(count, threads, N);                  \
        if (strided) kBuildTablesStrided<N><<<blocks, threads>>>(r0, allow, count, tables); \
        else         kBuildTables<N><<<blocks, threads>>>(r0, allow, count, tables);        \
    } while (0)

    switch (chains) {
    case 1: SF_LAUNCH(1); break;
    case 2: SF_LAUNCH(2); break;
    case 3: SF_LAUNCH(3); break;
    case 5: SF_LAUNCH(5); break;
    case 6: SF_LAUNCH(6); break;
    case 7: SF_LAUNCH(7); break;
    case 8: SF_LAUNCH(8); break;
    default: SF_LAUNCH(4); break;
    }
#undef SF_LAUNCH
}

} // namespace

// ---------------------------------------------------------------------------
//  Host side
// ---------------------------------------------------------------------------
int gpuDeviceCount()
{
    int n = 0;
    if (cudaGetDeviceCount(&n) != cudaSuccess) return 0;
    return n;
}

void gpuDeviceInfo(char *name, int nameLen, int *sms, double *gbps)
{
    cudaDeviceProp p{};
    if (cudaGetDeviceProperties(&p, 0) != cudaSuccess) return;
    if (name && nameLen > 0) std::snprintf(name, size_t(nameLen), "%s", p.name);
    if (sms) *sms = p.multiProcessorCount;
    if (gbps) {
        int mclk = 0;
        cudaDeviceGetAttribute(&mclk, cudaDevAttrMemoryClockRate, 0);
        *gbps = double(mclk) * 1000.0 * 2.0 * double(p.memoryBusWidth) / 8.0 / 1e9;
    }
}

void gpuSetTuning(int blockSize, int chainsPerThread, int strided)
{
    if (blockSize >= 32 && blockSize <= 1024 && (blockSize % 32) == 0) gBlockSize = blockSize;
    if (chainsPerThread >= 1 && chainsPerThread <= SF_MAX_CHAINS) gChains = chainsPerThread;
    gStrided = (strided != 0) ? 1 : 0;
}

void gpuGetTuning(int *blockSize, int *chainsPerThread, int *strided)
{
    if (blockSize) *blockSize = gBlockSize;
    if (chainsPerThread) *chainsPerThread = gChains;
    if (strided) *strided = gStrided;
}

struct GpuSearcher::Impl
{
    unsigned allow = 0;
    unsigned off32 = 0;
    // First level that needs a card table. levels may still contain level <= 0
    // entries (their bits are constant 11u), so this cannot be levels.front().
    int lvFirst = 0;

    std::vector<LevelDesc> levels;
    std::vector<CondG> conds;

    LevelDesc *dLevels = nullptr;
    CondG *dConds = nullptr;
    Table *dTables = nullptr;
    unsigned short *dOut = nullptr;
    unsigned short *hOut = nullptr;         // pinned

    size_t tableCap = 0;
    size_t outCap = 0;

    double lastBuildMs = 0.0;
    double lastEvalMs = 0.0;

    ~Impl() { release(); }

    void release()
    {
        if (dLevels) { cudaFree(dLevels); dLevels = nullptr; }
        if (dConds) { cudaFree(dConds); dConds = nullptr; }
        if (dTables) { cudaFree(dTables); dTables = nullptr; }
        if (dOut) { cudaFree(dOut); dOut = nullptr; }
        if (hOut) { cudaFreeHost(hOut); hOut = nullptr; }
        tableCap = outCap = 0;
    }

    bool ensureTables(size_t n)
    {
        if (n <= tableCap) return true;
        if (dTables) { cudaFree(dTables); dTables = nullptr; }
        if (cudaMalloc(reinterpret_cast<void **>(&dTables), n * sizeof(Table)) != cudaSuccess) {
            tableCap = 0;
            return false;
        }
        tableCap = n;
        return true;
    }

    bool ensureOut(size_t n)
    {
        if (n <= outCap) return true;
        if (dOut) { cudaFree(dOut); dOut = nullptr; }
        if (hOut) { cudaFreeHost(hOut); hOut = nullptr; }
        if (cudaMalloc(reinterpret_cast<void **>(&dOut), n * sizeof(unsigned short)) != cudaSuccess) {
            outCap = 0;
            return false;
        }
        if (cudaMallocHost(reinterpret_cast<void **>(&hOut), n * sizeof(unsigned short)) != cudaSuccess) {
            cudaFree(dOut);
            dOut = nullptr;
            outCap = 0;
            return false;
        }
        outCap = n;
        return true;
    }
};

GpuSearcher::GpuSearcher() : m_impl(new Impl) {}

GpuSearcher::~GpuSearcher() { delete m_impl; }

void GpuSearcher::lastTiming(double *buildMs, double *evalMs) const
{
    if (buildMs) *buildMs = m_impl->lastBuildMs;
    if (evalMs) *evalMs = m_impl->lastEvalMs;
}

bool GpuSearcher::prepare(const std::vector<LevelDesc> &levels,
                          const std::vector<CondG> &conds,
                          unsigned allow, unsigned off32, int lvFirst, std::string *error)
{
    Impl &d = *m_impl;
    d.allow = allow;
    d.off32 = off32;
    d.lvFirst = lvFirst;
    d.levels = levels;
    d.conds = conds;

    if (levels.empty()) {
        if (error) *error = "no levels to check";
        return false;
    }

    if (d.dLevels) { cudaFree(d.dLevels); d.dLevels = nullptr; }
    if (d.dConds) { cudaFree(d.dConds); d.dConds = nullptr; }

    const size_t lvBytes = levels.size() * sizeof(LevelDesc);
    const size_t cBytes = std::max<size_t>(1, conds.size()) * sizeof(CondG);

    if (cudaMalloc(reinterpret_cast<void **>(&d.dLevels), lvBytes) != cudaSuccess) {
        if (error) *error = "out of device memory (levels)";
        return false;
    }
    if (cudaMalloc(reinterpret_cast<void **>(&d.dConds), cBytes) != cudaSuccess) {
        if (error) *error = "out of device memory (conditions)";
        return false;
    }
    cudaMemcpy(d.dLevels, levels.data(), lvBytes, cudaMemcpyHostToDevice);
    if (!conds.empty()) {
        cudaMemcpy(d.dConds, conds.data(), conds.size() * sizeof(CondG),
                   cudaMemcpyHostToDevice);
    }
    return true;
}

bool gpuDumpTables(unsigned r0, unsigned allow, unsigned count,
                   std::vector<unsigned> &out, std::string *error)
{
    out.clear();
    Table *dTables = nullptr;
    if (cudaMalloc(reinterpret_cast<void **>(&dTables), size_t(count) * sizeof(Table)) != cudaSuccess) {
        if (error) *error = "out of device memory";
        return false;
    }
    launchBuild(r0, allow, count, dTables, gBlockSize, gChains, gStrided);

    const cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        cudaFree(dTables);
        if (error) *error = cudaGetErrorString(err);
        return false;
    }

    out.resize(size_t(count) * 8);
    if (cudaMemcpy(out.data(), dTables, out.size() * sizeof(unsigned),
                   cudaMemcpyDeviceToHost) != cudaSuccess) {
        cudaFree(dTables);
        if (error) *error = "device to host copy failed";
        return false;
    }
    cudaFree(dTables);
    return true;
}

bool GpuSearcher::scan(unsigned seedBegin, unsigned seedCount,
                       std::vector<unsigned short> &out, std::string *error)
{
    Impl &d = *m_impl;
    out.clear();
    if (d.levels.empty()) {
        if (error) *error = "no levels to check";
        return false;
    }

    // Table count = seeds + 101*(max wave span). Direct consequence of the
    // dedup identity.
    const unsigned maxStride = d.levels.back().tableStride;
    const size_t needTables = size_t(seedCount) + maxStride;
    if (!d.ensureTables(needTables) || !d.ensureOut(seedCount)) {
        if (error) *error = "out of device memory";
        return false;
    }

    // Table j holds R = r0 + j, with r0 = seedBegin + off32 + 101*lvFirst.
    const int lvFirst = d.lvFirst;
    const unsigned r0 = unsigned(seedBegin + d.off32 + 101u * unsigned(lvFirst));

    cudaEvent_t evA = nullptr;
    cudaEvent_t evB = nullptr;
    cudaEvent_t evC = nullptr;
    cudaEventCreate(&evA);
    cudaEventCreate(&evB);
    cudaEventCreate(&evC);

    cudaEventRecord(evA);
    launchBuild(r0, d.allow, unsigned(needTables), d.dTables, gBlockSize, gChains, gStrided);
    cudaEventRecord(evB);

    const int threads = gBlockSize;
    const int blocksB = int((seedCount + unsigned(threads) - 1) / unsigned(threads));
    kEvalSeeds<<<blocksB, threads>>>(seedCount, unsigned(d.levels.size()),
                                     d.dLevels, d.dConds, d.dTables, d.dOut);
    cudaEventRecord(evC);

    const cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        if (error) *error = cudaGetErrorString(err);
        cudaEventDestroy(evA); cudaEventDestroy(evB); cudaEventDestroy(evC);
        return false;
    }

    if (cudaMemcpyAsync(d.hOut, d.dOut, seedCount * sizeof(unsigned short),
                        cudaMemcpyDeviceToHost, nullptr) != cudaSuccess) {
        cudaDeviceSynchronize();
        // Pageable fallback: copy through a plain buffer.
        std::vector<unsigned short> stage(seedCount);
        if (cudaMemcpy(stage.data(), d.dOut, seedCount * sizeof(unsigned short),
                       cudaMemcpyDeviceToHost) != cudaSuccess) {
            if (error) *error = "device to host copy failed";
            cudaEventDestroy(evA); cudaEventDestroy(evB); cudaEventDestroy(evC);
            return false;
        }
        out.swap(stage);
        cudaEventDestroy(evA); cudaEventDestroy(evB); cudaEventDestroy(evC);
        return true;
    }
    if (cudaDeviceSynchronize() != cudaSuccess) {
        if (error) *error = cudaGetErrorString(cudaGetLastError());
        cudaEventDestroy(evA); cudaEventDestroy(evB); cudaEventDestroy(evC);
        return false;
    }

    float msBuild = 0.0f;
    float msEval = 0.0f;
    cudaEventElapsedTime(&msBuild, evA, evB);
    cudaEventElapsedTime(&msEval, evB, evC);
    d.lastBuildMs = double(msBuild);
    d.lastEvalMs = double(msEval);
    cudaEventDestroy(evA);
    cudaEventDestroy(evB);
    cudaEventDestroy(evC);

    out.assign(d.hOut, d.hOut + seedCount);
    return true;
}
