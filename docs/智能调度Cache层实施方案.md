# 智能调度 Cache 层实施方案（读写版）

> **目标**：构建 Chunk 级 LRU 缓存层，支持读写切面；读命中零拷贝，写就地修改（use_count==1）或 Copy-on-Write（有读者持有）；脏块延迟落盘（Write-Back）；连续模式预取。

---

## 一、架构总览

```
用户
  │ read_x_slice(x)   ← 命中零拷贝，未命中磁盘+缓存
  │ write_x_slice(x, data)  ← 脏块就地/COW + 延迟 flush
  │ flush()            ← 所有脏块落盘
  ▼
C3DRCacheReader（继承 C3DRReader）
  │
  ├─ [Read] read_chunk_by_index(idx) override
  │   ├─ 命中(非脏) → shared_ptr<const> 零拷贝 + LRU 前移
  │   ├─ 命中(脏)   → 返回脏数据 shared_ptr<const>（与脏缓冲共享所有权）
  │   └─ 未命中     → 父类磁盘 I/O → 插入缓存
  │
  ├─ [Write] write_x/y/z_slice → 逐 chunk 修改
  │   ├─ chunk 在缓存 + use_count==1 → 就地修改（零拷贝写，最快）
  │   ├─ chunk 在缓存 + use_count>1  → COW：拷贝→修改→替换 shared_ptr
  │   └─ chunk 不在缓存 → 从磁盘加载→插入缓存→修改
  │   全部标记 is_dirty=true
  │
  ├─ [Flush] 脏块 → FSEEK64+fwrite → is_dirty=false
  │
  ├─ [Evict] LRU 尾淘汰 → 脏块先 flush → 再移除
  │
  └─ maybe_prefetch()（同原方案，仅读路径触发）
```

### 核心设计决策

| 决策项 | 选择 | 理由 |
|--------|------|------|
| 写策略 | **Write-Back + COW 回退** | use_count==1 时就地修改（零拷贝写），有读者时 COW（安全）；脏块批量写回 |
| 缓存粒度 | Chunk 级 | 一个 chunk 可被 X/Y/Z 三轴切面复用，无冗余；512KB/块，2GB 可容 ~4096 块 |
| 淘汰策略 | LRU + 预取 | 连续切片天然局部性好；LRU 实现简单高效 |
| 多线程模型 | shared_mutex + io_mutex | 命中走 shared_lock（多线程并发），未命中/写走 unique_lock |
| 预取 hook | 方法隐藏 | C3DRCacheReader 定义同名 read_x/y/z_slice，调用父类后追加预取 |
| 数据管理 | shared_ptr\<const vector\<float\>\> | 缓存存储 const；写时 COW 替换整个 shared_ptr，旧 ptr 读者安全 |
| 脏块淘汰 | 先 flush 再淘汰 | 数据安全 |
| 文件模式 | `r+b` | C3DRCacheReader::open() 重新以读写模式打开 |
| 缓存大小 | 构造时确定，运行期不变 | 改大小需重新构造 C3DRCacheReader |

---

## 二、线程安全模型

```
                    m_cache_mutex (shared_mutex)
                    ┌──────────────────────────┐
 读切片 ──────────→ │ shared_lock              │ → 获取 shared_ptr<const>
                    │ use_count++ 自动          │ → 释放锁后安全读数据
                    └──────────────────────────┘

                    m_cache_mutex (shared_mutex)  +  COW
 写切片 ──────────→ │ unique_lock               │
                    │ 检查 use_count(data):       │
                    │   ==1 → 就地修改（快路径）   │
                    │   >1  → 拷贝→改→swap ptr    │
                    │ 标记 is_dirty=true          │
                    └──────────────────────────┘

                    m_cache_mutex + m_io_mutex
 Flush ──────────→ │ unique_lock(cache)          │
                    │ lock(io) → FSEEK64+fwrite   │
                    │ is_dirty = false            │
                    └──────────────────────────┘
```

**关键安全保证**：

- `shared_ptr<const>` 保证返回给读者的数据**逻辑不可变**：写时要么就地改（use_count==1，无其他持有人）要么替换整个 shared_ptr（旧 ptr 仍有效，读者拿到的是旧快照）
- 就地修改时 `unique_lock(m_cache_mutex)` 保证：修改期间无新读者获取该 chunk 的 shared_ptr，且 use_count 已确认为 1
- 脏块淘汰：先 flush 再从 cache 移除

---

## 三、内存分析（以 18GB 数据为例）

数据维度 800×2404×2500，最优形状 32×256×16，单 chunk = 32×256×16×4 = **512 KB**。

### 读场景

| 切面方向 | 所需 chunk 数 | 所需内存 | 2GB 缓存能否容纳 |
|---------|:------------:|---------:|:---:|
| X 切面 | nc_y × nc_z = 10 × 157 = 1570 | ~785 MB | OK |
| Y 切面 | nc_x × nc_z = 25 × 157 = 3925 | ~1.93 GB | LRU 自然淘汰处理 |
| Z 切面 | nc_x × nc_y = 25 × 10 = 250 | ~125 MB | OK |

**Cache 核心价值**：连续 X 切面 x=0..31 全部落在 ix=0 同一列 1570 个 chunk 上，第 1 个之后 31 个切面 **100% 命中**。

### 写场景额外开销

