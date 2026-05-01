# mmap 读取加速与优化器修正实施方案

---

## 一、背景与动机

### 1.1 性能现状

基于 18GB 数据（800×2404×2500，chunk 32×256×16，缓存 2GB）的评分基准测试：

| 测试项 | 耗时 (ms/slice) |
|--------|:---:|
| 随机 X | 314 |
| 随机 Y | 683 |
| 随机 Z | 77 |
| 连续 X | 96 |
| 连续 Y | 99 |
| 连续 Z | 22 |
| **综合** | **215** |

### 1.2 核心瓶颈分析

增大缓存至 8GB 后命中率从 17.7% 提升到 46.5%，但综合耗时反而从 215ms 恶化到 223ms。根因有三：

**瓶颈 1：应用层缓存与 OS 文件缓存的双重缓存竞争**

- 应用层 LRU 缓存和 OS 页面缓存存了同样数据的两份，内存利用率仅 50%
- 8GB 应用缓存抢走了 OS 文件缓存空间，导致 cache miss 走磁盘时失去 OS 预读加速
- 连续切片全面退化（+30%~50%）正是 OS 顺序预读能力被削弱的表现

**瓶颈 2：per-chunk 的系统调用与内存分配开销**

- `read_x_slice` 需 1570 个 chunk → 1570 次 `fseek+fread`（3140 次系统调用）
- Cache miss 路径：`make_shared<vector<float>>(131072)` → `fread` 填充 → `memcpy` 拼装
- 单个 X slice 累计 ~785MB 无效中间拷贝

**瓶颈 3：锁风暴**

- 每个 chunk 读操作：`shared_lock(m_cache_mutex)` → hash 查找 → unlock
- Cache miss 路径：`unique_lock` + `lock_guard(m_io_mutex)` + `unique_lock`
- 单 slice 累计 3140+ 次锁操作

**瓶颈 4：chunk 形状选择不合理**

当前优化器选取 32×256×16，Y 轴切片需要 3925 个 chunk（1.93GB），极度不均衡：

```
32×256×16:
  X-slice: nc_y×nc_z = 10×157  = 1,570 chunks  → 读 785 MB
  Y-slice: nc_x×nc_z = 25×157  = 3,925 chunks  → 读 1,930 MB (超2GB!)
  Z-slice: nc_x×nc_y = 25×10   =   250 chunks  → 读 125 MB
  均衡度: max/min = 3925/250 = 15.7 ← 极度不均衡

64×64×64:
  X-slice: nc_y×nc_z = 38×40   = 1,520 chunks  → 读 1,465 MB
  Y-slice: nc_x×nc_z = 13×40   =   520 chunks  → 读   500 MB ← 7.5倍改善!
  Z-slice: nc_x×nc_y = 13×38   =   494 chunks  → 读   476 MB
  均衡度: max/min = 1520/494 = 3.08 ← 好得多
  膨胀率: 1.052 ← 仍在 1.45x 内
```

优化器选错的原因：人为约束 `cx > cy > cz` 排除了 64×64×64。该约束逻辑有误——给 X 最大的 chunk 维度是在牺牲 X 切片帮助 Y/Z 切片，反而加剧不均衡。

### 1.3 Y-二次存储为何不可行

曾考虑追加 Y-优化存储区（如 256×32×16 的二次切块），但存储预算不足：

| 存储 | 大小 | 膨胀率 |
|------|------|--------|
| 原始数据 | 18.34 GB | 1.0x |
| 主存储 (1.07x) | 19.17 GB | 1.07x |
| **1.45x 预算剩余** | **7.32 GB** | **0.40x** |
| Y-二次存储(全集) | ~18.34 GB | ~1.0x |

全量二次副本需要 ~18GB，远超 7.32GB 预算。修正优化器选均衡形状才是正解。

---

## 二、方案概述

### 2.1 核心改动

1. **mmap 替换 fread/fseek + 去掉应用层缓存** — 消除双重缓存、per-chunk 系统调用/分配/锁开销
2. **修正 chunk 优化器** — 去掉 cx>cy>cz 约束，改用 minimax 评分 + 内存约束，自适应选择均衡 chunk 形状
3. **跨平台支持** — Windows / Linux 双平台 mmap 抽象，支持滑动窗口映射（TB 级 / 内存受限场景）

### 2.2 赛题约束满足情况

