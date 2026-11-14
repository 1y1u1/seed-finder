#include "seedengine.h"

#include "gpuhook.h"    // GPU 后端抽象 (没接后端时 gpuBackend() 返回空, 自动走 CPU)

#include <QElapsedTimer>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>          // SSE4.1 整数乘法, __cpuid
#define SF_SIMD_X86 1
#endif

namespace sf {

// ===========================================================================
//  游戏 RNG 复刻 (与 worker.cpp 的 page 类逐位一致)
// ===========================================================================
namespace {

// id -> 僵尸 转换表
static const int kConvId[33] = {
    31, 24, 1, 2, 3,
    4, 5, 6, 7, 31,
    31, 8, 9, 31, 10,
    11, 12, 13, 14, 31,
    15, 16, 17, 18, 31, 25,
    31, 31, 31, 31, 31, 31,
    19
};

// MT19937 的单行实例。缓冲区比原版 (0x270) 略大, 避免越界写,
// 在有效下标范围内与原版完全等价。
struct RowRng
{
    quint32 buf[0x400];
    int idx;

    void rngInit(quint32 seed)
    {
        buf[0] = seed;
        for (int i = 1; i < 0x18E; ++i) {
            buf[i] = (buf[i - 1] ^ (buf[i - 1] >> 30)) * 0x6C078965u + quint32(i);
        }
        idx = 0;
    }