写操作**不额外占用内存**——修改直接发生在缓存条目的 `shared_ptr` 所指数据上（就地修改）或替换 `shared_ptr`（COW，旧数据随读者释放自动回收）。

唯一额外开销：`is_dirty` 标志（1 bool/entry）。

### COW 开销分析

| 场景 | use_count 期望值 | 路径 | 开销 |
|------|:---------------:|------|------|
| 单线程顺序读写 | 1 | 就地修改 | 0（零拷贝写） |
| 切片组装期间写 | >1 | COW | 1×512KB 拷贝/chunk |
| 预取+写同时 | 1~2 | 多数就地 | 极少 COW |

**最坏情况**：写整个 X 切面（1570 chunk），每个 chunk 需 COW → 1570 × 512KB = 785MB 临时拷贝。但实际场景中切面组装完成后 `use_count` 即降为 1，不会触发 COW。

---

## 四、Phase 1：重构 C3DRReader（零功能变化）

### 4.1 目标

让 `read_chunk_by_index` 可被子类覆写，返回 `shared_ptr` 使缓存层可拦截所有 chunk 访问；`m_file` 提升为 protected 供子类重开为 `r+b`。

### 4.2 `include/chunk_io.h` 改动

| # | 改动点 | 改动前 | 改动后 |
|---|--------|--------|--------|
| 1 | 析构函数 | `~C3DRReader()` | `virtual ~C3DRReader()` |
| 2 | 新增 include | — | `#include <memory>` |
| 3 | `read_chunk_by_index` 可见性 | `private` | `protected` |
| 4 | `read_chunk_by_index` 虚化 | `std::vector<float>` 返回 | `virtual std::shared_ptr<const std::vector<float>>` 返回 |
| 5 | 新增成员 | — | `size_t m_chunk_bytes;` |
| 6 | 新增 getter | — | `size_t chunk_bytes() const` |
| 7 | **`m_file` 可见性** | `private` | `protected` |

#### 改动后完整类声明

```cpp
class C3DRReader {
public:
    C3DRReader();
    virtual ~C3DRReader();

    C3DRReader(const C3DRReader&) = delete;
    C3DRReader& operator=(const C3DRReader&) = delete;

    bool open(const std::string& path);
    void close();
    bool is_open() const { return m_file != nullptr; }

    // 只读访问器（与现有一致）
    const C3DRHeader&   header()        const { return m_header; }
    ChunkShape          chunk_shape()   const { return m_header.chunk_shape; }
    uint64_t            dim_x()         const { return m_header.dim_x; }
    uint64_t            dim_y()         const { return m_header.dim_y; }
    uint64_t            dim_z()         const { return m_header.dim_z; }
    uint32_t            num_chunks_x()  const { return m_nc_x; }
    uint32_t            num_chunks_y()  const { return m_nc_y; }
    uint32_t            num_chunks_z()  const { return m_nc_z; }
    size_t              chunk_bytes()   const { return m_chunk_bytes; }

    // 各方向填充后总尺寸
    uint64_t padded_dim_x() const;
    uint64_t padded_dim_y() const;
    uint64_t padded_dim_z() const;

    // 切面读取（与现有一致）
    std::vector<float> read_x_slice(uint32_t x);
    std::vector<float> read_y_slice(uint32_t y);
    std::vector<float> read_z_slice(uint32_t z);
    std::vector<float> read_chunk(uint32_t ix, uint32_t iy, uint32_t iz);

protected:
    // 子类可覆写：缓存层拦截此入口
    virtual std::shared_ptr<const std::vector<float>>
        read_chunk_by_index(uint64_t chunk_idx);

    C3DRHeader                  m_header;
    std::vector<C3DRIndexEntry> m_index;
    FILE*                       m_file;        // protected: 子类可重开为 r+b
    uint32_t                    m_nc_x;
    uint32_t                    m_nc_y;
    uint32_t                    m_nc_z;
    size_t                      m_chunk_bytes;  // cx*cy*cz*sizeof(float)
};
```

### 4.3 `src/chunk_io.cpp` 改动

#### 4.3.1 `read_chunk_by_index` 返回值适配

```cpp
// 改动前：
std::vector<float> C3DRReader::read_chunk_by_index(uint64_t chunk_idx) {
    if (chunk_idx >= m_index.size()) return {};
    uint64_t offset = m_index[chunk_idx].offset;
    size_t cx = ..., cy = ..., cz = ...;
    size_t total = cx * cy * cz;
    std::vector<float> buf(total, 0.0f);
    if (FSEEK64(m_file, static_cast<int64_t>(offset), SEEK_SET) != 0) return buf;
    fread(buf.data(), sizeof(float), total, m_file);
    return buf;
}

// 改动后：
std::shared_ptr<const std::vector<float>>
C3DRReader::read_chunk_by_index(uint64_t chunk_idx) {
    size_t total = m_chunk_bytes / sizeof(float);
    auto buf = std::make_shared<std::vector<float>>(total, 0.0f);
    if (chunk_idx >= m_index.size()) return buf;
    uint64_t offset = m_index[chunk_idx].offset;
    if (FSEEK64(m_file, static_cast<int64_t>(offset), SEEK_SET) != 0) return buf;
    fread(buf->data(), sizeof(float), total, m_file);
    return buf;
}
```

#### 4.3.2 `read_chunk(ix,iy,iz)` 解引用