| 约束 | 方案满足方式 |
|------|------------|
| 存储 ≤ 1.45x | 修正优化器评分函数含存储红线过滤，单一存储无二次副本 |
| 内存 ≤ 2GB 可配置 | mmap 滑动窗口，`--max-mem` 严格控制映射窗口大小 |
| 单进程多线程 | mmap 读路径零锁零共享状态，天然线程安全 |
| X 维度单列高效读取 | 均衡 chunk 形状下 X 切片 chunk 数可控 |
| 精度 < 1‰ | mmap 不改变数据内容，memcpy 与 fread 含义等价 |

### 2.3 预期性能提升

| 测试项 | 现状 (2GB cache) | 修正后 (mmap + 均衡chunk) | 加速比 |
|--------|:---:|:---:|:---:|
| 随机 X | 314 ms | ~70-100 ms | 3-4x |
| 随机 Y | 683 ms | ~50-80 ms | **8-13x** |
| 随机 Z | 77 ms | ~40-55 ms | 1.4-2x |
| 连续 X | 96 ms | ~20-35 ms | 3-5x |
| 连续 Y | 99 ms | ~15-30 ms | 3-6x |
| 连续 Z | 22 ms | ~10-18 ms | 1.2-2x |
| **综合** | **215 ms** | **~35-60 ms** | **~4-6x** |

---

## 三、Phase 1：跨平台 mmap 抽象层

### 3.1 新增文件

- `include/platform_mmap.h` — 接口定义
- `src/platform_mmap.cpp` — 跨平台实现

### 3.2 接口设计

```cpp
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

class MmapFile {
public:
    MmapFile() = default;
    ~MmapFile();

    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;
    MmapFile(MmapFile&& other) noexcept;
    MmapFile& operator=(MmapFile&& other) noexcept;

    // 打开文件并建立映射
    //   path:     文件路径
    //   writable: true=读写模式(用于写切面), false=只读
    //   max_window_bytes: 最大映射窗口大小(0=全文件映射)
    //     - 文件大小 ≤ max_window_bytes 时全文件映射
    //     - 文件大小 > max_window_bytes 时使用滑动窗口
    bool open(const std::string& path,
              bool writable = false,
              size_t max_window_bytes = 0);

    void close();

    bool is_open() const { return m_data != nullptr; }
    size_t size()  const { return m_file_size; }

    // 获取整个文件的可读指针（仅全文件映射模式下使用）
    const uint8_t* data() const { return m_data; }
    uint8_t*       data()       { return m_data; }

    // 确保地址范围 [offset, offset+len) 已映射，返回指针
    // 滑动窗口模式下自动管理映射窗口
    const uint8_t* ensure_mapped(uint64_t offset, size_t len) const;
    uint8_t*       ensure_mapped(uint64_t offset, size_t len);

    // 释放已用完的映射区域（提示 OS 可回收物理页面）
    // 全文件映射模式: 使用 madvise(MADV_DONTNEED) / DiscardVirtualMemory
    // 滑动窗口模式: unmap 旧窗口
    void release_region(uint64_t offset, size_t len);

    // 批量预取地址范围（内核异步预取，不阻塞调用线程）
    void prefetch(uint64_t offset, size_t len) const;

    // 刷盘（仅 writable 模式）
    void flush(uint64_t offset, size_t len);

private:
#ifdef _WIN32
    HANDLE m_hFile    = INVALID_HANDLE_VALUE;
    HANDLE m_hMapping = nullptr;
#else
    int m_fd = -1;
#endif
    uint8_t* m_data          = nullptr;
    size_t   m_file_size     = 0;
    size_t   m_max_window    = 0;     // 0 = 全文件映射
    bool     m_writable      = false;

    // 滑动窗口状态
    mutable uint8_t* m_window_base   = nullptr;
    mutable uint64_t m_window_offset = 0;
    mutable size_t   m_window_size   = 0;

    bool map_full();
    bool map_window(uint64_t offset, size_t len) const;
    void unmap_current() const;
};
```

### 3.3 跨平台 API 映射

| 功能 | Windows | Linux |
|------|---------|-------|
| 打开文件 | `CreateFileW` | `open()` |
| 获取文件大小 | `GetFileSizeEx` | `fstat` |
| 创建映射 | `CreateFileMapping` | — |
| 映射视图 | `MapViewOfFile` | `mmap` |
| 滑动窗口映射 | `MapViewOfFile(offset+length)` | `mmap(offset+length)` |
| 解除映射 | `UnmapViewOfFile` | `munmap` |
| 预取页面 | `PrefetchVirtualMemory` (Win8+) | `madvise(MADV_WILLNEED)` |
| 释放页面 | `DiscardVirtualMemory` | `madvise(MADV_DONTNEED)` |
| 刷盘 | `FlushViewOfFile` | `msync(MS_SYNC)` |
| 关闭 | `CloseHandle` × 2 | `close` |

