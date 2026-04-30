#pragma once

// ============================================================
// C3DR 文件 I/O 接口（阶段四：集成与 Profiling）
// ============================================================
// 定义 .c3dr 文件的写入与读取接口
//   - write_c3dr_file: 将原始 float 数据按最优切块形状分块落盘
//   - C3DRReader:      按需读取 .c3dr 文件中的切面 / 单个块
// ============================================================

#include "chunk_optimizer.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ─── 索引表结构体 ───────────────────────────────────────────────
// 每个 chunk 在文件中对应一条索引记录
struct C3DRIndexEntry {
    uint64_t offset;  // 该 chunk 在文件中的字节偏移（从文件起始算起）
};

// ─── C3DR 文件写入 ──────────────────────────────────────────────
// 将内存中的三维 float 数组按 ChunkShape 分块，写入 .c3dr 文件。
// 返回填充后的文件头（含 index_offset / data_offset 等信息），
// 调用方可将其序列化到文件最开头。
//   path : 输出 .c3dr 文件路径
//   data : 原始 float32 数据（行优先排列，X 最外层、Z 最内层）
//   dim_x/y/z : 原始数据三维尺寸
//   shape : find_optimal_chunk_shape() 返回的最优切块尺寸
C3DRHeader write_c3dr_file(
    const std::string& path,
    const std::vector<float>& data,
    uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
    const ChunkShape& shape);

// ─── C3DR 文件读取器 ────────────────────────────────────────────
// 封装 .c3dr 文件的 Header 解析、索引表加载和按轴切面读取。
class C3DRReader {
public:
    C3DRReader();
    ~C3DRReader();

    // 不允许拷贝 / 移动（持有 FILE* 资源）
    C3DRReader(const C3DRReader&) = delete;
    C3DRReader& operator=(const C3DRReader&) = delete;

    // 打开 .c3dr 文件并解析 Header + IndexTable
    bool open(const std::string& path);

    // 关闭文件，释放资源
    void close();

    // 是否已打开有效文件
    bool is_open() const { return m_file != nullptr; }

    // ── 只读访问器 ──────────────────────────────────────────

    const C3DRHeader&   header()        const { return m_header; }
    ChunkShape          chunk_shape()   const { return m_header.chunk_shape; }
    uint64_t            dim_x()         const { return m_header.dim_x; }
    uint64_t            dim_y()         const { return m_header.dim_y; }
    uint64_t            dim_z()         const { return m_header.dim_z; }
    uint32_t            num_chunks_x()  const { return m_nc_x; }
    uint32_t            num_chunks_y()  const { return m_nc_y; }
    uint32_t            num_chunks_z()  const { return m_nc_z; }

    // 各方向填充后总尺寸
    uint64_t padded_dim_x() const { return static_cast<uint64_t>(m_nc_x) * m_header.chunk_shape.cx; }
    uint64_t padded_dim_y() const { return static_cast<uint64_t>(m_nc_y) * m_header.chunk_shape.cy; }
    uint64_t padded_dim_z() const { return static_cast<uint64_t>(m_nc_z) * m_header.chunk_shape.cz; }

    // ── 切面读取 ────────────────────────────────────────────

    // 沿 X 轴切面（固定 x，取 y-z 平面）
    // 返回 dim_y × dim_z 个 float，行优先 (y 外层，z 内层)
    std::vector<float> read_x_slice(uint32_t x);

    // 沿 Y 轴切面（固定 y，取 x-z 平面）
    // 返回 dim_x × dim_z 个 float，行优先 (x 外层，z 内层)
    std::vector<float> read_y_slice(uint32_t y);

    // 沿 Z 轴切面（固定 z，取 x-y 平面）
    // 返回 dim_x × dim_y 个 float，行优先 (x 外层，y 内层)
    std::vector<float> read_z_slice(uint32_t z);

    // 读取单个分块（按块坐标 ix, iy, iz）
    // 返回 cx × cy × cz 个 float（含补零填充部分）
    std::vector<float> read_chunk(uint32_t ix, uint32_t iy, uint32_t iz);

private:
    // 按 chunk 线性索引读取
    std::vector<float> read_chunk_by_index(uint64_t chunk_idx);

    C3DRHeader                  m_header;
    std::vector<C3DRIndexEntry> m_index;   // 索引表（按块行优先排列）
    FILE*                       m_file;
    uint32_t                    m_nc_x;    // X 方向块数  ceil(dim_x / cx)
    uint32_t                    m_nc_y;    // Y 方向块数  ceil(dim_y / cy)
    uint32_t                    m_nc_z;    // Z 方向块数  ceil(dim_z / cz)
};