```cpp
// 改动前：
std::vector<float> C3DRReader::read_chunk(uint32_t ix, uint32_t iy, uint32_t iz) {
    if (ix >= m_nc_x || iy >= m_nc_y || iz >= m_nc_z) return {};
    uint64_t idx = ...;
    return read_chunk_by_index(idx);
}

// 改动后：
std::vector<float> C3DRReader::read_chunk(uint32_t ix, uint32_t iy, uint32_t iz) {
    if (ix >= m_nc_x || iy >= m_nc_y || iz >= m_nc_z) return {};
    uint64_t idx = ...;
    auto ptr = read_chunk_by_index(idx);
    return ptr ? *ptr : std::vector<float>();
}
```

#### 4.3.3 `read_x_slice` 适配

```cpp
// 逐处替换（read_y_slice / read_z_slice 同理）：

// 改动前                          改动后
auto chunk = read_chunk_by_index(chunk_idx);    // 不变
if (chunk.empty()) continue;                    → if (!chunk || chunk->empty()) continue;
const float* src = chunk.data() + ...;          → const float* src = chunk->data() + ...;
chunk[expr]                                      → (*chunk)[expr]
```

#### 4.3.4 `open()` 初始化 `m_chunk_bytes`

```cpp
// 在 open() 中读取 header 并计算 m_nc_x/y/z 之后：
m_chunk_bytes = static_cast<size_t>(m_header.chunk_shape.cx)
              * m_header.chunk_shape.cy
              * m_header.chunk_shape.cz
              * sizeof(float);
```

### 4.4 Phase 1 验证

`cmake --build build && ctest --test-dir build` — 全部通过，零功能变化。

---

## 五、Phase 2：C3DRCacheReader 读写核心

### 5.1 `include/cache_reader.h`

```cpp
#pragma once

#include "chunk_io.h"

#include <atomic>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

class C3DRCacheReader : public C3DRReader {
public:
    // cache_size_bytes: 缓存容量（字节），默认 2GB，最小 2GB
    explicit C3DRCacheReader(size_t cache_size_bytes = 2ULL * 1024 * 1024 * 1024);
    ~C3DRCacheReader() override;

    // 打开/关闭 .c3dr 文件
    bool open(const std::string& path);
    void close();

    // ── 读切面（隐藏基类方法，追加预取逻辑） ──────────────
    std::vector<float> read_x_slice(uint32_t x);
    std::vector<float> read_y_slice(uint32_t y);
    std::vector<float> read_z_slice(uint32_t z);

    // ── 写切面（修改 → 标脏，延迟落盘） ──────────────────
    void write_x_slice(uint32_t x, const std::vector<float>& data);
    void write_y_slice(uint32_t y, const std::vector<float>& data);
    void write_z_slice(uint32_t z, const std::vector<float>& data);

    // ── Flush（脏块落盘） ───────────────────────────────
    void flush();                    // 所有脏块
    void flush_chunk(uint64_t idx);  // 单块

    // ── 缓存管理 ────────────────────────────────────────
    void clear_cache();

    // ── 统计 ────────────────────────────────────────────
    size_t cache_hits()          const { return m_hits.load(std::memory_order_relaxed); }
    size_t cache_misses()        const { return m_misses.load(std::memory_order_relaxed); }
    size_t cache_memory_usage()  const { return m_current_cache_bytes.load(std::memory_order_relaxed); }
    size_t cache_capacity()      const { return m_max_cache_bytes; }
    size_t dirty_count()         const;

protected:
    // 覆写：缓存查找 + 磁盘加载
    std::shared_ptr<const std::vector<float>>
        read_chunk_by_index(uint64_t chunk_idx) override;

private:
    struct CacheEntry {
        std::shared_ptr<const std::vector<float>> data;
        std::list<uint64_t>::iterator             lru_iter;
        bool                                      is_dirty = false;
    };

    // ── 缓存数据结构 ────────────────────────────────────
    std::unordered_map<uint64_t, CacheEntry> m_cache;
    std::list<uint64_t>                      m_lru_list;  // front=MRU, back=LRU
    std::atomic<size_t>                      m_current_cache_bytes{0};
    size_t                                   m_max_cache_bytes;

    // ── 并发控制 ─────────────────────────────────────────
    mutable std::shared_mutex m_cache_mutex;  // 缓存读写锁
    mutable std::mutex        m_io_mutex;     // C3DRReader FILE* 独占锁

    // ── 统计 ─────────────────────────────────────────────
    std::atomic<size_t> m_hits{0};
    std::atomic<size_t> m_misses{0};

    // ── 预取 ─────────────────────────────────────────────
    struct SliceRequest { int axis; uint32_t position; };
    std::deque<SliceRequest> m_recent_requests;
    static constexpr size_t  PREFETCH_WINDOW = 3;

    // ── 内部方法 ─────────────────────────────────────────
    void evict_if_needed(size_t incoming_bytes);
    void maybe_prefetch(int axis, uint32_t position);
    std::vector<uint64_t> compute_needed_chunks(int axis, uint32_t pos);
    void do_prefetch(const std::vector<uint64_t>& indices);

    // ── 写内部方法 ───────────────────────────────────────
    void write_chunk_in_cache(uint64_t chunk_idx,
                              const std::vector<float>& patch,
                              size_t offset_in_chunk);
};
```

### 5.2 `src/cache_reader.cpp` — 核心算法

#### 5.2.1 生命周期（含 r+b 重开）