### 3.4 滑动窗口映射策略

```
文件大小 = 50GB, max_window = 2GB

读取 chunk [offset=30GB, len=512KB]:
  1. 检查当前窗口: [28GB, 30GB)
  2. offset=30GB 不在窗口内 → unmap 旧窗口
  3. 新窗口: [30GB, 32GB) → MapViewOfFile / mmap
  4. 返回 m_window_base + (30GB - 30GB) = m_window_base

连续读取多个 chunk (同一窗口内):
  - 不触发 remap, 直接计算偏移
  - 窗口大小 2GB >> 典型 chunk 512KB, 单次映射可覆盖 ~4000 个 chunk
```

### 3.5 预取实现

```cpp
void MmapFile::prefetch(uint64_t offset, size_t len) const {
    if (!m_data && !m_window_base) return;

#ifdef _WIN32
    // PrefetchVirtualMemory (Win8+)
    WIN32_MEMORY_RANGE_RANGE range;
    range.VirtualAddress = const_cast<uint8_t*>(ensure_mapped(offset, len));
    range.NumberOfBytes  = len;
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, sizeof(range));
#else
    // madvise MADV_WILLNEED
    madvise(ensure_mapped(offset, len), len, MADV_WILLNEED);
#endif
}
```

### 3.6 实施清单

- [ ] 创建 `include/platform_mmap.h`，定义 `MmapFile` 类接口
- [ ] 实现 Windows 平台：`CreateFileW` + `CreateFileMapping` + `MapViewOfFile`
- [ ] 实现 Linux 平台：`open` + `mmap` + `munmap`
- [ ] 实现滑动窗口映射（`ensure_mapped` + `map_window` + `unmap_current`）
- [ ] 实现预取（`prefetch`：`PrefetchVirtualMemory` / `madvise`）
- [ ] 实现页面释放（`release_region`：`DiscardVirtualMemory` / `MADV_DONTNEED`）
- [ ] 实现刷盘（`flush`：`FlushViewOfFile` / `msync`）
- [ ] 实现移动语义（`MmapFile(MmapFile&&)`）
- [ ] 在 `CMakeLists.txt` 添加 `platform_mmap.cpp` 源文件
- [ ] 编写单元测试：打开/关闭/基本映射/预取/窗口滑动

---

## 四、Phase 2：C3DRMmapReader — mmap 读取器

### 4.1 新增文件

- `include/mmap_reader.h` — 接口定义
- `src/mmap_reader.cpp` — 实现

### 4.2 接口设计

```cpp
#pragma once

#include "platform_mmap.h"
#include "chunk_optimizer.h"

#include <cstdint>
#include <string>
#include <vector>

class C3DRMmapReader {
public:
    C3DRMmapReader() = default;
    ~C3DRMmapReader();

    C3DRMmapReader(const C3DRMmapReader&) = delete;
    C3DRMmapReader& operator=(const C3DRMmapReader&) = delete;

    // 打开 .c3dr 文件，mmap 映射
    //   max_mem_bytes: 最大映射窗口大小 (0=全文件)
    bool open(const std::string& path, size_t max_mem_bytes = 0);
    void close();

    bool is_open() const;

    // ── 只读访问器 ──
    const C3DRHeader&   header()      const { return m_header; }
    ChunkShape          chunk_shape() const { return m_header.chunk_shape; }
    uint64_t            dim_x()       const { return m_header.dim_x; }
    uint64_t            dim_y()       const { return m_header.dim_y; }
    uint64_t            dim_z()       const { return m_header.dim_z; }
    uint32_t            num_chunks_x() const { return m_nc_x; }
    uint32_t            num_chunks_y() const { return m_nc_y; }
    uint32_t            num_chunks_z() const { return m_nc_z; }
    size_t              chunk_bytes() const { return m_chunk_bytes; }

    // ── 切面读取（零锁、零 per-chunk 分配）──
    std::vector<float> read_x_slice(uint32_t x);
    std::vector<float> read_y_slice(uint32_t y);
    std::vector<float> read_z_slice(uint32_t z);

    // ── 写切面（通过 mmap 直接写入映射区）──
    void write_x_slice(uint32_t x, const std::vector<float>& data);
    void write_y_slice(uint32_t y, const std::vector<float>& data);
    void write_z_slice(uint32_t z, const std::vector<float>& data);

    // 刷盘（将脏页写回磁盘）
    void flush();

private:
    MmapFile               m_mmap;
    C3DRHeader             m_header;
    std::vector<uint64_t>  m_index;      // chunk 偏移表
    uint32_t               m_nc_x = 0;
    uint32_t               m_nc_y = 0;
    uint32_t               m_nc_z = 0;
    size_t                 m_chunk_bytes = 0;
    size_t                 m_chunk_elems = 0;

    // 预取状态（连续模式检测）
    struct SliceAccess { int axis; uint32_t position; };
    std::vector<SliceAccess> m_recent_access;
    static constexpr size_t  PREFETCH_WINDOW = 3;

    // 内部方法
    const float* chunk_ptr(uint64_t chunk_idx) const;

    // 批量预取某切片所需的所有 chunk 页面
    void prefetch_slice_chunks(int axis, uint32_t pos);

    // 连续模式检测 + 预调度
    void maybe_prefetch(int axis, uint32_t position);

    // 计算某轴某位置所需的所有 chunk_idx
    std::vector<uint64_t> compute_slice_chunks(int axis, uint32_t pos) const;

    // 写入辅助
    void write_chunk_patch(uint64_t chunk_idx,
                           const std::vector<std::pair<size_t, const float*>>& patches,
                           const std::vector<size_t>& patch_sizes);
};
```