    quint32 rngGen(int max)
    {
        quint32 tmp = (buf[idx] & 0x80000000u) | (buf[idx + 1] & 0x7FFFFFFFu);
        buf[idx] = (tmp >> 1) ^ buf[(idx + 0x18D) % 0x270];
        if (tmp & 1u) buf[idx] ^= 0x9908B0DFu;

        buf[idx + 0x18D + 1] =
            (buf[idx + 0x18D] ^ (buf[idx + 0x18D] >> 30)) * 0x6C078965u + quint32(idx + 0x18D + 1);

        tmp = buf[idx];
        tmp ^= (tmp >> 11);
        tmp ^= ((tmp & 0xFF3A58ADu) << 7);
        tmp ^= ((tmp & 0xFFFFDF8Cu) << 15);
        tmp ^= (tmp >> 18);
        ++idx;
        return (tmp & 0x7FFFFFFFu) % quint32(max);
    }
};

// 名字有点长, 线程局部复用, 省掉每次 2.5KB 的栈构造
#if defined(_MSC_VER)
__declspec(thread) static RowRng t_rng;
#else
static thread_local RowRng t_rng;
#endif

// ---------------------------------------------------------------------------
//  批量抽卡
//  同一轮的多个种子是相互独立的 MT 链, 而 MT 初始化链的第 i 个词只依赖第 i-1 个词,
//  所以可以把 K 条链并排放在一起递推 (SoA)。MSVC 不会自动把这种递推向量化
//  (实测 /arch:AVX2 下 pmulld 仍是 2 条、imul 87 条), 所以这层 lane 内循环
//  自己用 SSE4.1 的 4 宽整数乘法手工展开, 标量版留作非 x86 平台的退路。
//  抽卡只需要前缀 head[0..kLaneHead-1] 和第 0x18D 个词 (tail):
//  在 idx < 227 时 (idx + 0x18D) % 0x270 == idx + 0x18D, 不会回绕。
// ---------------------------------------------------------------------------
const int kLanes = 16;       // 每批 lane 数 (实测 16 与 32 只差 1%)
const int kLaneHead = 132;   // 保留的 MT 前缀词数 (实测最多只用到 79 个)

// 去重批大小。一行 (种子 s, 第 lv 轮) 的选卡表只取决于
//   R = s + offset + 101*lv  (mod 2^32)
// 所以 (s, lv) 与 (s+101, lv-1) 是同一张表: 连续 N 个种子 × W 轮只需要
// N + 101*(W-1) 条 MT 链, 而不是 N*W 条 —— 全量 42.9 亿种子查 25 轮时,
// 链数从 1072 亿降到 42.9 亿。块越大, 101 的"边界代价"摊得越薄。
const int kBlock = 2048;
// 去重表的内存上限 (张, 每张 40 字节)。表数 = 101*(轮跨度-1) + 块大小,
// 65536 张约 2.6 MB, 覆盖到 630 多轮; 再宽就退回"按轮各算各的",
// 免得为了省算力反而把表扫出缓存。
const int kMaxTables = 65536;

#if defined(SF_SIMD_X86)
// SSE4.1 有 _mm_mullo_epi32 (一次算 4 个 32 位整数乘法)。x64 的编译基线只有 SSE2,
// 所以运行时探测一次, 老机器退回标量实现。
// (试过 8 宽的 AVX2 版本, 实测比 4 宽慢 10% 左右 —— 8 条 lane 的存取跨了两条
//  cache line, 抵消了少一半指令的收益, 所以只用 SSE4.1。)
inline bool sfHasSse41()
{
#if defined(_MSC_VER)
    int info[4] = {0, 0, 0, 0};
    __cpuid(info, 1);
    return (info[2] & (1 << 19)) != 0;      // ECX bit19 = SSE4.1
#elif defined(__GNUC__)
    return __builtin_cpu_supports("sse4.1");
#else
    return false;
#endif
}

inline bool sfSimdInit()
{
    static const bool ok = sfHasSse41();
    return ok;
}
#endif

// MT 输出变换 (含原版 rngGen 末尾的 & 0x7FFFFFFF)
inline quint32 mtTemper(quint32 tmp)
{
    tmp ^= (tmp >> 11);
    tmp ^= ((tmp & 0xFF3A58ADu) << 7);
    tmp ^= ((tmp & 0xFFFFDF8Cu) << 15);
    tmp ^= (tmp >> 18);
    return tmp & 0x7FFFFFFFu;
}

// 红眼前移 + 冰车/白眼顺序修正
inline void finishRow(quint32 d1[10])
{
    for (int j = 2; j < 6; ++j) {
        if (d1[j] & (1u << Z_GIGA)) d1[j] = d1[j + 1] ^ (1u << Z_GIGA);
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
}

// draw(max) 返回 -1 表示这一条 lane 的抽卡数超出了保留前缀
template <class Draw>
inline bool buildRow(int scene, Draw &draw, quint32 d1[10])
{
    const quint32 allow = quint32(sceneAllowMask(scene));

    quint32 has = 1u;               // 普僵 (id 0 → 位 0) 恒出现
    d1[0] = 1u;
    const int first = draw(5);
    if (first < 0) return false;
    if (first == 0) {
        d1[0] |= (1u << Z_NEWS);
        has |= (1u << Z_NEWS);
    } else {
        d1[0] |= (1u << Z_CONE);
        has |= (1u << Z_CONE);
    }

    int j = 1;
    while (j < 10) {
        const int type = draw(33);
        if (type < 0) return false;
        const int nid = kConvId[type];
        if (has & (1u << nid)) continue;
        if (allow & (1u << nid)) {
            has |= (1u << nid);
            d1[j] = d1[j - 1];
            if (nid != 24 && nid != 25) d1[j] |= (1u << nid);
            ++j;
        }
    }
    finishRow(d1);
    return true;
}

struct SerialDraw
{
    RowRng *rng;
    int operator()(int max) { return int(rng->rngGen(max)); }
};

// K 条 lane 的 MT 前缀, 交错递推
template <int K>
struct LaneBatch
{
    quint32 head[kLaneHead][K];
    quint32 tail[K];

    // count: 只有前 count 条 lane 有效 (大部分种子在前几轮就被硬性条件刷掉,
    // 只算还活着的那几条, 不必为已死的 lane 付 398 步初始化的钱)
    void init(const quint32 rs[K], int count)
    {
        if (count >= K) initImpl<K>(rs, 0);   // 满 lane: 用编译期上界, 便于向量化
        else initImpl<0>(rs, count);
    }

    // N > 0 时以 N 为编译期 lane 数, 否则用运行期 count
    template <int N>
    void initImpl(const quint32 rs[K], int count)
    {
        const int n = (N > 0) ? N : count;
        for (int l = 0; l < n; ++l) {
            const quint32 s = rs[l] ? rs[l] : 0x1105u;
            head[0][l] = s;
            tail[l] = s;
        }
#if defined(SF_SIMD_X86)
        if (sfSimdInit()) {
            initSimd(n);
            return;
        }
#endif
        initScalar(n);
    }

    // 标量递推: tail[i] = (tail[i-1] ^ (tail[i-1] >> 30)) * 0x6C078965 + i
    void initScalar(int n)
    {
        for (int i = 1; i < 0x18E; ++i) {
            const quint32 ii = quint32(i);
            if (i < kLaneHead) {
                quint32 *h = head[i];
                for (int l = 0; l < n; ++l) {
                    const quint32 x = tail[l];
                    const quint32 v = (x ^ (x >> 30)) * 0x6C078965u + ii;
                    h[l] = v;
                    tail[l] = v;
                }
            } else {
                for (int l = 0; l < n; ++l) {
                    const quint32 x = tail[l];
                    tail[l] = (x ^ (x >> 30)) * 0x6C078965u + ii;
                }
            }
        }
    }

#if defined(SF_SIMD_X86)
    // 同一个 i 上 4 条 lane 一起算 (4 宽)
    void initSimd(int n)
    {
        const __m128i mul = _mm_set1_epi32(int(0x6C078965u));
        for (int i = 1; i < 0x18E; ++i) {
            const __m128i add = _mm_set1_epi32(int(quint32(i)));
            quint32 *h = (i < kLaneHead) ? head[i] : nullptr;
            int l = 0;
            for (; l + 4 <= n; l += 4) {
                const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i *>(tail + l));
                __m128i v = _mm_xor_si128(x, _mm_srli_epi32(x, 30));
                v = _mm_add_epi32(_mm_mullo_epi32(v, mul), add);
                _mm_storeu_si128(reinterpret_cast<__m128i *>(tail + l), v);
                if (h) _mm_storeu_si128(reinterpret_cast<__m128i *>(h + l), v);
            }
            for (; l < n; ++l) {
                const quint32 x = tail[l];
                const quint32 v = (x ^ (x >> 30)) * 0x6C078965u + quint32(i);
                tail[l] = v;
                if (h) h[l] = v;
            }
        }
    }
#endif

    // 单条 lane 的完整选卡表 (d1[1..9] 就是各轮的累计上场表)
    // 返回 false = 抽卡数超出保留前缀, 调用方需要退回串行补算
    bool oneRow(quint32 allow, int l, quint32 d1[10]) const
    {
        quint32 t = tail[l];
        int k = 0;
        quint32 has = 1u;
        d1[0] = 1u;
        {
            const quint32 tmp = (head[0][l] & 0x80000000u) | (head[1][l] & 0x7FFFFFFFu);
            const quint32 v = ((tmp >> 1) ^ t) ^ ((tmp & 1u) ? 0x9908B0DFu : 0u);
            t = (t ^ (t >> 30)) * 0x6C078965u + quint32(k + 0x18E);
            ++k;
            if (mtTemper(v) % 5u == 0u) {
                d1[0] |= (1u << Z_NEWS);
                has |= (1u << Z_NEWS);
            } else {
                d1[0] |= (1u << Z_CONE);
                has |= (1u << Z_CONE);
            }
        }
        int j = 1;
        while (j < 10) {
            if (k + 1 >= kLaneHead) return false;
            const quint32 tmp = (head[k][l] & 0x80000000u) | (head[k + 1][l] & 0x7FFFFFFFu);
            const quint32 v = ((tmp >> 1) ^ t) ^ ((tmp & 1u) ? 0x9908B0DFu : 0u);
            t = (t ^ (t >> 30)) * 0x6C078965u + quint32(k + 0x18E);
            ++k;
            const int nid = kConvId[mtTemper(v) % 33u];
            if (has & (1u << nid)) continue;
            if (allow & (1u << nid)) {
                has |= (1u << nid);
                d1[j] = d1[j - 1];
                if (nid != 24 && nid != 25) d1[j] |= (1u << nid);
                ++j;
            }
        }
        finishRow(d1);
        return true;
    }

    // 计算同一轮 level 上 count 条 lane 的上场表; false = 前缀不够, 需要串行补算
    bool rows(int scene, int level, int count, quint32 out[K]) const
    {
        if (level <= 0) {
            for (int l = 0; l < count; ++l) out[l] = 11u;
            return true;
        }
        const quint32 allow = quint32(sceneAllowMask(scene));
        for (int l = 0; l < count; ++l) {
            quint32 d1[10];
            if (!oneRow(allow, l, d1)) return false;
            out[l] = (level >= 8) ? d1[9] : d1[level + 1];
        }
        return true;
    }

    // 计算 count 条 lane 的完整选卡表。按 rngSeed 去重时, 一张表要服务
    // 好几个 (种子, 轮), 所以这里把 d1 整张交出去, 不再只取其中一轮。
    bool tables(int scene, int count, quint32 (*out)[10]) const
    {
        const quint32 allow = quint32(sceneAllowMask(scene));
        for (int l = 0; l < count; ++l) {
            if (!oneRow(allow, l, out[l])) return false;
        }
        return true;
    }
};

// 串行补算: 直接给 rngSeed 算完整选卡表
bool rowTableSerial(quint32 rngSeed, int scene, quint32 d1[10])
{
    RowRng r;
    r.rngInit(rngSeed ? rngSeed : 0x1105u);
    SerialDraw draw;
    draw.rng = &r;
    return buildRow(scene, draw, d1);
}

} // namespace

bool rowTableBySeed(quint32 rngSeed, int scene, quint32 d1[10])
{
    return rowTableSerial(rngSeed, scene, d1);
}

void computeRow(quint32 pos, int scene, quint32 d1[10])
{
    quint32 rngSeed = pos * 101u;
    if (rngSeed == 0u) rngSeed = 0x1105u;
    t_rng.rngInit(rngSeed);
    SerialDraw draw;
    draw.rng = &t_rng;
    // buildRow 返回 false 表示 MT 前缀被抽空了 —— 正常参数下不会发生(前缀留了
    // 132 个词, 实测最多用 79 个), 但万一发生, d1[1..9] 是没写完的栈内容,
    // 直接拿去算 rowBits 会得到随机结果。退回"每轮只有普僵"这个确定的保守值。
    if (!buildRow(scene, draw, d1)) {
        for (int i = 0; i < 10; ++i) d1[i] = 1u;
    }
}

// ===========================================================================
//  文本
// ===========================================================================
QString maskToNames(quint32 mask)
{
    QStringList parts;
    for (int i = 0; i < Z_COUNT; ++i) {
        if (mask & (1u << i)) parts << zombieName(i);
    }
    if (parts.isEmpty()) return QString();
    return parts.join(QString::fromUtf8("、"));
}

QString maskToAbbr(quint32 mask)
{
    QStringList parts;
    for (int i = 0; i < Z_COUNT; ++i) {
        if (mask & (1u << i)) parts << zombieAbbr(i);
    }
    if (parts.isEmpty()) return QString();
    return parts.join(QString::fromUtf8(""));
}

QString flagRangeText(int beginFlag, int endFlag)
{
    // beginFlag / endFlag 是内部偶数边界, 对应的"F 数"是 beginFlag+1 .. endFlag
    const int firstFlag = beginFlag + 1;
    const int lastFlag = endFlag;
    if (lastFlag <= firstFlag) {
        return QString::fromUtf8("%1 F").arg(firstFlag);
    }
    return QString::fromUtf8("%1 到 %2 F").arg(firstFlag).arg(lastFlag);
}

QString WaveCondition::shortText() const
{
    // 表格里一行要放下最多 20 种僵尸, 所以列表统一用单字简称
    QStringList parts;
    if (needMask) parts << QString::fromUtf8("必出 ") + maskToAbbr(needMask);
    if (refuseMask) parts << QString::fromUtf8("禁出 ") + maskToAbbr(refuseMask);
    if (atLeastMask) {
        parts << QString::fromUtf8("至少%1(").arg(atLeast) + maskToAbbr(atLeastMask)
                     + QString::fromUtf8(")");
    }
    if (atMostMask) {
        parts << QString::fromUtf8("至多%1(").arg(atMost) + maskToAbbr(atMostMask)
                     + QString::fromUtf8(")");
    }
    if (parts.isEmpty()) return QString::fromUtf8("无条件");
    return parts.join(QString::fromUtf8(" "));
}

QString WaveCondition::summary() const
{
    QStringList parts;
    if (needMask) parts << QString::fromUtf8("必出 %1").arg(maskToNames(needMask));
    if (refuseMask) parts << QString::fromUtf8("禁出 %1").arg(maskToNames(refuseMask));
    if (atLeastMask) {
        parts << QString::fromUtf8("「%1」中至少 %2 种").arg(maskToNames(atLeastMask)).arg(atLeast);
    }
    if (atMostMask) {
        parts << QString::fromUtf8("「%1」中至多 %2 种").arg(maskToNames(atMostMask)).arg(atMost);
    }
    if (parts.isEmpty()) return QString::fromUtf8("无条件");
    return parts.join(QString::fromUtf8("；"));
}

// ===========================================================================
//  条件合法性检查
//
//  "非法" 指该条件在任何种子的对应轮上都不可能满足, 例如 "3~4 F 必出红眼"
//  (红眼最早第 6 轮 / F 11 才会出现), 或 "必出冰车" 但场景是白天前院。
//  只做纯逻辑判断, 不碰引擎状态, 对话框和主窗口都能直接调用。
// ===========================================================================
// 只跟条件本身有关、与区间无关的矛盾。条件编辑对话框里拿不到区间, 用这个。
QString checkConditionLocal(const WaveCondition &cond, const QString &kind)
{
    if (cond.isEmpty()) return QString();

    // 路障/读报是固定僵尸: 第 1 轮从这两者中抽一种, 累积表逐轮继承,
    // 所以每一轮都必然包含其中之一 —— 同时禁出就是死局
    if ((cond.refuseMask & (1u << Z_CONE)) && (cond.refuseMask & (1u << Z_NEWS))) {
        return QString::fromUtf8("%1 不能同时禁出「路障」和「读报」").arg(kind);
    }
    // 必出 与 禁出 交集非空
    if (cond.needMask & cond.refuseMask) {
        return QString::fromUtf8("%1 不能同时把「%2」设为必出和禁出")
            .arg(kind, maskToNames(cond.needMask & cond.refuseMask));
    }
    // 至多出的数量比"每轮必然出现"的僵尸还少。
    // 普僵在界面上选不到, 所以只有 路障+读报 这一对能撞上 (每一轮必出其一)。
    if (cond.atMostMask && cond.atMost < 1
        && (cond.atMostMask & (1u << Z_CONE)) && (cond.atMostMask & (1u << Z_NEWS))) {
        return QString::fromUtf8("%1 的「至多出」不允许 0 种：每一轮都会出"
                                 "路障或读报中的一种")
            .arg(kind);
    }
    return QString();
}

QString checkBandLocal(const RuleBand &b, int scene, const QString &kind)
{
    if (b.cond.isEmpty() || !b.valid()) return QString();

    const int firstRound = 1;         // 轮次从 1 开始
    const int firstGigaRound = 6;     // 红眼最早第 6 轮 (F 11 起)
    const int firstZoniRound = 3;     // 冰车/白眼最早第 3 轮 (第 3 轮交换规则)

    const QString where = QString::fromUtf8("%1「%2」")
                              .arg(kind)
                              .arg(flagRangeText(b.beginFlag, b.endFlag));

    // 0) 与区间无关的矛盾先查
    {
        const QString local = checkConditionLocal(b.cond, where);
        if (!local.isEmpty()) return local;
    }

    // 1) 必出的僵尸出现得比它最早的轮次还早
    if (b.cond.needMask) {
        const int earliestRound = std::max(firstRound, b.beginLevel() + 1);
        struct Gate { quint32 mask; int round; const char *label; };
        const Gate gates[] = {
            { (1u << Z_GIGA),                     firstGigaRound, "红眼" },
            { (1u << Z_ZOMBONI) | (1u << Z_GARG), firstZoniRound, "冰车/白眼" },
        };
        for (const Gate &g : gates) {
            if (earliestRound < g.round && (b.cond.needMask & g.mask)) {
                return QString::fromUtf8("%1 %4最早要到第 %2 轮才会出现")
                    .arg(where)
                    .arg(g.round)
                    .arg(QString::fromUtf8(g.label));
            }
        }
    }

    // 3) 必出 / 至少数量的要求超过了这一轮能出现的僵尸总数
    //    每轮构成 (用 1.2 原版 d_page 实测, 五场景一致):
    //      第 ℓ 轮固定 普僵 + 路障/读报之一, 之后是 ℓ+1 个格位, 其中末尾两个
    //      可能是空槽 (24/25, 占格但不加种类), 所以
    //        种类数下限 min(ℓ+1, 9), 上限 min(ℓ+3, 11)。
    //    扣掉必然出现的普僵与路障/读报, 非固定种类最多
    //      min(ℓ+3, 11) - 2 = min(ℓ+1, 9) 种。
    //    条件对这一段的每一轮都要成立, 而格位随轮次单调不减,
    //    所以卡住的是这一段最早的那一轮 —— 必须按它来算, 不能按最晚的。
    if (b.cond.needTotal > 0 || b.cond.atLeast > 0) {
        const int want = std::max(b.cond.needTotal, b.cond.atLeast);
        const int firstNeededRound = std::max(firstRound, b.beginLevel() + 1);
        int extraSlots = std::min(firstNeededRound, 9);
        // 场景总共能出的僵尸种类有限, 扣掉固定 2 种后是新增的天花板
        const int sceneKinds = popcnt(sceneZombieMask(scene) & ~1u
                                      & ~((1u << Z_CONE) | (1u << Z_NEWS)));
        extraSlots = std::min(extraSlots, sceneKinds);
        // 要求里的固定僵尸 (普僵; 路障/读报按至多 1 个算) 不占新增名额
        const quint32 reqMask = b.cond.needMask | b.cond.atLeastMask;
        int fixedInReq = 0;
        if (reqMask & (1u << Z_REGULAR)) ++fixedInReq;
        if (reqMask & ((1u << Z_CONE) | (1u << Z_NEWS))) ++fixedInReq;
        const int extraNeed = std::max(0, want - fixedInReq);
        if (extraNeed > extraSlots) {
            return QString::fromUtf8("%1 本轮必出的僵尸太多").arg(where);
        }
    }

    // 4) 禁出的种类太多, 这一段最晚那一轮需要的种类凑不出来。
    //    与上面一条方向相反: 每一轮的格位随轮次单调不减, 所以"候选不够"卡住的是
    //    这一段最晚的那一轮 —— 必须按它来算。
    //
    //    门槛必须取**能证明的**下限。取高了就会把"很难但确实有解"的条件误判成
    //    无解 (3~4 F 禁出 16 种那条就是这么被误拦的, 用 1.2 原版实测五场景都有解)。
    //    1.2 原版 d_page 逐轮统计出来的种类数下限是 min(ℓ+1, 9):
    //      ℓ=1 时 2 种, ℓ=2 时 3 种 …… 第 8 轮起固定 9 种。
    //    所以最晚那一轮禁出之后, 本场景还能出的种类数少于这个下限才是真无解。
    {
        const int lastLevel = std::max(1, std::min(b.endLevel() - 1, 9));
        const int minKinds = std::min(lastLevel + 1, 9);
        const quint32 avail = sceneZombieMask(scene) & ~b.cond.refuseMask
                              & ((1u << Z_COUNT) - 1u);
        if (popcnt(avail) < minKinds) {
            return QString::fromUtf8("%1 本轮禁出的僵尸太多").arg(where);
        }
    }

    // 至少出 > 至多出、必出∩禁出这两条与区间无关, 已在 checkConditionLocal 里查过
    return QString();
}

QString checkBand(const RuleBand &b, int scene, const QString &kind,
                  const QVector<RuleBand> &siblings)
{
    const QString local = checkBandLocal(b, scene, kind);
    if (!local.isEmpty()) return local;
    if (b.cond.isEmpty() || !b.valid()) return QString();

    const QString where = QString::fromUtf8("%1「%2」")
                              .arg(kind)
                              .arg(flagRangeText(b.beginFlag, b.endFlag));

    // 6) 种类级去重的推论: 同一条件段内, 禁出某种僵尸后它就不可能在这一段的
    //    任何一轮出现 —— 若同一僵尸在重叠的段被"必出", 必出条件就不可能满足。
    for (int j = 0; j < siblings.size(); ++j) {
        const RuleBand &o = siblings[j];
        if (o.cond.isEmpty() || !o.valid()) continue;
        const bool overlap = b.beginFlag < o.endFlag && o.beginFlag < b.endFlag;
        if (!overlap) continue;
        if (o.cond.needMask & b.cond.refuseMask) {
            return QString::fromUtf8("%1 禁出的「%2」在与它重叠的条件「%3」里被设为必出，"
                                     "同一轮不可能既出又不出")
                .arg(where)
                .arg(maskToNames(o.cond.needMask & b.cond.refuseMask))
                .arg(flagRangeText(o.beginFlag, o.endFlag));
        }
    }
    return QString();
}

// ===========================================================================
//  规则表
// ===========================================================================
void RuleTable::build(const QVector<RuleBand> &bands, int levelBegin, int levelEnd)
{
    m_begin = levelBegin;
    m_end = levelEnd;
    m_flat.clear();
    m_index.clear();
    m_count.clear();
    m_activeLevels = 0;

    const int n = std::max(0, m_end - m_begin);
    if (n <= 0) return;

    // 两遍扫描: 先统计每一轮被几条规则带覆盖, 再按前缀和把条件平铺进 m_flat。
    // 同一轮的多条条件必须"同时"满足 —— 以前这里是"同轮去重 (后定义者生效)",
    // 一条条件会把前面重叠的那条整条覆盖掉, 导致搜索和详情都按缺了条件的结果算。
    m_count.assign(size_t(n), 0);
    for (int i = 0; i < bands.size(); ++i) {
        const RuleBand &b = bands.at(i);
        if (!b.valid() || b.cond.isEmpty()) continue;
        const int lb = std::max(b.beginLevel(), levelBegin);
        const int le = std::min(b.endLevel(), levelEnd);
        for (int lv = lb; lv < le; ++lv) {
            const int k = lv - m_begin;
            if (k >= 0 && k < n) ++m_count[size_t(k)];
        }
    }

    m_index.assign(size_t(n), 0);
    int total = 0;
    for (int k = 0; k < n; ++k) {
        m_index[size_t(k)] = total;
        total += m_count[size_t(k)];
        if (m_count[size_t(k)] > 0) ++m_activeLevels;
    }
    m_flat.resize(size_t(total));

    // 第二遍: 按规则带顺序依次填入各自的轮 (起点靠前的先写, 顺序不影响"与")
    std::vector<int> write(size_t(n), 0);   // 每轮已写条数 (从 0 开始累加)
    for (int i = 0; i < bands.size(); ++i) {
        const RuleBand &b = bands.at(i);
        if (!b.valid() || b.cond.isEmpty()) continue;
        WaveCondition c = b.cond;
        c.sanitize();
        const int lb = std::max(b.beginLevel(), levelBegin);
        const int le = std::min(b.endLevel(), levelEnd);
        for (int lv = lb; lv < le; ++lv) {
            const int k = lv - m_begin;
            if (k < 0 || k >= n) continue;
            const int slot = m_index[size_t(k)] + write[size_t(k)];
            ++write[size_t(k)];
            m_flat[size_t(slot)] = c;
        }
    }
}

const WaveCondition *RuleTable::condsAt(int level, int *count) const
{
    if (count) *count = 0;
    const int k = level - m_begin;
    if (k < 0 || k >= int(m_count.size())) return nullptr;
    const int c = m_count[size_t(k)];
    if (c <= 0) return nullptr;
    if (count) *count = c;
    return &m_flat[size_t(m_index[size_t(k)])];
}

// ===========================================================================
//  详情 / 求值
// ===========================================================================
std::vector<DetailRow> viewDetail(quint32 seed, int uid, int mode, int scene,
                                  int beginLevel, int endLevel,
                                  const RuleTable &rank, const RuleTable &hard)
{
    std::vector<DetailRow> rows;
    const quint32 pos = seedToRVal(seed, uid + mode);
    rows.reserve(size_t(std::max(0, endLevel - beginLevel)));

    for (int lv = beginLevel; lv < endLevel; ++lv) {
        DetailRow row;
        row.level = lv;
        row.flag = lv * 2;
        quint32 d1[10];
        computeRow(pos + quint32(lv), scene, d1);
        row.bits = rowBits(d1, lv);

        int nc = 0;
        const WaveCondition *cs = rank.condsAt(lv, &nc);
        if (cs && nc > 0) {
            row.hasRule = true;
            row.matched = true;
            for (int i = 0; i < nc; ++i) {
                if (!cs[i].matches(row.bits)) { row.matched = false; break; }
            }
        }
        int nh = 0;
        const WaveCondition *hs = hard.condsAt(lv, &nh);
        if (hs && nh > 0) {
            row.hasHard = true;
            row.hardOk = true;
            for (int i = 0; i < nh; ++i) {
                if (!hs[i].matches(row.bits)) { row.hardOk = false; break; }
            }
        }
        rows.push_back(row);
    }
    return rows;
}

SeedEval evaluateSeed(quint32 seed, int uid, int mode, const WaveOracle &oracle,
                      const RuleTable &rank, const RuleTable &hard)
{
    SeedEval ev;
    ev.active = rank.activeLevels();
    ev.matched = 0;
    ev.hardOk = true;

    const quint32 pos = seedToRVal(seed, uid + mode);
    const int begin = std::min(rank.levelBegin(), hard.levelBegin());
    const int end = std::max(rank.levelEnd(), hard.levelEnd());

    for (int lv = begin; lv < end; ++lv) {
        int nc = 0, nh = 0;
        const WaveCondition *cs = rank.condsAt(lv, &nc);
        const WaveCondition *hs = hard.condsAt(lv, &nh);
        if (!cs && !hs) continue;

        quint32 d1[10];
        computeRow(pos + quint32(lv), oracle.scene(), d1);
        const quint32 bits = rowBits(d1, lv);

        if (cs) {
            bool ok = true;
            for (int i = 0; i < nc; ++i) {
                if (!cs[i].matches(bits)) { ok = false; break; }
            }
            if (ok) ++ev.matched;
        }
        if (hs) {
            bool ok = true;
            for (int i = 0; i < nh; ++i) {
                if (!hs[i].matches(bits)) { ok = false; break; }
            }
            if (!ok) {
                ev.hardOk = false;
                break;
            }
        }
    }
    return ev;
}

// ===========================================================================
//  结果收集
// ===========================================================================
void ResultCollector::init(int activeWaves, int outputSize)
{
    m_active = std::max(0, activeWaves);
    m_cap = std::max(1, outputSize);
    m_buckets.assign(size_t(m_active + 1), CountBucket());
    for (int i = 0; i <= m_active; ++i) m_buckets[size_t(i)].matched = i;
}

void ResultCollector::add(int matched, quint32 seed)
{
    if (matched < 0 || matched > m_active) return;
    CountBucket &b = m_buckets[size_t(matched)];
    ++b.total;
    if (b.examples.size() < m_cap) b.examples.push_back(seed);
}

QVector<CountBucket> ResultCollector::take() const
{
    QVector<CountBucket> out;
    for (int i = m_active; i >= 0; --i) {
        const CountBucket &b = m_buckets[size_t(i)];
        if (b.total == 0) continue;
        out.push_back(b);
    }
    return out;
}

void ResultCollector::merge(const ResultCollector &other)
{
    const int n = std::min(int(other.m_buckets.size()), m_active + 1);
    for (int i = 0; i < n; ++i) {
        const CountBucket &b = other.m_buckets[size_t(i)];
        m_buckets[size_t(i)].total += b.total;
        const int room = m_cap - m_buckets[size_t(i)].examples.size();
        if (room <= 0) continue;
        const int take = std::min(room, b.examples.size());
        for (int k = 0; k < take; ++k) m_buckets[size_t(i)].examples.push_back(b.examples[k]);
    }
}

quint64 ResultCollector::qualified(int minMatched) const
{
    quint64 n = 0;
    for (int i = std::max(0, minMatched); i <= m_active; ++i) n += m_buckets[size_t(i)].total;
    return n;
}

// ===========================================================================
//  检索
// ===========================================================================
SearchResult Searcher::run(const SearchSpec &spec,
                           const std::function<void(int, quint64, quint64)> &onProgress,
                           const std::atomic<bool> *cancel,
                           const std::atomic<bool> *pause)
{
    QElapsedTimer timer;
    timer.start();

    SearchResult res;
    res.waveBegin = spec.beginFlag / 2;
    res.waveEnd = spec.endFlag / 2;

    RuleTable rankTable;
    rankTable.build(spec.bands, res.waveBegin, res.waveEnd);
    RuleTable hardTable;
    hardTable.build(spec.hard, res.waveBegin, res.waveEnd);
    res.activeWaves = rankTable.activeLevels();

    if (res.waveEnd <= res.waveBegin) {
        res.error = QString::fromUtf8("F 数区间无效");
        return res;
    }
    if (rankTable.empty() && hardTable.empty()) {
        res.error = QString::fromUtf8("请先添加至少一条出怪条件");
        return res;
    }

    // ---- 每轮的条件按轮连续展开 ---------------------------------------
    // 同一轮可能被多条规则带覆盖 (条件取"与"), 所以存的是
    // "按轮连续展开的条件数组 + 每轮在数组里的偏移和条数"。
    // CPU 与 GPU 两条路径共用这份编译结果。
    const int levelBegin = res.waveBegin;
    const int levelEnd = res.waveEnd;
    const int nLevels = levelEnd - levelBegin;

    const size_t nSize = size_t(std::max(0, nLevels));
    std::vector<quint8> needLevel(nSize, 0);
    std::vector<int> rankOff(nSize, 0), rankCnt(nSize, 0);
    std::vector<int> hardOff(nSize, 0), hardCnt(nSize, 0);
    std::vector<WaveCondition> rankFlat, hardFlat;
    for (int i = 0; i < nLevels; ++i) {
        int rc = 0, hc = 0;
        const WaveCondition *rs = rankTable.condsAt(levelBegin + i, &rc);
        const WaveCondition *hs = hardTable.condsAt(levelBegin + i, &hc);
        if (rs && rc > 0) {
            rankOff[size_t(i)] = int(rankFlat.size());
            rankCnt[size_t(i)] = rc;
            rankFlat.insert(rankFlat.end(), rs, rs + rc);
        }
        if (hs && hc > 0) {
            hardOff[size_t(i)] = int(hardFlat.size());
            hardCnt[size_t(i)] = hc;
            hardFlat.insert(hardFlat.end(), hs, hs + hc);
        }
        needLevel[size_t(i)] = (rc > 0 || hc > 0) ? 1 : 0;
    }
    const quint32 off32 = quint32(spec.uid + spec.mode);
    const int scene = spec.scene;

    // CPU 与 GPU 两条路径共用的计数器 (GPU 快路径也要用, 所以写在分叉之前)
    std::atomic<quint64> scanned(0);
    std::atomic<bool> cancelHit(false);
    // 达标提前停止: 任一执行单元发现"全部满足"的总数达到 autoStop 就置位, 全体停下
    std::atomic<bool> goalHit(false);
    std::atomic<quint64> globalFull(0);
    // 进度用: GPU 路径只用 hits[0]; CPU 路径每个线程写自己那一格。
    // 轮询线程只读原子量, 不去碰工作线程正在改写的 ResultCollector (数据竞争)。
    std::unique_ptr<std::atomic<quint64>[]> hits(new std::atomic<quint64>[64]);
    for (int t = 0; t < 64; ++t) hits[size_t(t)].store(0, std::memory_order_relaxed);

    // 种子区间裁剪。
    // 扫描的是"连续种子"空间 (与参考实现 zombie_calculator 一致)。
    // 注意: 原版 worker.cpp 扫的是 r_val 空间 (cur 每步 +1), 相邻种子相差 101,
    //       等于只检查了区间内 1/101 的种子; 本引擎按种子逐个连续检查。
    quint64 span = spec.span;
    if (span == 0) span = 1;

    const quint64 begin = quint64(spec.seedBegin);
    if (begin >= SEED_SPACE) {
        res.error = QString::fromUtf8("起始种子超出范围");
        return res;
    }
    if (span > SEED_SPACE - begin) span = SEED_SPACE - begin;

    // ---- GPU 快路径 ---------------------------------------------------
    // 去重后的选卡表只有 "种子数 + 101*(跨度-1)" 张, 显存完全放得下, 所以
    // 一次扫一大块。分块只是为了控制显存占用, 并让"暂停 / 停止 / 找够了提前停"
    // 能及时生效 (粒度是一块)。
    GpuBackend *gpu = (spec.gpu == 0) ? nullptr : gpuBackend();
    // 小规模检索交给 CPU: GPU 每块都要建表 + 回传, 首次还要初始化 CUDA 上下文
    // (几十毫秒), 扫几千个种子时反而比 CPU 慢一个数量级。
    // 经验值: 2e5 个种子以下 CPU 更快, 以上 GPU 明显占优。
    if (gpu && nLevels > 0 && span >= 200000) {
        std::vector<GpuLevelDesc> glevels;
        std::vector<GpuCond> gconds;
        int lvFirst = 0;
        for (int i = 0; i < nLevels && lvFirst == 0; ++i) {
            if (!needLevel[size_t(i)]) continue;
            const int lv = levelBegin + i;
            if (lv > 0) lvFirst = lv;
        }
        // 只有 F 1~2 的条件时没有选卡表可算, 交给 CPU 更快。
        if (lvFirst > 0) {
            for (int i = 0; i < nLevels; ++i) {
                if (!needLevel[size_t(i)]) continue;
                const int lv = levelBegin + i;
                GpuLevelDesc g;
                g.level = lv;
                g.tableStride = (lv > 0) ? unsigned(101 * (lv - lvFirst)) : 0u;
                if (rankCnt[size_t(i)] > 0) {
                    g.rankOff = unsigned(gconds.size());
                    g.rankCnt = unsigned(rankCnt[size_t(i)]);
                    for (int c = 0; c < rankCnt[size_t(i)]; ++c) {
                        const WaveCondition &w = rankFlat[size_t(rankOff[size_t(i)] + c)];
                        GpuCond gc;
                        gc.needMask = w.needMask;
                        gc.needTotal = unsigned(w.needTotal);
                        gc.refuseMask = w.refuseMask;
                        gc.atLeastMask = w.atLeastMask;
                        gc.atLeast = unsigned(w.atLeast);
                        gc.atMostMask = w.atMostMask;
                        gc.atMost = unsigned(w.atMost);
                        gconds.push_back(gc);
                    }
                }
                if (hardCnt[size_t(i)] > 0) {
                    g.hardOff = unsigned(gconds.size());
                    g.hardCnt = unsigned(hardCnt[size_t(i)]);
                    for (int c = 0; c < hardCnt[size_t(i)]; ++c) {
                        const WaveCondition &w = hardFlat[size_t(hardOff[size_t(i)] + c)];
                        GpuCond gc;
                        gc.needMask = w.needMask;
                        gc.needTotal = unsigned(w.needTotal);
                        gc.refuseMask = w.refuseMask;
                        gc.atLeastMask = w.atLeastMask;
                        gc.atLeast = unsigned(w.atLeast);
                        gc.atMostMask = w.atMostMask;
                        gc.atMost = unsigned(w.atMost);
                        gconds.push_back(gc);
                    }
                }
                glevels.push_back(g);
            }

            std::string gpuErr;
            if (glevels.empty() || glevels.back().level <= 0 ||
                !gpu->prepare(glevels, gconds, quint32(sceneAllowMask(scene)), off32, lvFirst, &gpuErr)) {
                // 准备失败就当没有 GPU, 继续走 CPU
            } else {
                // 分块从 64K 起步, 成功一块就翻 4 倍, 封顶 4M 个种子。
                // 小步起步的原因: 很多查询扫不到十万个种子就"找够了"提前停止,
                // 这时固定 4M 的话, 光是建 128 MB 选卡表就白花好几毫秒。
                // 大步收尾则是为了避免大范围扫描时被 launch 开销拖慢。
                const quint64 kChunkMax = quint64(1) << 22;
                quint64 chunk = quint64(1) << 16;
                ResultCollector gcol;
                gcol.init(res.activeWaves, spec.outputSize);
                quint64 gDone = 0;
                bool gOk = true;

                while (gDone < span) {
                    while (pause && pause->load(std::memory_order_relaxed)
                           && !(cancel && cancel->load(std::memory_order_relaxed))
                           && !goalHit.load(std::memory_order_relaxed)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    if (cancel && cancel->load(std::memory_order_relaxed)) {
                        cancelHit.store(true, std::memory_order_relaxed);
                        break;
                    }
                    if (goalHit.load(std::memory_order_relaxed)) break;

                    const quint64 n = std::min<quint64>(span - gDone, chunk);
                    std::vector<unsigned short> outv;
                    if (!gpu->scan(unsigned(begin + gDone), unsigned(n), outv, &gpuErr) ||
                        outv.size() != size_t(n)) {
                        gOk = false;        // 中途失败: 丢掉已算的部分, 整个改用 CPU 重扫
                        break;
                    }

                    for (quint64 l = 0; l < n; ++l) {
                        const unsigned short m = outv[size_t(l)];
                        if (m == 0) continue;          // 被硬性条件刷掉
                        const int mt = int(m) - 1;
                        gcol.add(mt, quint32(begin + gDone + l));
                        if (spec.autoStop > 0 &&
                            (rankTable.empty() || mt == res.activeWaves)) {
                            if (globalFull.fetch_add(1, std::memory_order_relaxed) + 1 >= spec.autoStop) {
                                goalHit.store(true, std::memory_order_relaxed);
                                gDone += l + 1;        // 当前种子已计入, 也算进已检查数
                                break;
                            }
                        }
                    }
                    if (goalHit.load(std::memory_order_relaxed)) break;
                    gDone += n;
                    if (chunk < kChunkMax) {
                        chunk *= 4;
                        if (chunk > kChunkMax) chunk = kChunkMax;
                    }

                    scanned.store(gDone, std::memory_order_relaxed);
                    if (onProgress) {
                        const int pct = int((std::min(gDone, span) * 100) / span);
                        const quint64 q = gcol.qualified(0);
                        hits[0].store(q, std::memory_order_relaxed);
                        onProgress(pct, gDone, q);
                    }
                }

                if (gOk) {
                    res.buckets = gcol.take();
                    res.scanned = gDone;
                    if (res.scanned > span) res.scanned = span;
                    res.qualified = gcol.qualified(0);
                    res.stopped = cancelHit.load(std::memory_order_relaxed);
                    res.enoughFound = goalHit.load(std::memory_order_relaxed);
                    res.usedGpu = true;
                    res.seconds = double(timer.elapsed()) / 1000.0;
                    if (onProgress) onProgress(100, res.scanned, res.qualified);
                    return res;
                }
                // GPU 失败: 清场后落到 CPU 路径从头重扫
                goalHit.store(false, std::memory_order_relaxed);
                globalFull.store(0, std::memory_order_relaxed);
                scanned.store(0, std::memory_order_relaxed);
                cancelHit.store(false, std::memory_order_relaxed);
            }
        }
    }

    int threads = spec.threads > 0 ? spec.threads : int(std::thread::hardware_concurrency());
    if (threads <= 0) threads = 1;
    if (quint64(threads) > span) threads = int(span);
    // 上限保护: threads 是从界面/命令行传进来的, 一旦被填成一个很大的数,
    // 光是建线程和给每个线程分配 ResultCollector 就会把内存吃光。
    // 实际并行度超过 64 已经没有收益(这里扫描量再大也是内存带宽受限)。
    if (threads > 64) threads = 64;

    std::vector<ResultCollector> cols;
    cols.resize(static_cast<size_t>(threads));
    for (int t = 0; t < threads; ++t) cols[size_t(t)].init(res.activeWaves, spec.outputSize);

    std::atomic<int> doneThreads(0);

    // 每个线程处理连续的一段
    const quint64 per = span / quint64(threads);
    const quint64 rem = span % quint64(threads);

    std::vector<std::thread> pool;
    pool.reserve(size_t(threads));

    for (int t = 0; t < threads; ++t) {
        const quint64 myCount = per + (quint64(t) < rem ? 1 : 0);
        const quint64 myBegin = begin + quint64(t) * per + std::min<quint64>(quint64(t), rem);
        pool.emplace_back([&, t, myBegin, myCount]() {
            ResultCollector &col = cols[size_t(t)];
            std::atomic<quint64> &myHitsSlot = hits[size_t(t)];

            // ---- 这一批真正要算的轮 (循环不变量) ----------------------------
            // lv <= 0 的轮 (F 1~2) 位恒为 11u, 查表没意义, 单独一次算掉;
            // 其余按 lv 升序登记, 用 R 差值当表内偏移。
            std::vector<int> statIdx;            // lv <= 0 的轮在 level 数组里的下标
            std::vector<int> advLv, advIdx, advStride;
            advLv.reserve(size_t(nLevels));
            advIdx.reserve(size_t(nLevels));
            for (int i = 0; i < nLevels; ++i) {
                if (!needLevel[size_t(i)]) continue;
                const int lv = levelBegin + i;
                if (lv <= 0) { statIdx.push_back(i); continue; }
                advLv.push_back(lv);
                advIdx.push_back(i);
            }
            const int aStatic = int(statIdx.size());
            const int aCount = int(advLv.size());
            const int lvFirst = aCount > 0 ? advLv[0] : 0;
            for (int k = 0; k < aCount; ++k) advStride.push_back(101 * (advLv[size_t(k)] - lvFirst));
            // 表缓存要存的张数是 "101*(轮跨度-1) + 块大小"。跨度太大 (比如区间划到
            // 几千 F, 只查头尾两轮) 时这些表根本换不回收益, 就退回"逐轮各算各的"。
            const int lastStride = aCount > 0 ? advStride[size_t(aCount - 1)] : 0;
            const bool dedup = (lastStride + kLanes <= kMaxTables);
            const int blockCap = dedup ? kBlock : kLanes;

            // 每轮的条件指针/条数也一次取好, 免得内层反复算偏移
            std::vector<const WaveCondition *> advRank(size_t(aCount), nullptr);
            std::vector<const WaveCondition *> advHard(size_t(aCount), nullptr);
            std::vector<int> advRankN(size_t(aCount), 0), advHardN(size_t(aCount), 0);
            for (int k = 0; k < aCount; ++k) {
                const size_t i = size_t(advIdx[size_t(k)]);
                advRankN[size_t(k)] = rankCnt[i];
                advHardN[size_t(k)] = hardCnt[i];
                if (rankCnt[i] > 0) advRank[size_t(k)] = &rankFlat[size_t(rankOff[i])];
                if (hardCnt[i] > 0) advHard[size_t(k)] = &hardFlat[size_t(hardOff[i])];
            }
            std::vector<const WaveCondition *> statRank(size_t(aStatic), nullptr);
            std::vector<const WaveCondition *> statHard(size_t(aStatic), nullptr);
            std::vector<int> statRankN(size_t(aStatic), 0), statHardN(size_t(aStatic), 0);
            for (int k = 0; k < aStatic; ++k) {
                const size_t i = size_t(statIdx[size_t(k)]);
                statRankN[size_t(k)] = rankCnt[i];
                statHardN[size_t(k)] = hardCnt[i];
                if (rankCnt[i] > 0) statRank[size_t(k)] = &rankFlat[size_t(rankOff[i])];
                if (hardCnt[i] > 0) statHard[size_t(k)] = &hardFlat[size_t(hardOff[i])];
            }

            LaneBatch<kLanes> batch;
            quint32 rs[kLanes];
            quint32 tabRows[kLanes][10];
            std::vector<quint32> tab;                 // 一整块的选卡表, 每张 10 个词
            size_t cachedTables = 0;                  // tab 里目前有效的表数
            int prevBlock = 0;                        // 上一块实际处理了几个种子
            std::vector<quint8> alive(size_t(kBlock), 0);
            std::vector<int> matched(size_t(kBlock), 0);
            // 只在真的走表缓存时才预留。!dedup 的查询 (轮跨度大到几千 F, 只查
            // 头尾两轮) 根本不碰 tab, 但 advStride 的末项可能上百万, 无条件
            // reserve 的话每个线程都要白占几十 MB (线程一多就是几百 MB)。
            if (dedup && aCount > 0) {
                tab.reserve(size_t(advStride[size_t(aCount - 1)] + blockCap) * 10);
            }

            quint64 done = 0;
            quint64 reported = 0;        // 已上报到 scanned 的数量
            quint64 myHits = 0;

            while (done < myCount) {
                // 暂停: 所有线程原地等待。cancel 优先级更高 —— 暂停中按"停止"
                // 也要能立刻退出, 所以等待条件里带上 cancel 的检查。
                while (pause && pause->load(std::memory_order_relaxed)
                       && !(cancel && cancel->load(std::memory_order_relaxed))
                       && !goalHit.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (cancel && cancel->load(std::memory_order_relaxed)) {
                    cancelHit.store(true, std::memory_order_relaxed);
                    break;
                }
                if (goalHit.load(std::memory_order_relaxed)) break;

                // 参与排序的种子数与本块起点
                const int block = int(std::min<quint64>(myCount - done, quint64(blockCap)));
                const quint64 seedBase = myBegin + done;

                for (int l = 0; l < block; ++l) {
                    alive[size_t(l)] = 1;
                    matched[size_t(l)] = 0;
                }
                int aliveCount = block;

                // ---- 一次算出一整块的选卡表, 按 R 去重 --------------------
                // 表里的第 j 张对应 R = (seedBase + off32 + 101*lvFirst) + j。
                // 上一块已经算过 [block, 旧表长) 这一段 (就是 101*(lvSpan-1) 个
                // 跨块的 R), 搬过来接着用, 只补算新露出来的 block 张。
                if (aCount > 0 && dedup) {
                    const int need = advStride[size_t(aCount - 1)] + block;   // 本块要用的表数
                    // 位移量是"上一块"的大小, 不是本块 —— 只有最后一块会比 kBlock 小,
                    // 用本块大小去搬就会错位。
                    int keep = int(cachedTables) - prevBlock;
                    if (keep < 0) keep = 0;
                    if (keep > need) keep = need;
                    if (keep > 0) {
                        std::memmove(tab.data(), tab.data() + size_t(prevBlock) * 10,
                                     size_t(keep) * 10 * sizeof(quint32));
                    }
                    tab.resize(size_t(need) * 10);
                    cachedTables = size_t(need);

                    for (int base = keep; base < need; base += kLanes) {
                        const int n = std::min(kLanes, need - base);
                        for (int l = 0; l < n; ++l) {
                            const quint64 s = quint64(quint32(seedBase + quint64(base + l)));
                            rs[l] = quint32(s + quint64(off32) + quint64(101u) * quint64(lvFirst));
                        }
                        batch.init(rs, n);
                        if (batch.tables(scene, n, tabRows)) {
                            for (int l = 0; l < n; ++l) {
                                for (int j = 0; j < 10; ++j) tab[size_t(base + l) * 10 + j] = tabRows[l][j];
                            }
                        } else {
                            for (int l = 0; l < n; ++l) {
                                quint32 t[10];
                                if (!rowTableSerial(rs[l], scene, t))
                                    for (int j = 0; j < 10; ++j) t[j] = 1u;
                                for (int j = 0; j < 10; ++j) tab[size_t(base + l) * 10 + j] = t[j];
                            }
                        }
                    }
                }

                // lv <= 0 的轮 (F 1~2): 位恒为 11u, 与种子无关, 每轮判一次就够
                for (int k = 0; k < aStatic && aliveCount > 0; ++k) {
                    const WaveCondition *rankC = statRank[size_t(k)];
                    const WaveCondition *hardC = statHard[size_t(k)];
                    if (rankC) {
                        bool ok = true;
                        for (int c = 0; c < statRankN[size_t(k)] && ok; ++c) {
                            if (!rankC[c].matches(11u)) ok = false;
                        }
                        if (ok) {
                            for (int l = 0; l < block; ++l) ++matched[size_t(l)];
                        }
                    }
                    if (hardC) {
                        bool ok = true;
                        for (int c = 0; c < statHardN[size_t(k)] && ok; ++c) {
                            if (!hardC[c].matches(11u)) ok = false;
                        }
                        if (!ok) {
                            // 这几轮的位与种子无关, 一死全死。必须把 alive[] 也
                            // 清掉 —— 只把 aliveCount 置 0 的话, 后面按 alive[]
                            // 逐个计数时仍然会把整块种子都算成"合格"。
                            for (int l = 0; l < block; ++l) alive[size_t(l)] = 0;
                            aliveCount = 0;
                        }
                    }
                }

                // 轮跨度太大时不做表缓存: 每条 lane 每轮单独算 (块已缩到 kLanes)
                if (aCount > 0 && !dedup) {
                    quint32 outs[kLanes];
                    int idxs[kLanes];
                    for (int k = 0; k < aCount && aliveCount > 0; ++k) {
                        const int lv = advLv[size_t(k)];
                        const WaveCondition *rankC = advRank[size_t(k)];
                        const int rankN = advRankN[size_t(k)];
                        const WaveCondition *hardC = advHard[size_t(k)];
                        const int hardN = advHardN[size_t(k)];

                        int cnt = 0;
                        for (int l = 0; l < block; ++l) {
                            if (!alive[size_t(l)]) continue;
                            idxs[cnt] = l;
                            const quint64 s = quint64(quint32(seedBase + quint64(l)));
                            rs[cnt] = quint32(s + quint64(off32) + quint64(101u) * quint64(lv));
                            ++cnt;
                        }
                        if (cnt == 0) break;
                        batch.init(rs, cnt);
                        if (!batch.rows(scene, lv, cnt, outs)) {
                            for (int i = 0; i < cnt; ++i) {
                                quint32 t[10];
                                if (!rowTableSerial(rs[i], scene, t))
                                    for (int j = 0; j < 10; ++j) t[j] = 1u;
                                outs[i] = (lv >= 8) ? t[9] : t[lv + 1];
                            }
                        }
                        for (int i = 0; i < cnt; ++i) {
                            const int l = idxs[i];
                            const quint32 bits = outs[i];
                            if (rankC) {
                                bool ok = true;
                                for (int c = 0; c < rankN && ok; ++c) {
                                    if (!rankC[c].matches(bits)) ok = false;
                                }
                                if (ok) ++matched[size_t(l)];
                            }
                            if (hardC) {
                                bool ok = true;
                                for (int c = 0; c < hardN && ok; ++c) {
                                    if (!hardC[c].matches(bits)) ok = false;
                                }
                                if (!ok) {
                                    alive[size_t(l)] = 0;
                                    --aliveCount;
                                }
                            }
                        }
                    }
                }

                for (int k = 0; k < aCount && dedup && aliveCount > 0; ++k) {
                    const int lv = advLv[size_t(k)];
                    const int stride = advStride[size_t(k)];
                    const WaveCondition *rankC = advRank[size_t(k)];
                    const int rankN = advRankN[size_t(k)];
                    const WaveCondition *hardC = advHard[size_t(k)];
                    const int hardN = advHardN[size_t(k)];

                    // 同一轮里各条 lane 的位就在 tab[stride + l] 上
                    const int col = (lv >= 8) ? 9 : lv + 1;
                    for (int l = 0; l < block; ++l) {
                        if (!alive[size_t(l)]) continue;
                        const quint32 bits = tab[size_t(stride + l) * 10 + col];
                        if (rankC) {
                            bool ok = true;
                            for (int c = 0; c < rankN && ok; ++c) {
                                if (!rankC[c].matches(bits)) ok = false;
                            }
                            if (ok) ++matched[size_t(l)];
                        }
                        if (hardC) {
                            bool ok = true;
                            for (int c = 0; c < hardN && ok; ++c) {
                                if (!hardC[c].matches(bits)) ok = false;
                            }
                            if (!ok) {
                                alive[size_t(l)] = 0;
                                --aliveCount;
                            }
                        }
                    }
                }

                // 逐个种子计数: 达到设定数量就停在那个种子上, 与串行版行为一致
                int consumed = block;
                for (int l = 0; l < block; ++l) {
                    if (!alive[size_t(l)]) continue;
                    col.add(matched[size_t(l)], quint32(seedBase + quint64(l)));
                    ++myHits;
                    if (spec.autoStop > 0 &&
                        (rankTable.empty() || matched[size_t(l)] == res.activeWaves)) {
                        if (globalFull.fetch_add(1, std::memory_order_relaxed) + 1 >= spec.autoStop) {
                            goalHit.store(true, std::memory_order_relaxed);
                            consumed = l + 1;   // 当前种子已计入, 也要算进已检查数
                            break;
                        }
                    }
                }
                myHitsSlot.store(myHits, std::memory_order_relaxed);
                prevBlock = block;

                done += quint64(consumed);
                if (done - reported >= 4096) {
                    scanned.fetch_add(done - reported, std::memory_order_relaxed);
                    reported = done;
                }
                if (goalHit.load(std::memory_order_relaxed)) break;
            }
            scanned.fetch_add(done - reported, std::memory_order_relaxed);
            myHitsSlot.store(myHits, std::memory_order_relaxed);
            doneThreads.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // 主线程做进度上报
    while (doneThreads.load(std::memory_order_relaxed) < threads) {
        if (cancel && cancel->load(std::memory_order_relaxed)) break;
        // 暂停中不刷新进度: 百分比停在上一次的值, 让界面显示"已暂停"
        const bool paused = pause && pause->load(std::memory_order_relaxed);
        if (!paused) {
            const quint64 s = scanned.load(std::memory_order_relaxed);
            quint64 q = 0;
            for (int t = 0; t < threads; ++t) q += hits[size_t(t)].load(std::memory_order_relaxed);
            if (onProgress) {
                const int pct = int((std::min(s, span) * 100) / span);
                onProgress(pct, s, q);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    for (size_t i = 0; i < pool.size(); ++i) {
        if (pool[i].joinable()) pool[i].join();
    }

    // 合并
    ResultCollector merged;
    merged.init(res.activeWaves, spec.outputSize);
    for (int t = 0; t < threads; ++t) merged.merge(cols[size_t(t)]);
    res.buckets = merged.take();
    res.scanned = scanned.load(std::memory_order_relaxed);
    if (res.scanned > span) res.scanned = span;
    res.qualified = merged.qualified(0);
    res.stopped = cancelHit.load(std::memory_order_relaxed);
    res.enoughFound = goalHit.load(std::memory_order_relaxed);
    res.seconds = double(timer.elapsed()) / 1000.0;
    if (onProgress) onProgress(100, res.scanned, res.qualified);
    return res;
}

// ===========================================================================
//  交叉校验: 用原版分页实现对照
// ===========================================================================
QString Searcher::selfCheck()
{
    // --- 原版实现 (worker.cpp 的 d_page), 作为黄金标准 ---
    struct RefPage
    {
        int scene;
        quint32 buf[0x400];
        int idx;
        quint32 data[1024][8];

        void rngInit(quint32 seed)
        {
            buf[0] = seed;
            for (int i = 1; i < 0x18E; ++i) {
                buf[i] = (buf[i - 1] ^ (buf[i - 1] >> 30)) * 0x6C078965u + quint32(i);
            }
            idx = 0;
        }
        quint32 rngGen(int max)
        {
            quint32 tmp = (buf[idx] & 0x80000000u) | (buf[idx + 1] & 0x7FFFFFFFu);
            buf[idx] = (tmp >> 1) ^ buf[(idx + 0x18D) % 0x270];
            if (tmp & 1u) buf[idx] ^= 0x9908B0DFu;
            buf[idx + 0x18D + 1] =
                (buf[idx + 0x18D] ^ (buf[idx + 0x18D] >> 30)) * 0x6C078965u + quint32(idx + 0x18D + 1);
            tmp = buf[idx];
            tmp ^= (tmp >> 11);
            tmp ^= ((tmp & 0xFF3A58ADu) << 7);
            tmp ^= ((tmp & 0xFFFFDF8Cu) << 15);
            tmp ^= (tmp >> 18);
            ++idx;
            return (tmp & 0x7FFFFFFFu) % quint32(max);
        }
        void init(int sc, quint32 pageIndex)
        {
            scene = sc;
            const quint32 allow = quint32(sceneAllowMask(sc));
            quint32 curSeed = pageIndex * 1024u;
            for (int i = 0; i < 1024; ++i) {
                quint32 rngSeed = curSeed * 101u;
                if (rngSeed == 0u) rngSeed = 0x1105u;
                rngInit(rngSeed);
                ++curSeed;

                quint32 has = 1u;
                quint32 d1[10];
                d1[0] = 1u;
                if (rngGen(5) == 0) {
                    d1[0] |= (1u << Z_NEWS);
                    has |= (1u << Z_NEWS);
                } else {
                    d1[0] |= (1u << Z_CONE);
                    has |= (1u << Z_CONE);
                }
                int j = 1;
                while (j < 10) {
                    const int type = int(rngGen(33));
                    const int nid = kConvId[type];
                    if (has & (1u << nid)) continue;
                    if (allow & (1u << nid)) {
                        has |= (1u << nid);
                        d1[j] = d1[j - 1];
                        if (nid != 24 && nid != 25) d1[j] |= (1u << nid);
                        ++j;
                    }
                }
                for (j = 2; j < 6; ++j) {
                    if (d1[j] & (1u << Z_GIGA)) d1[j] = d1[j + 1] ^ (1u << Z_GIGA);
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
                for (j = 0; j < 8; ++j) data[i][j] = d1[j + 2];
            }
        }
        quint32 get(quint32 pos, int level)
        {
            if (level == 0) return 11u;
            --level;
            if (level >= 8) level = 7;
            return data[pos & 1023u][level];
        }
    };

    // 逐位对比 d1[2..9] (等价于轮 1..8 的位掩码)
    const quint32 base = 0x1000u;      // 第 4 个页 (pageIndex = 4)
    quint32 counter = 0;

    for (int scene = 0; scene < 8; ++scene) {
        if (scene == 3 || scene == 5 || scene == 7) continue;   // 与原版一致的 5 个可见场景
        RefPage ref;
        ref.init(scene, base / 1024u);

        for (quint32 i = 0; i < 64; ++i) {
            const quint32 pos = base + i;
            quint32 d1[10];
            computeRow(pos, scene, d1);
            for (int lv = 1; lv <= 8; ++lv) {
                const quint32 a = rowBits(d1, lv);
                const quint32 b = ref.get(pos, lv);
                if (a != b) {
                    return QString::fromUtf8("校验失败: scene=%1 pos=0x%2 level=%3 本引擎=0x%4 原版=0x%5")
                        .arg(scene)
                        .arg(pos, 8, 16, QLatin1Char('0'))
                        .arg(lv)
                        .arg(a, 0, 16)
                        .arg(b, 0, 16);
                }
                ++counter;
            }
        }
    }
    if (counter == 0) return QString::fromUtf8("校验未执行");
    return QString();
}

} // namespace sf