```cpp
C3DRCacheReader::C3DRCacheReader(size_t cache_size_bytes)
    : m_max_cache_bytes(cache_size_bytes < 2ULL * 1024 * 1024 * 1024
                        ? 2ULL * 1024 * 1024 * 1024 : cache_size_bytes)
{}

C3DRCacheReader::~C3DRCacheReader() {
    flush();       // 析构前落盘
    clear_cache();
}

bool C3DRCacheReader::open(const std::string& path) {
    clear_cache();
    if (!C3DRReader::open(path)) return false;
    // 关闭只读句柄，重开为读写模式
    fclose(m_file);
    m_file = fopen(path.c_str(), "r+b");
    return m_file != nullptr;
}

void C3DRCacheReader::close() {
    flush();
    clear_cache();
    C3DRReader::close();
}

void C3DRCacheReader::clear_cache() {
    std::unique_lock lock(m_cache_mutex);
    m_cache.clear();
    m_lru_list.clear();
    m_current_cache_bytes.store(0, std::memory_order_relaxed);
    m_recent_requests.clear();
}
```

#### 5.2.2 `read_chunk_by_index` — 读路径（缓存查找 + 磁盘加载）

```
算法：read_chunk_by_index(chunk_idx)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
① shared_lock(m_cache_mutex)
   hit = m_cache.find(chunk_idx)
   if hit:
     m_hits++
     m_lru_list.splice(front, m_lru_list, hit->lru_iter)  // O(1) 移到最前
     return hit->data 的 shared_ptr 拷贝      ← 零分配，仅 atomic inc
   unlock

② m_misses++

③ lock(m_io_mutex)
   disk_data = C3DRReader::read_chunk_by_index(chunk_idx)  ← 磁盘 FSEEK64+fread
   unlock(m_io_mutex)

④ unique_lock(m_cache_mutex)
   Double-check: m_cache.find(chunk_idx)
   if 已加载: return (另一线程先完成了)

   chunk_mem = disk_data->size() * sizeof(float)
   evict_if_needed(chunk_mem)

   m_lru_list.push_front(chunk_idx)
   m_cache[chunk_idx] = { disk_data, m_lru_list.begin(), false /* not dirty */ }
   m_current_cache_bytes += chunk_mem

   return disk_data
```

#### 5.2.3 `write_chunk_in_cache` — 写路径核心

```cpp
void C3DRCacheReader::write_chunk_in_cache(
    uint64_t chunk_idx,
    const std::vector<float>& patch,
    size_t offset_in_chunk)
{
    std::unique_lock lock(m_cache_mutex);

    auto it = m_cache.find(chunk_idx);

    if (it != m_cache.end()) {
        // ---- 命中缓存 ----
        auto& entry = it->second;

        if (entry.data.use_count() == 1) {
            // >>> 快路径：无他人持有，就地修改（零拷贝写） <<<
            // const_cast 安全前提：
            //   1. use_count==1 → 只有缓存持有此 shared_ptr
            //   2. unique_lock → 期间无新读者获取该 ptr
            auto& mutable_data = const_cast<std::vector<float>&>(*entry.data);
            std::memcpy(mutable_data.data() + offset_in_chunk,
                        patch.data(),
                        patch.size() * sizeof(float));
        } else {
            // >>> 慢路径：有读者持有，Copy-on-Write <<<
            auto copy = std::make_shared<std::vector<float>>(*entry.data);
            std::memcpy(copy->data() + offset_in_chunk,
                        patch.data(),
                        patch.size() * sizeof(float));
            entry.data = std::shared_ptr<const std::vector<float>>(std::move(copy));
            // 旧 shared_ptr 的读者仍持有不可变快照，安全
        }

        entry.is_dirty = true;
        m_lru_list.splice(m_lru_list.begin(), m_lru_list, entry.lru_iter);

    } else {
        // ---- 未命中：先加载再修改 ----
        // 释放 cache_mutex，用 io_mutex 读盘
        lock.unlock();
        std::lock_guard io_lock(m_io_mutex);
        auto disk_data = C3DRReader::read_chunk_by_index(chunk_idx);

        lock.lock();
        // Double-check
        auto it2 = m_cache.find(chunk_idx);
        if (it2 != m_cache.end()) {
            // 另一线程已加载，递归处理
            lock.unlock();
            write_chunk_in_cache(chunk_idx, patch, offset_in_chunk);
            return;
        }

        // 拷贝并修改（刚从磁盘加载，use_count==1，直接就地改）
        auto mutable_copy = std::make_shared<std::vector<float>>(*disk_data);
        std::memcpy(mutable_copy->data() + offset_in_chunk,
                    patch.data(),
                    patch.size() * sizeof(float));

        size_t chunk_mem = mutable_copy->size() * sizeof(float);
        evict_if_needed(chunk_mem);

        m_lru_list.push_front(chunk_idx);
        m_cache[chunk_idx] = {
            std::shared_ptr<const std::vector<float>>(std::move(mutable_copy)),
            m_lru_list.begin(),
            true  // is_dirty
        };
        m_current_cache_bytes += chunk_mem;
    }
}
```

#### 5.2.4 `write_x_slice` — 写切面入口