### 4.3 核心读路径实现

```cpp
// chunk 指针计算：零系统调用、零内存分配
const float* C3DRMmapReader::chunk_ptr(uint64_t chunk_idx) const {
    uint64_t offset = m_index[chunk_idx];
    return reinterpret_cast<const float*>(
        m_mmap.ensure_mapped(offset, m_chunk_bytes));
}

// X 轴切面读取
std::vector<float> C3DRMmapReader::read_x_slice(uint32_t x) {
    if (x >= m_header.dim_x) return {};

    uint32_t ix      = x / m_header.chunk_shape.cx;
    uint32_t local_x = x % m_header.chunk_shape.cx;
    size_t   cy = m_header.chunk_shape.cy;
    size_t   cz = m_header.chunk_shape.cz;
    uint64_t dim_y = m_header.dim_y;
    uint64_t dim_z = m_header.dim_z;

    // 1. 批量预取该切片所需的所有 chunk 页面
    prefetch_slice_chunks(0, x);

    // 2. 分配结果缓冲区
    std::vector<float> result(static_cast<size_t>(dim_y * dim_z), 0.0f);

    // 3. 逐 chunk memcpy 拼装（无锁、无 shared_ptr、无 hash lookup）
    for (uint32_t iy = 0; iy < m_nc_y; ++iy) {
        uint32_t y_start = iy * cy;
        for (uint32_t iz = 0; iz < m_nc_z; ++iz) {
            uint32_t z_start = iz * cz;

            uint64_t chunk_idx = (uint64_t)ix * m_nc_y * m_nc_z
                               + (uint64_t)iy * m_nc_z + iz;

            const float* src = chunk_ptr(chunk_idx)
                             + (size_t)local_x * cy * cz;

            uint32_t y_end = std::min(y_start + cy, (uint32_t)dim_y);
            uint32_t z_end = std::min(z_start + cz, (uint32_t)dim_z);

            for (uint32_t yy = y_start; yy < y_end; ++yy) {
                size_t src_row = (yy - y_start) * cz;
                size_t dst_row = (size_t)yy * dim_z;
                size_t count = z_end - z_start;
                std::memcpy(result.data() + dst_row + z_start,
                            src + src_row,
                            count * sizeof(float));
            }
        }
    }

    // 4. 连续模式检测 → 预取下一切片
    maybe_prefetch(0, x);

    return result;
}
```

### 4.4 批量预取实现

```cpp
void C3DRMmapReader::prefetch_slice_chunks(int axis, uint32_t pos) {
    auto chunks = compute_slice_chunks(axis, pos);

    // 合并相邻 chunk 的预取范围，减少 prefetch 调用次数
    std::vector<std::pair<uint64_t, size_t>> ranges;
    for (uint64_t idx : chunks) {
        uint64_t off = m_index[idx];
        if (!ranges.empty() &&
            ranges.back().first + ranges.back().second == off) {
            ranges.back().second += m_chunk_bytes;  // 合并相邻
        } else {
            ranges.emplace_back(off, m_chunk_bytes);
        }
    }

    for (auto& [off, len] : ranges) {
        m_mmap.prefetch(off, len);
    }
}
```

