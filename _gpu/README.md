# GPU 加速（CUDA）

CUDA 后端的实现与对拍程序都在这里，**已经入库**（`.gitignore` 只忽略构建产物：
`*.obj` / `release/` / `Makefile*` / 日志）。

`SeedFinder2.pro` 用 `exists(_gpu/gpucuda.obj)` 探测：编出 `gpucuda.obj` 就用 GPU，
没编出来就走 `src/core/gpuhook.cpp` 的空实现 —— 同一份源码、同一个 exe，
不需要维护两套代码，也不需要出两个可执行文件。

## 现状

CUDA 后端已经接进主程序：

- `src/core/gpuhook.h/.cpp` —— 后端抽象，纯 C++（不含 Qt、不含 CUDA 头）。
  没编 CUDA 时 `sf::createGpuBackend()` 返回 `nullptr`，引擎自动走 CPU，
  11 个测试程序的行为完全不变。
- `SeedFinder2.pro` 末尾用 `exists(_gpu/gpucuda.obj)` 探测，存在才定义
  `SF_WITH_CUDA` 并链接 `gpucuda_backend.cpp` + `cudart`。
- `sf::SearchSpec::gpu`（默认 1）。`Searcher::run` 先把每轮条件按轮连续展开
  （CPU/GPU 共用），再按块调后端 `scan()`。界面「用 GPU 加速」复选框和
  `--nogpu` 命令行都能关掉。
- `sf::SearchResult::usedGpu` 标记这一轮真的是 GPU 算完的，界面状态文字加 `GPU · ` 前缀。

### 关键设计

- **2e5 个种子以下交给 CPU**。GPU 首次要初始化 CUDA 上下文（几十毫秒），
  每块还要建表 + 回传，扫几千个种子时反而比 CPU 慢一个数量级。
- **分块从 64K 起步，每块翻 4 倍，封顶 4M**。小步起步是因为很多查询扫不到十万个
  种子就"找够了"提前停止，固定 4M 光建 128 MB 选卡表就白花好几毫秒；大步收尾是
  为了避免大范围扫描被 launch 开销拖慢。
- **prepare 失败 / scan 出错 / 回读长度不符 → 整轮清场，从 0 交给 CPU 重扫**。
  不清场的话会重复计数。
- **不用共享内存**。去重后 512 种子窗口要 94 KB 表（顶到 `optin` 上限 101376 附近），
  而每张表只被复用 5.7 次，靠 48 MB 的 L2 更划算。

## 实测（RTX 5080 Laptop，60 SM / 1.5 GHz / 896 GB/s / 48 MB L2 / sm_120）

```
gpucuda_test check 300000 2 0xA1B2C3D4
  verify: OK (300000 tables, 2400000 words compared)

gpucuda_test pipe 4000000 25
  gpu: 2.3 ms  1752 M seeds/s
  cpu1: 0.04 M seeds/s      gpu / 1 cpu-thread ≈ 36000x
```

| 版本 | build ms | eval ms | M seeds/s |
|---|---|---|---|
| 单链（CHAINS=1，block 512） | 12.35 | 3.77 | 1985 |
| 多链 CHAINS=2 | 12.64 | 3.41 | 1993 |
| 多链 CHAINS=8 | 21.22 | 6.51 | 1154 |

- 24 核 CPU 主线 25 轮约 **31 M seeds/s**，GPU 约 **1.75 ~ 2.04 G seeds/s**，即 **56 ~ 66×**。
  全量 42.9 亿种子从 2.3 分钟降到 **2.1 ~ 2.5 秒**。
- **occupancy 本来就是满的**：`26 regs × 512 线程 × 3 块 = 39936` < 每 SM 的 65536，
  `3 × 512 = 1536` = `maxThreadsPerSM`，ptxas 报 0 spill / 0 stack frame。
- **多链没有收益，反而更慢**：瓶颈是每条 MT 链 398 步严格串行依赖，60 SM × 48 warp
  已经把延迟掩盖住了；再加多链只推高寄存器压力、恶化访存合并（CHAINS=8 → 48 regs，
  吞吐掉到 1154 M/s）。所以默认 `chains=1, blockSize=512`，多链仅作可选调参保留。

## 算法要点

CPU 端瓶颈是 MT19937 初始化那条 **398 步串行链**（每步一次 32 位乘法）。

1. **滚动两词代替 `head[132]`**：CPU 的 `LaneBatch` 保留 `head[0..131]` 整段前缀，
   但抽卡下标 `k` 逐次 +1，每次只用 `head[k]` 和 `head[k+1]`。GPU 上改成
   `a`/`b` 两个滚动变量，寄存器占用从 132 词降到 2 词 —— 大数组会溢到 local memory，
   速度掉一个数量级。