```cpp
void C3DRCacheReader::write_x_slice(uint32_t x, const std::vector<float>& data) {
    if (x >= m_header.dim_x) return;
    if (data.size() != static_cast<size_t>(m_header.dim_y * m_header.dim_z)) return;

    uint32_t ix      = x / static_cast<uint32_t>(m_header.chunk_shape.cx);
    uint32_t local_x = x % static_cast<uint32_t>(m_header.chunk_shape.cx);
    size_t   cy = static_cast<size_t>(m_header.chunk_shape.cy);
    size_t   cz = static_cast<size_t>(m_header.chunk_shape.cz);
    uint64_t dim_y = m_header.dim_y;
    uint64_t dim_z = m_header.dim_z;

    for (uint32_t iy = 0; iy < m_nc_y; ++iy) {
        uint32_t y_start = iy * static_cast<uint32_t>(cy);
        for (uint32_t iz = 0; iz < m_nc_z; ++iz) {
            uint32_t z_start = iz * static_cast<uint32_t>(cz);

            uint64_t chunk_idx = static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z
                               + iz;

            uint32_t y_end = (std::min)(y_start + static_cast<uint32_t>(cy),
                                        static_cast<uint32_t>(dim_y));
            uint32_t z_end = (std::min)(z_start + static_cast<uint32_t>(cz),
                                        static_cast<uint32_t>(dim_z));

            size_t offset_in_chunk = static_cast<size_t>(local_x) * cy * cz;

            // 逐行构建 patch
            size_t y_count = y_end - y_start;
            std::vector<float> patch(y_count * cz, 0.0f);
            for (uint32_t yy = y_start; yy < y_end; ++yy) {
                uint32_t local_y = yy - y_start;
                size_t src_row = static_cast<size_t>(yy) * dim_z + z_start;
                size_t dst_row = static_cast<size_t>(local_y) * cz;
                size_t count = z_end - z_start;
                std::memcpy(patch.data() + dst_row,
                            data.data() + src_row,
                            count * sizeof(float));
            }

            write_chunk_in_cache(chunk_idx, patch, offset_in_chunk);
        }
    }
}
```

> `write_y_slice` / `write_z_slice` 同理，反向映射逻辑与 `read_y_slice` / `read_z_slice` 对称。

#### 5.2.5 `write_y_slice`

```cpp
void C3DRCacheReader::write_y_slice(uint32_t y, const std::vector<float>& data) {
    if (y >= m_header.dim_y) return;
    if (data.size() != static_cast<size_t>(m_header.dim_x * m_header.dim_z)) return;

    uint32_t iy      = y / static_cast<uint32_t>(m_header.chunk_shape.cy);
    uint32_t local_y = y % static_cast<uint32_t>(m_header.chunk_shape.cy);
    size_t   cx = static_cast<size_t>(m_header.chunk_shape.cx);
    size_t   cy = static_cast<size_t>(m_header.chunk_shape.cy);
    size_t   cz = static_cast<size_t>(m_header.chunk_shape.cz);
    uint64_t dim_x = m_header.dim_x;
    uint64_t dim_z = m_header.dim_z;

    for (uint32_t ix = 0; ix < m_nc_x; ++ix) {
        uint32_t x_start = ix * static_cast<uint32_t>(cx);
        for (uint32_t iz = 0; iz < m_nc_z; ++iz) {
            uint32_t z_start = iz * static_cast<uint32_t>(cz);

            uint64_t chunk_idx = static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z
                               + iz;

            uint32_t x_end = (std::min)(x_start + static_cast<uint32_t>(cx),
                                        static_cast<uint32_t>(dim_x));
            uint32_t z_end = (std::min)(z_start + static_cast<uint32_t>(cz),
                                        static_cast<uint32_t>(dim_z));

            // Y 切面在该 chunk 中位于 local_y 行
            // 对于每个 local_x，偏移 = local_x * cy * cz + local_y * cz
            size_t x_count = x_end - x_start;
            std::vector<float> patch(x_count * cz, 0.0f);
            for (uint32_t xx = x_start; xx < x_end; ++xx) {
                uint32_t local_x = xx - x_start;
                size_t src_base = static_cast<size_t>(xx) * dim_z + z_start;
                size_t dst_base = static_cast<size_t>(local_x) * cz;
                size_t count = z_end - z_start;
                for (size_t k = 0; k < count; ++k) {
                    patch[dst_base + k] = data[src_base + k];
                }
            }

            // 每个 local_x 行在 chunk 中的偏移不同，需逐行写入
            for (uint32_t lx = 0; lx < x_count; ++lx) {
                size_t chunk_offset = static_cast<size_t>(lx) * cy * cz
                                    + static_cast<size_t>(local_y) * cz;
                std::vector<float> row_patch(z_end - z_start);
                std::memcpy(row_patch.data(),
                            patch.data() + static_cast<size_t>(lx) * cz,
                            row_patch.size() * sizeof(float));
                write_chunk_in_cache(chunk_idx, row_patch, chunk_offset);
            }
        }
    }
}
```

#### 5.2.6 `write_z_slice`

