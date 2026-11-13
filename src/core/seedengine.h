#pragma once

// ============================================================================
//  种子查找核心引擎 (v3)
//
//  RNG 复刻与原版 worker.cpp 逐位等价 (见 Searcher::selfCheck 交叉校验),
//  但检索条件从"一组全局条件"扩展为:
//    · 多条规则带 RuleBand —— 每条覆盖任意 F 区间, 各自携带独立的
//      必出 / 禁出 / 至少出 / 至多出 四组条件
//    · 硬性规则带 hard —— 同一套条件语义, 但用作"段内每轮都必须满足"的
//      淘汰条件 (1.2 里的"全局条件"在 3.0 统一成硬性规则带)
//
//  与原版的关键差别: 原版为每个 (种子, 轮) 初始化一整页 1024 个种子的表,
//  实际只用其中 1 项。本引擎直接对单个 pos 复刻 RNG, 结果完全一致,
//  速度提升约三个数量级。
// ============================================================================

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <vector>

#include "zombiedata.h"

namespace sf {

const qint64 INV = 2083697005LL;            // 101 在 2^32 上的逆元
const quint64 SEED_SPACE = 0x100000000ULL;  // 种子空间大小 (0 ~ 0xFFFFFFFF)

inline int popcnt(quint32 x)
{
#if defined(_MSC_VER)
    return int(__popcnt(static_cast<unsigned>(x)));
#else
    return __builtin_popcount(static_cast<unsigned>(x));
#endif
}

// 种子值与内部 pos 的互换:  pos = (offset + seed) * INV  (mod 2^32)
inline quint32 seedToRVal(quint64 seed, int offset)
{
    return quint32((quint64(quint32(offset + qint64(seed))) * quint64(INV)) & 0xFFFFFFFFULL);
}
inline quint32 rValToSeed(quint32 rVal, int offset)
{
    return quint32((quint64(rVal) * 101ULL - quint64(quint32(offset))) & 0xFFFFFFFFULL);
}

// ---------------------------------------------------------------------------
//  单轮条件
// ---------------------------------------------------------------------------
struct WaveCondition
{
    quint32 needMask = 0;       // 必出
    quint32 refuseMask = 0;     // 禁出
    quint32 atLeastMask = 0;    // 至少出
    quint32 atMostMask = 0;     // 至多出
    int atLeast = 0;
    int atMost = 0;
    int needTotal = 0;

    bool isEmpty() const
    {
        return needMask == 0 && refuseMask == 0 && atLeastMask == 0 && atMostMask == 0;
    }
    void sanitize()
    {
        needTotal = popcnt(needMask);
        if (atLeastMask == 0) {
            atLeast = 0;
        } else {
            if (atLeast < 0) atLeast = 0;
            if (atLeast > popcnt(atLeastMask)) atLeast = popcnt(atLeastMask);
        }
        if (atMostMask == 0) {
            atMost = 0;
        } else {
            if (atMost < 0) atMost = 0;
            if (atMost > popcnt(atMostMask)) atMost = popcnt(atMostMask);
        }
    }
    inline bool matches(quint32 bits) const
    {
        if (popcnt(bits & needMask) != needTotal) return false;
        if (bits & refuseMask) return false;
        if (atLeastMask && popcnt(bits & atLeastMask) < atLeast) return false;
        if (atMostMask && popcnt(bits & atMostMask) > atMost) return false;
        return true;
    }
    bool operator==(const WaveCondition &o) const
    {
        return needMask == o.needMask && refuseMask == o.refuseMask
               && atLeastMask == o.atLeastMask && atMostMask == o.atMostMask
               && atLeast == o.atLeast && atMost == o.atMost;
    }
    bool operator!=(const WaveCondition &o) const { return !(*this == o); }

    QString summary() const;        // 完整描述
    QString shortText() const;      // 列表用简述
};

// ---------------------------------------------------------------------------
//  规则带: 覆盖 [beginFlag, endFlag)
//
//  注意 "轮" 与 "F 数" 的换算 (与出怪计算器一致):
//    轮次从 1 开始编号, 起始轮必须为奇数, 终止轮必须为偶数;
//    第 r 轮对应的内部 level = (r - 1) / 2, 因此
//        beginFlag(偶数) = 起始轮 - 1
//        endFlag(偶数)   = 终止轮
//    一条 [起始轮 11, 终止轮 12] 的规则 = beginFlag 10 / endFlag 12 = 只覆盖 level 5.
// ---------------------------------------------------------------------------
struct RuleBand
{
    int beginFlag = 0;
    int endFlag = 0;
    WaveCondition cond;

