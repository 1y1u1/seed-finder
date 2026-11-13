// sf::GpuBackend 的 CUDA 实现: 把引擎的抽象接口接到 gpucuda.cu 里的 GpuSearcher。
//
// NOTE: keep this file ASCII-only. nvcc-side tools parse source in code page 936;
// a UTF-8 Chinese comment can be mis-decoded and swallow the next line.
#include "gpuhook.h"
#include "gpucuda.h"

namespace sf {

namespace {

class CudaBackend : public GpuBackend
{
public:
    bool prepare(const std::vector<GpuLevelDesc> &levels,
                 const std::vector<GpuCond> &conds,
                 unsigned allow, unsigned off32, int lvFirst,
                 std::string *error) override
    {
        std::vector<LevelDesc> gl;
        gl.reserve(levels.size());
        for (size_t i = 0; i < levels.size(); ++i) {
            LevelDesc d;
            d.level = levels[i].level;
            d.tableStride = levels[i].tableStride;
            d.rankOff = levels[i].rankOff;
            d.rankCnt = levels[i].rankCnt;
            d.hardOff = levels[i].hardOff;
            d.hardCnt = levels[i].hardCnt;
            gl.push_back(d);
        }
        std::vector<CondG> gc;
        gc.reserve(conds.size());
        for (size_t i = 0; i < conds.size(); ++i) {
            CondG c;
            c.needMask = conds[i].needMask;
            c.needTotal = conds[i].needTotal;
            c.refuseMask = conds[i].refuseMask;
            c.atLeastMask = conds[i].atLeastMask;
            c.atLeast = conds[i].atLeast;
            c.atMostMask = conds[i].atMostMask;
            c.atMost = conds[i].atMost;
            gc.push_back(c);
        }
        return m_gpu.prepare(gl, gc, allow, off32, lvFirst, error);
    }

    bool scan(unsigned seedBegin, unsigned seedCount,
              std::vector<unsigned short> &out, std::string *error) override
    {
        return m_gpu.scan(seedBegin, seedCount, out, error);
    }

private:
    GpuSearcher m_gpu;
};

} // namespace

GpuBackend *createGpuBackend()
{
    if (gpuDeviceCount() <= 0) return nullptr;
    return new CudaBackend();
}

} // namespace sf