```cpp
void C3DRCacheReader::write_z_slice(uint32_t z, const std::vector<float>& data) {
    if (z >= m_header.dim_z) return;
    if (data.size() != static_cast<size_t>(m_header.dim_x * m_header.dim_y)) return;

    uint32_t iz      = z / static_cast<uint32_t>(m_header.chunk_shape.cz);
    uint32_t local_z = z % static_cast<uint32_t>(m_header.chunk_shape.cz);
    size_t   cx = static_cast<size_t>(m_header.chunk_shape.cx);
    size_t   cy = static_cast<size_t>(m_header.chunk_shape.cy);
    size_t   cz = static_cast<size_t>(m_header.chunk_shape.cz);
    uint64_t dim_x = m_header.dim_x;
    uint64_t dim_y = m_header.dim_y;

    for (uint32_t ix = 0; ix < m_nc_x; ++ix) {
        uint32_t x_start = ix * static_cast<uint32_t>(cx);
        for (uint32_t iy = 0; iy < m_nc_y; ++iy) {
            uint32_t y_start = iy * static_cast<uint32_t>(cy);

            uint64_t chunk_idx = static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z
                               + iz;

            uint32_t x_end = (std::min)(x_start + static_cast<uint32_t>(cx),
                                        static_cast<uint32_t>(dim_x));
            uint32_t y_end = (std::min)(y_start + static_cast<uint32_t>(cy),
                                        static_cast<uint32_t>(dim_y));

            // Z 切面在该 chunk 中：每个 (local_x, local_y) 间隔 cz
            // 需逐元素写入
            for (uint32_t xx = x_start; xx < x_end; ++xx) {
                uint32_t local_x = xx - x_start;
                for (uint32_t yy = y_start; yy < y_end; ++yy) {
                    uint32_t local_y = yy - y_start;
                    size_t chunk_offset = static_cast<size_t>(local_x) * cy * cz
                                        + static_cast<size_t>(local_y) * cz
                                        + local_z;
                    std::vector<float> elem = { data[static_cast<size_t>(xx) * dim_y + yy] };
                    write_chunk_in_cache(chunk_idx, elem, chunk_offset);
                }
            }
        }
    }
}
```

> **优化提示**：`write_z_slice` 逐元素调用 `write_chunk_in_cache` 开销大（每元素一次 unique_lock），实际实现可批量收集同一 chunk 的所有修改后一次性写入，减少锁开销。

#### 5.2.7 `flush` — 脏块落盘

```cpp
void C3DRCacheReader::flush() {
    std::unique_lock cache_lock(m_cache_mutex);
    std::lock_guard io_lock(m_io_mutex);

    for (auto& [idx, entry] : m_cache) {
        if (!entry.is_dirty) continue;

        uint64_t offset = m_index[idx].offset;
        size_t total = entry.data->size();

        if (FSEEK64(m_file, static_cast<int64_t>(offset), SEEK_SET) != 0) continue;
        fwrite(entry.data->data(), sizeof(float), total, m_file);

        entry.is_dirty = false;
    }
}

void C3DRCacheReader::flush_chunk(uint64_t chunk_idx) {
    std::unique_lock cache_lock(m_cache_mutex);
    auto it = m_cache.find(chunk_idx);
    if (it == m_cache.end() || !it->second.is_dirty) return;

    std::lock_guard io_lock(m_io_mutex);
    uint64_t offset = m_index[chunk_idx].offset;
    size_t total = it->second.data->size();

    if (FSEEK64(m_file, static_cast<int64_t>(offset), SEEK_SET) == 0) {
        fwrite(it->second.data->data(), sizeof(float), total, m_file);
    }
    it->second.is_dirty = false;
}
```

#### 5.2.8 `evict_if_needed` — LRU 淘汰（脏块先 flush）

```
算法：evict_if_needed(incoming_bytes)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
前提：调用方已持有 unique_lock(m_cache_mutex)

while m_current_cache_bytes + incoming_bytes > m_max_cache_bytes
      AND !m_lru_list.empty():
    victim_idx = m_lru_list.back()
    auto& victim = m_cache[victim_idx]

    if victim.is_dirty:
        // 脏块：先落盘再淘汰
        // 注意：调用方已持有 unique_lock(m_cache_mutex) + 可能持有 m_io_mutex
        // 在 flush() 路径中调用 evict_if_needed 时 m_io_mutex 已锁定
        // 在 read/write 路径中调用时尚未持有 m_io_mutex
        FSEEK64 + fwrite victim.data → 落盘

    victim_bytes = victim.data->size() * sizeof(float)
    m_current_cache_bytes -= victim_bytes
    m_lru_list.pop_back()
    m_cache.erase(victim_idx)
```

> **安全性**：淘汰的 shared_ptr 引用计数减 1。若调用方仍持有该 chunk 的 shared_ptr 拷贝，数据不会被释放，直至调用方使用完毕。

#### 5.2.9 读切面 + 预取（隐藏基类）

```cpp
std::vector<float> C3DRCacheReader::read_x_slice(uint32_t x) {
    auto result = C3DRReader::read_x_slice(x);  // 走 cache 版 read_chunk_by_index
    maybe_prefetch(0, x);
    return result;
}

std::vector<float> C3DRCacheReader::read_y_slice(uint32_t y) {
    auto result = C3DRReader::read_y_slice(y);
    maybe_prefetch(1, y);
    return result;
}

std::vector<float> C3DRCacheReader::read_z_slice(uint32_t z) {
    auto result = C3DRReader::read_z_slice(z);
    maybe_prefetch(2, z);
    return result;
}
```

#### 5.2.10 `maybe_prefetch` — 连续模式检测 + 预取