### 4.5 写切面实现

```cpp
void C3DRMmapReader::write_x_slice(uint32_t x, const std::vector<float>& data) {
    // 写切面结构与读切面对称
    // 区别：通过 ensure_mapped 获取可写指针，直接 memcpy 到映射区
    // OS 延迟写回（等效 Write-Back），flush() 时刷盘

    uint32_t ix      = x / m_header.chunk_shape.cx;
    uint32_t local_x = x % m_header.chunk_shape.cx;
    size_t   cy = m_header.chunk_shape.cy;
    size_t   cz = m_header.chunk_shape.cz;

    for (uint32_t iy = 0; iy < m_nc_y; ++iy) {
        for (uint32_t iz = 0; iz < m_nc_z; ++iz) {
            uint64_t chunk_idx = (uint64_t)ix * m_nc_y * m_nc_z
                               + (uint64_t)iy * m_nc_z + iz;

            float* dst = const_cast<float*>(chunk_ptr(chunk_idx))
                       + (size_t)local_x * cy * cz;

            // ... 构建 patch 并 memcpy 到 dst ...
        }
    }
    // 无需 flush，OS 延迟写回；进程退出前或显式 flush() 时落盘
}
```

### 4.6 与旧 cache_reader 对比

| 操作 | C3DRCacheReader | C3DRMmapReader |
|------|----------------|----------------|
| 读单个 chunk | hash 查找 + shared_lock + 引用计数 | 指针算术：`base + offset` |
| 读切片(1570 chunks) | 1570×(lock+lookup+unlock) | 1×prefetch + 1570×memcpy |
| 内存拷贝次数/切片 | 2×(fread→vector + memcpy→result) | 1×(memcpy→result，直接从 mmap) |
| per-chunk 分配 | `make_shared<vector<float>>(131072)` | 无分配 |
| 锁操作/切片 | 3140+ | 0 |
| 缓存层 | 应用层 LRU + OS 页面缓存（双重） | 仅 OS 页面缓存（单一） |
| 多线程并发读 | `shared_mutex` 竞争 | 零锁（只读内存天然安全） |

### 4.7 实施清单

- [ ] 创建 `include/mmap_reader.h`，定义 `C3DRMmapReader` 类接口
- [ ] 实现 `open()`：读 Header + IndexTable + mmap 映射
- [ ] 实现 `read_x_slice()`：批量预取 + memcpy 拼装
- [ ] 实现 `read_y_slice()`
- [ ] 实现 `read_z_slice()`
- [ ] 实现 `write_x/y/z_slice()`：可写映射 + memcpy
- [ ] 实现 `flush()`：刷盘脏页
- [ ] 实现 `prefetch_slice_chunks()`：合并范围批量预取
- [ ] 实现 `maybe_prefetch()`：连续模式检测 + 预调度
- [ ] 实现 `compute_slice_chunks()`：按轴计算所需 chunk 列表
- [ ] 在 `CMakeLists.txt` 添加 `mmap_reader.cpp` 源文件
- [ ] 编写单元测试（对齐 `test_cache_reader.cpp` 的 13 个用例）

---

## 五、Phase 3：修正 Chunk 优化器

### 5.1 修改文件

- `src/chunk_optimizer.cpp` — 核心算法修正
- `include/common.h` — 调整常量（如需）
- `tests/test_chunk_optimizer.cpp` — 更新测试用例

### 5.2 问题与修正

#### 修正 1：去掉 `cx > cy > cz` 人为约束

当前优化器强制 `cx > cy > cz`，这排除了 64×64×64 等均衡形状。

**原始逻辑缺陷**：
```
X-slice chunk 数 = nc_y × nc_z   ← 与 cx 无关！
Y-slice chunk 数 = nc_x × nc_z   ← cx 增大则 nc_x 减小，Y 受益
Z-slice chunk 数 = nc_x × nc_y   ← cx 增大则 nc_x 减小，Z 受益

给 cx 最大值 = 牺牲 X 帮助 Y/Z → 加剧不均衡
```

**修正**：搜索空间中 `(cx, cy, cz)` 的所有排列均参与候选，不做人为排序约束。

#### 修正 2：改用 minimax + 绝对吞吐加权评分