    int beginLevel() const { return beginFlag / 2; }
    int endLevel() const { return endFlag / 2; }
    bool valid() const { return endFlag > beginFlag && beginFlag >= 0; }
};

QString maskToNames(quint32 mask);
QString maskToAbbr(quint32 mask);
QString flagRangeText(int beginFlag, int endFlag);

// ---------------------------------------------------------------------------
// 条件合法性检查
//
// 返回空串表示这条条件一定能被某些种子满足; 否则返回可直接展示给用户的中文说明。
// 放在引擎层是为了让"编辑条件点确定"和"点开始查找"共用同一套判断, 不会出现
// 对话框放行、开查又被拦下的不一致。
//
//   kind     —— 提示语里怎么称呼这张表, 例如 "出怪条件" / "全局条件"
//   siblings —— 同一张表里的其他条件, 用于判断跨段矛盾 (可传空表跳过该检查)
// ---------------------------------------------------------------------------
QString checkConditionLocal(const WaveCondition &cond, const QString &kind);
QString checkBandLocal(const RuleBand &b, int scene, const QString &kind);
QString checkBand(const RuleBand &b, int scene, const QString &kind,
                  const QVector<RuleBand> &siblings);

// ---------------------------------------------------------------------------
//  单轮出怪计算 (纯函数)
// ---------------------------------------------------------------------------

// 为一个 pos 生成完整的 10 项累积选卡表 —— 这是最贵的一步, 一次算出供
// 全部 25 轮复用, 与游戏 / 原版 worker.cpp 逐位一致。
void computeRow(quint32 pos, int scene, quint32 d1[10]);

// 直接按 rngSeed 生成完整选卡表。GPU 对拍程序也用它当黄金标准。
bool rowTableBySeed(quint32 rngSeed, int scene, quint32 d1[10]);

// 从选卡表取第 level 轮的出怪位掩码 (level 0 固定 11, >=8 夹到 8)
inline quint32 rowBits(const quint32 d1[10], int level)
{
    if (level <= 0) return 11u;
    if (level >= 8) return d1[9];
    return d1[level + 1];
}

class WaveOracle
{
public:
    explicit WaveOracle(int scene = 2) : m_scene(scene) {}
    int scene() const { return m_scene; }

    // pos 对应游戏内部 r_val
    inline quint32 bits(quint32 pos, int level) const
    {
        if (level <= 0) return 11u;
        quint32 d1[10];
        computeRow(pos, m_scene, d1);
        return rowBits(d1, level);
    }
    // 便捷入口: 直接按种子 + 轮号取
    // 第 level 轮的 rngSeed 是 seed + offset + 101 * level, 所以 pos 要加上 level:
    // rVal(seed + 101*level) = rVal(seed) + level (因为 INV * 101 ≡ 1)。
    // 少加这一项的话, 每一轮取到的都是第 0 轮那张表里的位。
    inline quint32 bitsOfSeed(quint32 seed, int offset, int level) const
    {
        if (level <= 0) return 11u;
        return bits(quint32(seedToRVal(seed, offset) + quint32(level)), level);
    }

private:
    int m_scene;
};

// ---------------------------------------------------------------------------
//  规则表: 规则带 → 按轮号索引
// ---------------------------------------------------------------------------
class RuleTable
{
public:
    void build(const QVector<RuleBand> &bands, int levelBegin, int levelEnd);

    int levelBegin() const { return m_begin; }
    int levelEnd() const { return m_end; }
    int activeLevels() const { return m_activeLevels; }
    bool empty() const { return m_activeLevels == 0; }