```
算法：maybe_prefetch(axis, position)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
1. 记录到 m_recent_requests（保留最近 PREFETCH_WINDOW=3 条）
2. 检查连续模式：同轴 × 步长一致(+1 或 -1) × 连续 3 次
3. 若未检测到连续模式 → return
4. 计算预测位置 next_pos = position + delta
5. 检测是否接近/跨越 chunk 边界：
   X 轴：next_pos % cx == 0（即将进入新 ix 列）
   Y 轴：next_pos % cy == 0
   Z 轴：next_pos % cz == 0
6. 若跨越边界：
   needed = compute_needed_chunks(axis, next_pos)
   减去已在缓存中的 → do_prefetch(剩余)
```

#### 5.2.11 `compute_needed_chunks`

```cpp
std::vector<uint64_t> C3DRCacheReader::compute_needed_chunks(int axis, uint32_t pos) {
    std::vector<uint64_t> needed;
    auto cx = m_header.chunk_shape.cx;
    auto cy = m_header.chunk_shape.cy;
    auto cz = m_header.chunk_shape.cz;

    if (axis == 0) {  // X slice
        uint32_t ix = pos / cx;
        for (uint32_t iy = 0; iy < m_nc_y; ++iy)
            for (uint32_t iz = 0; iz < m_nc_z; ++iz)
                needed.push_back(static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z + iz);
    } else if (axis == 1) {  // Y slice
        uint32_t iy = pos / cy;
        for (uint32_t ix = 0; ix < m_nc_x; ++ix)
            for (uint32_t iz = 0; iz < m_nc_z; ++iz)
                needed.push_back(static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z + iz);
    } else {  // Z slice
        uint32_t iz = pos / cz;
        for (uint32_t ix = 0; ix < m_nc_x; ++ix)
            for (uint32_t iy = 0; iy < m_nc_y; ++iy)
                needed.push_back(static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z + iz);
    }
    return needed;
}
```

#### 5.2.12 `do_prefetch`

```cpp
void C3DRCacheReader::do_prefetch(const std::vector<uint64_t>& indices) {
    std::vector<uint64_t> to_load;
    {
        std::shared_lock lock(m_cache_mutex);
        for (uint64_t idx : indices) {
            if (m_cache.find(idx) == m_cache.end()) {
                to_load.push_back(idx);
            }
        }
    }
    for (uint64_t idx : to_load) {
        read_chunk_by_index(idx);
    }
}
```

---

## 六、Phase 3：CLI 集成

### 6.1 `include/args.h` + `src/args.cpp`

```cpp
// args.h 新增字段
uint64_t cache_size_mb = 2048;   // 默认 2GB，最小 2048

// args.cpp 新增参数
// --cache-size <N>    内存缓存大小(MB)，最小 2048，默认 2048
```

### 6.2 `src/main.cpp`

benchmark 路径改用 C3DRCacheReader，支持读写测试：

```cpp
#include "cache_reader.h"

// 在 benchmark 分支中：
size_t cache_bytes = args.cache_size_mb * 1024ULL * 1024ULL;
C3DRCacheReader cache_reader(cache_bytes);
cache_reader.open(tmp_path);
// ... 读取 + 写入 + flush 测试
// 输出命中/未命中/脏块统计
```

### 6.3 `include/profiler.h` + `src/profiler.cpp`

`benchmark_slices` 加 `cache_size` 参数，内部用 C3DRCacheReader，可选执行 write-back 测试。

---

## 七、Phase 4：测试

### `tests/test_cache_reader.cpp`

| # | 测试用例 | 验证点 |
|---|---------|--------|
| 1 | 生命周期 open/close/flush | 正常开关，脏块 flush 后文件一致 |
| 2 | 读切面正确性 X/Y/Z | 与 C3DRReader 逐元素比对 |
| 3 | 命中/未命中计数 | 首次 miss，二次 hit |
| 4 | LRU 淘汰正确性 | 填满→读新→最旧淘汰 |
| 5 | 内存上限强制 | `cache_memory_usage() <= cache_capacity()` |
| 6 | **写切面正确性** | write_x_slice → flush → 重新 open 用 C3DRReader 读出，逐元素比对 |
| 7 | **写后读一致性** | write_x_slice → 不 flush → cache_reader 读回，数据 matches |
| 8 | **COW 安全性** | 获取 shared_ptr → write 同 chunk → 原 ptr 数据不变 |
| 9 | **脏块淘汰** | 脏块被 LRU 淘汰 → 自动 flush → 重新加载，数据 matches |
| 10 | **flush 幂等** | 连续 flush 两次 → 无异常，数据不变 |
| 11 | 预取触发 | 连续 3+ 同轴切片 → 预取命中 |
| 12 | 不同缓存大小 | 2GB / 4GB |

---

## 八、文件改动汇总

| # | 文件 | 操作 | 阶段 |
|---|------|------|------|
| 1 | `include/chunk_io.h` | 修改 | Phase 1 |
| 2 | `src/chunk_io.cpp` | 修改 | Phase 1 |
| 3 | `include/cache_reader.h` | **新增** | Phase 2 |
| 4 | `src/cache_reader.cpp` | **新增** | Phase 2 |
| 5 | `include/args.h` | 修改 | Phase 3 |
| 6 | `src/args.cpp` | 修改 | Phase 3 |
| 7 | `src/main.cpp` | 修改 | Phase 3 |
| 8 | `include/profiler.h` | 修改 | Phase 3 |
| 9 | `src/profiler.cpp` | 修改 | Phase 3 |
| 10 | `CMakeLists.txt` | 修改 | Phase 4 |
| 11 | `tests/test_cache_reader.cpp` | **新增** | Phase 4 |