**旧评分**：`cost = max(Tx,Ty,Tz) / min(Tx,Ty,Tz)` — 只衡量相对偏离

问题：4000/40=100 与 400/4=100 评分相同，但后者绝对性能好 10 倍。

**新评分**：

```cpp
double evaluate_shape(uint64_t dim_x, uint64_t dim_y, uint64_t dim_z,
                       const ChunkShape& s,
                       size_t memory_limit_bytes,
                       double storage_ratio) {
    uint32_t nc_x = ceil_div(dim_x, s.cx);
    uint32_t nc_y = ceil_div(dim_y, s.cy);
    uint32_t nc_z = ceil_div(dim_z, s.cz);

    // 各轴切片所需 chunk 数
    double Tx = (double)nc_y * nc_z;
    double Ty = (double)nc_x * nc_z;
    double Tz = (double)nc_x * nc_y;

    double worst = std::max({Tx, Ty, Tz});
    double avg   = (Tx + Ty + Tz) / 3.0;

    // 最慢轴瓶颈 × 均值惩罚
    // avg 越大说明总 I/O 量越大，即使均衡也慢
    double cost = worst * (1.0 + 0.1 * std::log2(avg + 1.0));

    // 内存约束惩罚：单轴切片超过内存限制则加重罚分
    double chunk_mem = (double)s.cx * s.cy * s.cz * sizeof(float);
    double max_chunks_for_mem = (double)memory_limit_bytes / chunk_mem;
    if (worst > max_chunks_for_mem) {
        cost *= (worst / max_chunks_for_mem);
    }

    // 存储膨胀惩罚（保留原有的红线过滤逻辑）
    if (storage_ratio > MAX_STORAGE_RATIO) {
        cost = std::numeric_limits<double>::max();
    }

    return cost;
}
```

**评分特性**：

| 数据形状 | 最优 chunk | 原因 |
|---------|-----------|------|
| 800×2404×2500 | 64×64×64 | 对称数据，均衡最优 |
| 10000×100×100 | 512×64×64 | X 极长，大 cx 使 nc_x=20，Y/Z slice 仅需 40 vs 314 |
| 100×100×10000 | 64×64×512 | Z 极长，大 cz 使 nc_z=20，X/Y slice 仅需 40 vs 314 |
| 512×512×512 | 64×64×64 | 完美对称 |

### 5.3 搜索空间优化

去掉 cx>cy>cz 后，搜索空间从 216 种增加到 216×6=1296（含排列），但很多排列是等价的。优化：

1. **跳过等价排列**：若 cx=cy=cz，只需 1 种；若 cx=cy≠cz，只需 3 种
2. **提前剪枝**：体积约束 + 存储红线仍先过滤
3. **性能**：1296 种纯算术评估 < 1ms，无需优化

### 5.4 实施清单

- [ ] 修改 `find_optimal_chunk_shape()`：去掉 cx>cy>cz 排序约束
- [ ] 实现新评分函数 `evaluate_shape()`：minimax + 绝对吞吐加权 + 内存约束
- [ ] 修改搜索循环：遍历所有 `(cx,cy,cz)` 排列组合
- [ ] 保留体积约束过滤（512KB~4MB）和存储红线过滤（1.45x）
- [ ] 新增 `memory_limit_bytes` 参数（默认 2GB，可配置）
- [ ] 更新 `tests/test_chunk_optimizer.cpp`：
  - [ ] 验证 800×2404×2500 选出接近均衡的形状
  - [ ] 验证 10000×100×100 选出 cx 较大的形状
  - [ ] 验证 100×100×10000 选出 cz 较大的形状
  - [ ] 验证 512³ 仍选 64³
  - [ ] 验证存储红线仍生效
  - [ ] 验证内存约束惩罚生效

---

## 六、Phase 4：集成与 CLI

### 6.1 修改文件

- `bench_e2e.cpp` — 切换到 C3DRMmapReader
- `src/main.cpp` — 支持 mmap 模式 + 新优化器
- `include/args.h` + `src/args.cpp` — 新增 `--max-mem` 参数
- `src/profiler.cpp` — 适配 MmapReader

### 6.2 CLI 参数变更

| 参数 | 变更 | 说明 |
|------|------|------|
| `--cache-size <MB>` | **移除** | 不再需要应用层缓存 |
| `--max-mem <MB>` | **新增** | 最大内存使用量（默认 2048），控制 mmap 窗口 + 优化器内存约束 |

### 6.3 bench_e2e.cpp 改动

