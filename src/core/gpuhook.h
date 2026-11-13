// GPU 后端抽象。
//
// 引擎 (src/core/seedengine.cpp) 只依赖这个头里的接口, 不依赖 CUDA:
// 没注册后端时 (测试程序 / 没装 CUDA 的机器) Searcher 全部走 CPU 路径,
// 行为与以前一模一样。真正的 CUDA 实现放在 _gpu/ 下, 由 main.cpp 在启动时
// 通过 createGpuBackend() 拿到并注册。
//
// 这个头必须能被 nvcc 处理的编译单元安全包含: 只用标准库, 不用 Qt。
#pragma once

#include <string>
#include <vector>

namespace sf {

// 一轮的检查描述。该轮的条件按"连续展开"放在 GpuCond 数组里,
// 这里只存起点和条数 (同一轮可能被多条规则带覆盖, 条件取"与")。
struct GpuLevelDesc
{
    int level = 0;              // 轮次 (从 1 开始)
    unsigned tableStride = 0;   // = 101 * (level - levelFirst), 选卡表内的偏移
    unsigned rankOff = 0;       // 排名条件在 GpuCond 数组里的起点
    unsigned rankCnt = 0;
    unsigned hardOff = 0;       // 硬性条件的起点
    unsigned hardCnt = 0;
};

// WaveCondition 的 POD 版本, 可以直接上传显存。
struct GpuCond
{
    unsigned needMask = 0;
    unsigned needTotal = 0;
    unsigned refuseMask = 0;
    unsigned atLeastMask = 0;
    unsigned atLeast = 0;
    unsigned atMostMask = 0;
    unsigned atMost = 0;
};

class GpuBackend
{
public:
    virtual ~GpuBackend() {}

    // 上传条件表。一次查找调用一次, 之后 scan() 不再碰主机内存。
    // lvFirst 是"第一条需要算选卡表的轮次"(levels 里可能有 level<=0 的轮,
    // 那些轮位恒为 11u, 不参与选卡表偏移计算, 所以不能拿 levels.front() 当基准)。
    virtual bool prepare(const std::vector<GpuLevelDesc> &levels,
                         const std::vector<GpuCond> &conds,
                         unsigned allow, unsigned off32, int lvFirst,
                         std::string *error) = 0;

    // 扫描一段连续种子。out[i] == 0 表示该种子被硬性条件刷掉,
    // 否则是 (满足的轮数 + 1)。
    virtual bool scan(unsigned seedBegin, unsigned seedCount,
                      std::vector<unsigned short> &out, std::string *error) = 0;
};

// 注册 / 卸载后端。传 nullptr 表示不用 GPU。
void setGpuBackend(GpuBackend *backend);
GpuBackend *gpuBackend();

// 界面用: 有没有可用的 GPU 后端。
inline bool gpuBackendReady() { return gpuBackend() != nullptr; }

// 由 _gpu/gpucuda_backend.cpp 实现; 没有可用设备时返回 nullptr。
GpuBackend *createGpuBackend();

} // namespace sf