    // 取某一轮的全部条件。同一轮可能被多条规则带覆盖, 这些条件必须"同时"
    // 满足 (以前每轮只留最后一条, 前面的会被整条丢掉, 是查得不对的根源)。
    // 返回指向 m_flat 中连续 count 个条件的指针; 没有条件时返回 nullptr 且 *count = 0。
    const WaveCondition *condsAt(int level, int *count) const;

private:
    int m_begin = 0;
    int m_end = 0;
    int m_activeLevels = 0;
    std::vector<WaveCondition> m_flat;   // 按轮号连续存放
    std::vector<int> m_index;            // 轮号偏移 → m_flat 起始下标
    std::vector<int> m_count;            // 轮号偏移 → 条件条数
};

// ---------------------------------------------------------------------------
//  单种子逐轮详情
// ---------------------------------------------------------------------------
struct DetailRow
{
    int level = 0;
    int flag = 0;
    quint32 bits = 0;
    bool hasRule = false;       // 这一轮是否落在某条"出怪条件"里
    bool matched = false;       // 该出怪条件是否满足
    bool hasHard = false;       // 这一轮是否落在某条"全局条件"里
    bool hardOk = true;         // 该全局条件是否满足
};

std::vector<DetailRow> viewDetail(quint32 seed, int uid, int mode, int scene,
                                  int beginLevel, int endLevel,
                                  const RuleTable &rank, const RuleTable &hard);

struct SeedEval
{
    int matched = 0;            // 满足的"轮"数(出怪条件), 用于排名
    int active = 0;             // 出怪条件覆盖的总轮数
    bool hardOk = true;         // 全局条件(硬性)是否全部满足
};

SeedEval evaluateSeed(quint32 seed, int uid, int mode, const WaveOracle &oracle,
                      const RuleTable &rank, const RuleTable &hard);

// ===========================================================================
//  检索
// ===========================================================================
struct SearchSpec
{
    int uid = 1;
    int mode = 13;
    int scene = 2;

    int beginFlag = 0;              // 偶数 (= 起始轮 - 1)
    int endFlag = 50;               // 偶数 (= 终止轮)

    quint32 seedBegin = 0;          // 种子区间起点 (含)
    quint64 span = 1000000;         // 扫描多少个连续种子
    int outputSize = 20;            // 每种匹配轮数最多保留多少个示例种子
    int threads = 0;                // 0 = 自动
    quint64 autoStop = 0;           // 非 0: 找满这么多个"全部满足"的种子就提前停
    int gpu = 1;                    // 0 = 只用 CPU; 默认优先 GPU, 设备/初始化不可用时自动退回 CPU

    // 出怪条件: 只用于排名 (满足的轮数越多越靠前)
    QVector<RuleBand> bands;
    // 全局条件: 硬性要求, 段内每一轮都必须全部满足, 不满足的种子直接丢弃
    QVector<RuleBand> hard;
};

// 按"匹配轮数"分桶的结果
struct CountBucket
{
    int matched = 0;
    quint64 total = 0;              // 该匹配轮数的种子总数
    QVector<quint32> examples;      // 示例种子 (不超过 outputSize)
};

struct SearchResult
{
    QVector<CountBucket> buckets;   // 按 matched 降序, 只含满足全部全局条件的种子
    quint64 scanned = 0;
    quint64 qualified = 0;          // 满足全部全局条件的种子总数
    int activeWaves = 0;
    int waveBegin = 0;
    int waveEnd = 0;
    bool stopped = false;
    bool enoughFound = false;       // 因达到 autoStop 目标而提前结束
    bool usedGpu = false;           // 这一轮真的由 GPU 算完的 (界面据此提示)
    double seconds = 0.0;
    QString error;

    quint64 fullCount() const
    {
        for (int i = 0; i < buckets.size(); ++i)
            if (buckets[i].matched == activeWaves) return buckets[i].total;
        return 0;
    }
};

// 线程内共享的累加器
class ResultCollector
{
public:
    void init(int activeWaves, int outputSize);
    void add(int matched, quint32 seed);
    void merge(const ResultCollector &other);       // 合并另一个线程的结果 (保留总数)
    QVector<CountBucket> take() const;
    quint64 qualified(int minMatched) const;

private:
    int m_active = 0;
    int m_cap = 20;
    std::vector<CountBucket> m_buckets;     // 下标 = matched (0..active)
};

// 多线程检索器
class Searcher
{
public:
    Searcher() = default;

    // 同步执行; onProgress 可为空 (percent, scanned, qualified)
    // pause 非空时, 工作线程在每个 chunk 边界检查它: 置位就原地等待, 清掉后接着算。
    // 检查粒度是 chunk (kLanes 个种子), 所以暂停最多延迟几十微秒。
    SearchResult run(const SearchSpec &spec,
                     const std::function<void(int, quint64, quint64)> &onProgress,
                     const std::atomic<bool> *cancel,
                     const std::atomic<bool> *pause = nullptr);

    // 交叉校验: 用原版分页实现与本引擎对比, 返回空串表示一致
    static QString selfCheck();
};

} // namespace sf