```cpp
// 旧:
//   C3DRCacheReader reader(cache_bytes);
//   reader.open(args.c3dr_path);

// 新:
//   C3DRMmapReader reader;
//   reader.open(args.c3dr_path, max_mem_bytes);

// 统计输出变更:
//   旧: 缓存命中率 (hit / miss)
//   新: 预取命中率 (可选, 通过对比 prefetched 和实际访问区间估算)
```

### 6.4 main.cpp 流程变更

```
1. 解析 CLI 参数（含 --max-mem）
2. RawFileReader.open() 流式校验
3. find_optimal_chunk_shape(dim_x, dim_y, dim_z, memory_limit)
     ↑ 传入 memory_limit，优化器考虑内存约束
4. write_c3dr_file_stream() 落盘（不变）
5. --benchmark:
     C3DRMmapReader reader;
     reader.open(output_path, max_mem_bytes);
     benchmark_slices(reader);
```

### 6.5 profiler.cpp 适配

```cpp
// 旧: benchmark_slices_cache(C3DRCacheReader& reader, ...)
// 新: benchmark_slices_mmap(C3DRMmapReader& reader, ...)
//     接口一致，内部使用 reader.read_x/y/z_slice()
```

### 6.6 实施清单

- [ ] 修改 `args.h/args.cpp`：移除 `--cache-size`，新增 `--max-mem`（默认 2048 MB）
- [ ] 修改 `bench_e2e.cpp`：切换到 `C3DRMmapReader`，移除 cache 相关统计
- [ ] 修改 `main.cpp`：使用新优化器参数 + `C3DRMmapReader`
- [ ] 修改 `profiler.cpp`：适配 `C3DRMmapReader`
- [ ] 修改 `CMakeLists.txt`：添加新源文件，移除 `cache_reader.cpp`（可先保留不删）
- [ ] 端到端测试：运行 `contest_bench.py`，对比新旧性能

---

## 七、Phase 5：验证与回归

### 7.1 验证清单

- [ ] **精度回归**：输出文件 `verify.py --compare` 验证精度 < 1‰
- [ ] **存储膨胀**：确认 .c3dr 文件 ≤ 原始 1.45 倍
- [ ] **内存限制**：`--max-mem 2048` 下，进程内存 ≤ 2GB（用任务管理器 / `GetProcessMemoryInfo` 验证）
- [ ] **跨平台**：Linux 下编译运行（`mmap` / `madvise` / `msync`）
- [ ] **性能对比**：

```
预期基准测试结果 (800×2404×2500, max-mem 2048):

========== 测试结果 ==========
随机切片 (avg/slice):
  X 轴:  ~70-100 ms    (vs 314 ms, 3-4x 加速)
  Y 轴:  ~50-80 ms     (vs 683 ms, 8-13x 加速)
  Z 轴:  ~40-55 ms     (vs 77 ms, 1.4-2x 加速)
连续切片 (avg/slice):
  X 轴:  ~20-35 ms     (vs 96 ms, 3-5x 加速)
  Y 轴:  ~15-30 ms     (vs 99 ms, 3-6x 加速)
  Z 轴:  ~10-18 ms     (vs 22 ms, 1.2-2x 加速)
----------------------------------------
综合平均耗时:     ~35-60 ms  (vs 215 ms, 4-6x 加速)
========================================
```

### 7.2 TB 级数据注意事项

当数据达到 TB 级时需额外关注：

1. **大页支持**：Windows `FILE_FLAG_LARGE_PAGES`（2MB 页），减少 TLB miss
2. **异步预取线程**：后台线程预取下一批 chunk，主线程处理当前 batch → 流水线化
3. **Index 分区加载**：百万级 chunk 的 index 表按需加载，避免一次性读入上百 MB 索引
4. **滑动窗口 remap 频率**：TB 级文件下窗口 2GB 仍能覆盖 ~4000 个 512KB chunk，足够单次切片使用

这些为后续优化项，当前 18-50GB 级别不需要。

---

## 八、文件变更汇总

