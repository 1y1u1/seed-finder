#include "gpuhook.h"

namespace sf {

namespace {
GpuBackend *g_backend = nullptr;
}

void setGpuBackend(GpuBackend *backend)
{
    g_backend = backend;
}

GpuBackend *gpuBackend()
{
    return g_backend;
}

#ifndef SF_WITH_CUDA
// 没编 CUDA 时 (测试程序 / 没装 CUDA 的机器) 提供空实现,
// 这样 main.cpp 里的初始化代码不用改动也能链接通过。
GpuBackend *createGpuBackend()
{
    return nullptr;
}
#endif

} // namespace sf