---

## 九、TODO List

### Phase 1：重构 C3DRReader

- [ ] **TODO 1.1** — `include/chunk_io.h`：`~C3DRReader()` → `virtual ~C3DRReader()`
- [ ] **TODO 1.2** — `include/chunk_io.h`：新增 `#include <memory>`
- [ ] **TODO 1.3** — `include/chunk_io.h`：`read_chunk_by_index` → `protected virtual`，返回 `shared_ptr<const vector<float>>`
- [ ] **TODO 1.4** — `include/chunk_io.h`：`m_file` 从 `private` → `protected`
- [ ] **TODO 1.5** — `include/chunk_io.h`：新增 `m_chunk_bytes` + `chunk_bytes()` getter
- [ ] **TODO 1.6** — `src/chunk_io.cpp`：`open()` 初始化 `m_chunk_bytes`
- [ ] **TODO 1.7** — `src/chunk_io.cpp`：`read_chunk_by_index` 适配 shared_ptr
- [ ] **TODO 1.8** — `src/chunk_io.cpp`：`read_chunk` 解引用 shared_ptr
- [ ] **TODO 1.9** — `src/chunk_io.cpp`：`read_x_slice` 适配 `chunk->` / `(*chunk)[]`
- [ ] **TODO 1.10** — `src/chunk_io.cpp`：`read_y_slice` 同上
- [ ] **TODO 1.11** — `src/chunk_io.cpp`：`read_z_slice` 同上
- [ ] **TODO 1.12** — 编译验证 + 全量测试通过

### Phase 2：C3DRCacheReader 读写核心

- [ ] **TODO 2.1** — 新增 `include/cache_reader.h`：完整类声明（含 CacheEntry、is_dirty、写接口、flush）
- [ ] **TODO 2.2** — 新增 `src/cache_reader.cpp`：生命周期（含 r+b 重开 + 析构 flush）
- [ ] **TODO 2.3** — 实现 `read_chunk_by_index` 覆写：缓存查找 → 磁盘加载 → 插入缓存 → LRU
- [ ] **TODO 2.4** — 实现 `evict_if_needed`：脏块先 flush 再淘汰
- [ ] **TODO 2.5** — 实现 `write_chunk_in_cache`（use_count==1 就地修改 → COW 回退）
- [ ] **TODO 2.6** — 实现 `write_x_slice` / `write_y_slice` / `write_z_slice`
- [ ] **TODO 2.7** — 实现 `flush` / `flush_chunk`（脏块落盘）
- [ ] **TODO 2.8** — 实现 `read_x/y/z_slice`（隐藏基类 + maybe_prefetch）
- [ ] **TODO 2.9** — 实现 `maybe_prefetch` / `compute_needed_chunks` / `do_prefetch`
- [ ] **TODO 2.10** — 编译验证

### Phase 3：CLI 集成

- [ ] **TODO 3.1** — `include/args.h` 新增 `cache_size_mb`
- [ ] **TODO 3.2** — `src/args.cpp` 新增 `--cache-size`
- [ ] **TODO 3.3** — `src/main.cpp` benchmark 改用 C3DRCacheReader
- [ ] **TODO 3.4** — `src/main.cpp` benchmark 结束后输出 cache 统计
- [ ] **TODO 3.5** — `include/profiler.h` / `src/profiler.cpp` 增加缓存读写测试
- [ ] **TODO 3.6** — 编译验证

### Phase 4：测试

- [ ] **TODO 4.1** — 测试 1~5：生命周期 + 读正确性 + 命中计数 + LRU + 内存上限
- [ ] **TODO 4.2** — 测试 6~10：写正确性 + 写后读 + COW 安全 + 脏淘汰 + flush 幂等
- [ ] **TODO 4.3** — 测试 11~12：预取 + 不同缓存大小
- [ ] **TODO 4.4** — `CMakeLists.txt` 添加 `cache_reader.cpp` 源文件 + `test_cache_reader` 测试目标
- [ ] **TODO 4.5** — 全量测试通过
- [ ] **TODO 4.6** — 端到端验证：18GB 数据 benchmark 对比（有/无 cache 的读写耗时）

---

## 十、关键设计约束

1. **Write-Back 延迟落盘**：`write_x/y/z_slice` 只标脏，不触盘；需手动 `flush()` 或析构/淘汰时自动 flush
2. **use_count 就地优化**：`unique_lock` 下 `use_count==1` 保证无其他持有者，`const_cast` 就地修改是安全的
3. **COW 安全保证**：`use_count>1` 时拷贝替换，旧 `shared_ptr<const>` 的读者持有不可变快照，永远安全
4. **脏块淘汰安全**：LRU 淘汰脏块前先 `FSEEK64+fwrite` 落盘，再移除 cache 条目
5. **IO 串行化**：`m_io_mutex` 保证同一时刻只有一个线程做磁盘 I/O（含 flush）
6. **缓存并发**：`m_cache_mutex` 使用 `shared_mutex`，读走 `shared_lock`，写/淘汰走 `unique_lock`
7. **文件模式**：C3DRCacheReader 以 `r+b` 打开文件（读写），C3DRReader 仍以 `rb` 打开（只读）
8. **缓存大小固定**：构造时确定，运行期不变
9. **Y 切面边界**：当 Y 切面所需 chunk 总内存 > cache 容量时，LRU 自然处理（边加载边淘汰），不报错不 warn