| # | 文件 | 操作 | 阶段 |
|---|------|------|------|
| 1 | `include/platform_mmap.h` | **新增** | Phase 1 |
| 2 | `src/platform_mmap.cpp` | **新增** | Phase 1 |
| 3 | `include/mmap_reader.h` | **新增** | Phase 2 |
| 4 | `src/mmap_reader.cpp` | **新增** | Phase 2 |
| 5 | `src/chunk_optimizer.cpp` | 修改（去掉 cx>cy>cz + 新评分函数） | Phase 3 |
| 6 | `include/common.h` | 修改（如需调整常量） | Phase 3 |
| 7 | `tests/test_chunk_optimizer.cpp` | 修改（新增验证用例） | Phase 3 |
| 8 | `bench_e2e.cpp` | 修改（切换 C3DRMmapReader） | Phase 4 |
| 9 | `src/main.cpp` | 修改（mmap 模式 + 新优化器参数） | Phase 4 |
| 10 | `include/args.h` / `src/args.cpp` | 修改（--max-mem 替换 --cache-size） | Phase 4 |
| 11 | `src/profiler.cpp` / `include/profiler.h` | 修改（适配 MmapReader） | Phase 4 |
| 12 | `CMakeLists.txt` | 修改（添加新源文件） | Phase 4 |
| 13 | `tests/test_mmap_reader.cpp` | **新增** | Phase 5 |
| 14 | `include/cache_reader.h` / `src/cache_reader.cpp` | 废弃（保留不删，待确认后清理） | Phase 5 |

---

## 九、总实施清单

### Phase 1：跨平台 mmap 抽象层
- [ ] 创建 `include/platform_mmap.h`，定义 `MmapFile` 类接口
- [ ] 实现 Windows 平台：`CreateFileW` + `CreateFileMapping` + `MapViewOfFile`
- [ ] 实现 Linux 平台：`open` + `mmap` + `munmap`
- [ ] 实现滑动窗口映射（`ensure_mapped` + `map_window` + `unmap_current`）
- [ ] 实现预取（`prefetch`：`PrefetchVirtualMemory` / `madvise`）
- [ ] 实现页面释放（`release_region`：`DiscardVirtualMemory` / `MADV_DONTNEED`）
- [ ] 实现刷盘（`flush`：`FlushViewOfFile` / `msync`）
- [ ] 实现移动语义（`MmapFile(MmapFile&&)`）
- [ ] 在 `CMakeLists.txt` 添加 `platform_mmap.cpp` 源文件
- [ ] 编写单元测试：打开/关闭/基本映射/预取/窗口滑动

### Phase 2：C3DRMmapReader
- [ ] 创建 `include/mmap_reader.h`，定义 `C3DRMmapReader` 类接口
- [ ] 实现 `open()`：读 Header + IndexTable + mmap 映射
- [ ] 实现 `read_x_slice()`：批量预取 + memcpy 拼装
- [ ] 实现 `read_y_slice()`
- [ ] 实现 `read_z_slice()`
- [ ] 实现 `write_x/y/z_slice()`：可写映射 + memcpy
- [ ] 实现 `flush()`：刷盘脏页
- [ ] 实现 `prefetch_slice_chunks()`：合并范围批量预取
- [ ] 实现 `maybe_prefetch()`：连续模式检测 + 预调度
- [ ] 实现 `compute_slice_chunks()`：按轴计算所需 chunk 列表
- [ ] 在 `CMakeLists.txt` 添加 `mmap_reader.cpp` 源文件
- [ ] 编写单元测试

### Phase 3：修正 Chunk 优化器
- [ ] 修改 `find_optimal_chunk_shape()`：去掉 cx>cy>cz 约束
- [ ] 实现新评分函数 `evaluate_shape()`：minimax + 绝对吞吐加权 + 内存约束
- [ ] 修改搜索循环：遍历所有 `(cx,cy,cz)` 排列组合
- [ ] 新增 `memory_limit_bytes` 参数（默认 2GB）
- [ ] 更新测试用例

### Phase 4：集成与 CLI
- [ ] 修改 `args.h/args.cpp`：移除 `--cache-size`，新增 `--max-mem`
- [ ] 修改 `bench_e2e.cpp`：切换到 `C3DRMmapReader`
- [ ] 修改 `main.cpp`：新优化器参数 + `C3DRMmapReader`
- [ ] 修改 `profiler.cpp/h`：适配 `C3DRMmapReader`
- [ ] 修改 `CMakeLists.txt`：添加新源文件

### Phase 5：验证与回归
- [ ] 精度回归验证（< 1‰）
- [ ] 存储膨胀验证（≤ 1.45x）
- [ ] 内存限制验证（≤ 配置上限）
- [ ] Linux 跨平台编译运行
- [ ] 性能基准测试对比
- [ ] 废弃 `cache_reader.h/cpp`（确认功能无缺失后清理）