2. **去重恒等式**（CPU 主线已有）：`(种子 s, 第 lv 轮)` 的整张选卡表只由
   `R = (s + uid + mode + 101*lv) mod 2^32` 决定，因为 `INV*101 ≡ 1 (mod 2^32)`。
   所以连续 N 个种子 × W 轮只需 `N + 101*(W-1)` 条链。GPU 一次建这一段表，
   25 轮重复读同一批表，只从 HBM 取一次，其余命中 L2。

## 文件

| 文件 | 作用 |
|---|---|
| `gpucuda.h` | 纯 C++ 接口（`LevelDesc` / `CondG` / `GpuSearcher`），不依赖 Qt |
| `gpucuda.cu` | kernel：`kBuildTables<CHAINS>` / `kBuildTablesStrided<CHAINS>` / `kEvalSeeds` |
| `gpucuda_backend.cpp` | `sf::createGpuBackend()` 的实现，包装 `GpuSearcher` |
| `gpucuda_test.cpp` | 四种模式：`dump` / `check`（与 CPU 逐位对拍）/ `pipe`（吞吐）/ `tune`（调参扫描） |
| `gpucmp.cpp` | **主程序级**交叉对拍：18 个用例（3 场景 × 4 条件 + 6 个 autoStop），直接比 `SearchResult` |
| `build_cuda_test.bat` | 编 `gpucuda_test.exe` |
| `build_cmp.bat` | 编 `gpucmp.exe` |

（早期基于 D3D11 的原型已随 CUDA 版落地删除。）

## 构建

`.cu` **不能**写进 `SOURCES`（qmake 不会调 nvcc），要先单独编成 obj：

```
nvcc -Xcompiler "/MD" -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH -arch=sm_120 -O3 -c gpucuda.cu -o gpucuda.obj
```

静态版必须用 `/MT` 编成另一个 obj（`gpucuda_static.obj`），并在 `.pro` 里按
`static` 分支分别链接 —— CRT 混用会报一堆 `__imp_llrintf` / `__imp_nearbyintf` 未解析。

踩过的坑：

- **`.cu` 与 `_gpu/*.h` 必须纯 ASCII**。`nvcc` 的 `cudafe++` 按代码页 936 解析源码，
  UTF-8 中文注释的尾字节会被 GBK 解成 `0x5C`（反斜杠），吞掉下一行，报
  `error: expected a declaration` / `identifier "LevelDesc" is undefined`。
- **`-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` 必需**。MSVC 14.52 的 `yvals_core.h:915-918`
  硬性要求 `__CUDACC_VER_MAJOR__ >= 13.4`，否则 `_EMIT_STL_ERROR(STL1002, ...)`，
  而 `cudafe++` 撞上断言后会以 `0xC0000005` 崩，看起来像编译器坏了。
- **`-Xcompiler "/MD"` 必需**（动态版），否则 `gpucuda.obj` 是 `/MT`，
  链接报 `LNK2038: 检测到"RuntimeLibrary"的不匹配项`。
- **CUDA 13 已不带 `culibos.lib`**，静态链接只要 `-lcudart_static`。
- `cudaEventElapsedTime` 只收 `float*`；`cudaDeviceProp::clockRate` 在 CUDA 13 已移除，
  要用 `cudaDeviceGetAttribute(&v, cudaDevAttrClockRate, 0)`。

## 对拍的意义

`gpucmp.exe <种子数> <波数>` 会跑 18 个用例，全绿输出 `18 cases, 0 bad -> ALL OK`。
它抓到过两个真 bug：

1. GPU 侧 `prepare()` 的 `lvFirst` 取 `levels.front().level`，而 `levels` 可能含
   `level <= 0`（F 1~2，位恒为 11u 不需要表），`r0` 整体偏 101 张表，
   每个 bucket 的 example 种子都差 0x65。改为 `prepare()` 显式传入并存下。
2. CPU 侧 F 1~2 的硬性条件淘汰 `if (!ok) aliveCount = 0;` **没清 `alive[]`**，
   后面按 `alive[]` 逐个计数时整块种子都被算成"合格"——只在全局条件挂 F 1~2 时触发。

写 autoStop 用例时注意：条件必须是**恒真**的，否则两端都扫满整个区间，
比出来的 `scanned` 相等毫无意义。第一版用"全区间必出红眼"——25 轮同时有红眼的种子
一个都没有；第二版用"禁出普通僵尸"——`bits` 里带 `d1[0]` 传下来的 `Z_REGULAR` 位，
全判死。最终用"至多出 19 个冰冻僵尸"（每波最多 10 张卡，抽屉原理下恒成立）。
CPU 侧还必须 `threads = 1`，否则各线程块边界不同，停下来时扫过的种子集天然不一样。
