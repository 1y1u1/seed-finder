# GPU 加速可行性研究

D3D11 compute shader 路线（零安装依赖，Qt 工程直接链 `d3d11.lib` + `d3dcompiler.lib`，
HLSL 用 `D3DCompile()` 运行时编译）。

## 为什么选 D3D11 而不是 CUDA

本机实测环境：

| 途径 | 状态 |
|---|---|
| CUDA Toolkit (`nvcc`) | **不可用** —— 未安装，全盘无 `nvrtc64_*.dll` |
| OpenCL | **不可用** —— `HKLM\SOFTWARE\Khronos\OpenCL\Vendors` 不存在，无厂商 ICD |
| Vulkan | **不可用** —— `vulkan-1.dll` 在，但 `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` 不存在 |
| DirectML | 只有 `DirectML.dll`，**缺 `DirectML.h` 头文件** |
| **D3D11 / D3D12** | **完整可用** —— 头文件、lib、`d3dcompiler_47.dll` 全在 |
| CUDA Driver API (`nvcuda.dll`) | 在，但要自己写 PTX 或另装 toolkit |

硬件：NVIDIA GeForce RTX 5080 Laptop GPU，compute_cap 12.0，15.6 GB 显存，驱动 610.62 / CUDA UMD 13.3。

## 关键算法发现

CPU 端瓶颈是 MT19937 初始化那条 **398 步串行链**（每步一次 32 位乘法），
每算一轮出怪都要重跑一遍。CPU 靠 `LaneBatch` 把 16 个种子的链交错在一起来摊薄
指令延迟，但仍是标量执行。

GPU 擅长"多条独立串行链并排"，所以**一个 GPU 线程算一个种子**正合适。

移植时的一个重要简化：`seedfinder3/src/core/seedengine.cpp` 的 `LaneBatch` 保留了
`head[0..131]` 整段 MT 前缀，但抽卡时下标 `k` 是逐次 `+1` 前进的，每次只用
`head[k]` 和 `head[k+1]`。改用**滚动两词**（`a = head[k]`、`b = head[k+1]`）即可，
寄存器占用从 132 个词降到 2 个 —— 这对 GPU 至关重要，大数组会溢出到 local memory，
速度掉一个数量级。

## 两个原型

### `gpuprobe.cpp` —— 只算 bits，测纯算力上限

一个 GPU 线程算一个 `(种子, 轮次)` 组合，输出上场表 `bits`。
全部结果与 CPU 的 `rowBitsBySeed` 逐位对比。

### `gpupipe.cpp` —— 完整流水线

一个 GPU 线程算一个种子的**全部轮次**，内部完成条件判定 + 硬性条件提前淘汰，
只回读一个 `uint`/种子给主机做分档统计。

```
gpupipe.exe <种子数> [关掉硬性条件=1]
```

## 实测结果（RTX 5080 Laptop）

`gpuprobe.exe 200000`：

```
gpu:      13.5 ms   371.33 M (seed,level)/s
verify: OK  (5000000 compared, 0 bad)
cpu1:   5603.1 ms     0.89 M (seed,level)/s
```

`gpupipe.exe 4000000`（带硬性条件，大部分种子早早淘汰）：

```
gpu pipeline:  86.3 ms  (46.33 M seeds/s, 1158 M unit/s)
verify: OK  (4000000 seeds compared, 0 bad)
cpu single thread: 32829.2 ms (0.122 M seeds/s)
gpu / 1 thread = 380x
```

`gpupipe.exe 4000000 1`（关掉硬性条件，每个种子都算满 25 轮，真实上界）：

```
gpu pipeline: 108.5 ms  (36.88 M seeds/s, 922 M unit/s)
verify: OK  (4000000 seeds compared, 0 bad)
cpu single thread: 111330.6 ms (0.036 M seeds/s)
gpu / 1 thread = 1026x
```

对照：主线 CPU 24 核全开，25 轮约 **2.2 ~ 2.9 M seeds/s**。
GPU 最坏情况 **36.9 M seeds/s**，约 **13~17 倍**。

## 结论

- 算法移植**完全正确**（两个原型共 900 万条结果逐位一致，0 mismatch）。
- 真实场景下 GPU 比 24 核 CPU 快一个数量级，值得集成。
- 尚未接入 `seedfinder3`：需要处理分块调度（单次 `Dispatch` 控制在 TDR 2 秒以内）、
  多条件同轮（当前原型假设每轮最多一条条件）、以及 GPU 不可用时的静默回退。

## 构建

```
_gpu\build.bat        :: gpuprobe
_gpu\build_pipe.bat   :: gpupipe
```

两个 `.pro` 都不列 `seedengine.cpp` —— 源码里已经 `#include` 了它，再列会重复定义。
