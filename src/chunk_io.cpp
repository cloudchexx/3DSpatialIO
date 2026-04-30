#include "chunk_io.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

// ════════════════════════════════════════════════════════════════
// 内部工具函数
// ════════════════════════════════════════════════════════════════

namespace {

// 向上取整除法
inline uint32_t ceil_div(uint64_t a, uint32_t b) {
    return static_cast<uint32_t>((a + b - 1) / b);
}

// 单个 chunk 的字节大小
inline size_t chunk_bytes(const ChunkShape& s) {
    return static_cast<size_t>(s.cx) * s.cy * s.cz * sizeof(float);
}

}  // namespace

// ════════════════════════════════════════════════════════════════
// write_c3dr_file — 将原始数据分块写入 .c3dr 文件
// ════════════════════════════════════════════════════════════════

C3DRHeader write_c3dr_file(
    const std::string& path,
    const std::vector<float>& data,
    uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
    const ChunkShape& shape)
{
    // ── 计算各维度分块数量 ──────────────────────────────────
    uint32_t nc_x = ceil_div(dim_x, static_cast<uint32_t>(shape.cx));
    uint32_t nc_y = ceil_div(dim_y, static_cast<uint32_t>(shape.cy));
    uint32_t nc_z = ceil_div(dim_z, static_cast<uint32_t>(shape.cz));
    uint64_t total_chunks = static_cast<uint64_t>(nc_x) * nc_y * nc_z;

    // ── 计算各区域偏移 ──────────────────────────────────────
    // 布局: [Header (64B)] [IndexHeader (16B)] [IndexEntries (total_chunks*8B)] [Data...]
    constexpr uint64_t HEADER_SIZE = sizeof(C3DRHeader);
    constexpr uint64_t INDEX_HEADER_SIZE = 16;
    uint64_t index_entries_size = total_chunks * sizeof(uint64_t);
    uint64_t index_offset = HEADER_SIZE;
    uint64_t data_offset = index_offset + INDEX_HEADER_SIZE + index_entries_size;
    size_t   one_chunk_sz = chunk_bytes(shape);

    // ── 打开输出文件 ────────────────────────────────────────
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        throw std::runtime_error("无法创建输出文件: " + path);
    }

    // ── 预留 Header + IndexTable 空间（先写 0 占位） ───────
    std::vector<uint8_t> placeholder(data_offset, 0);
    if (fwrite(placeholder.data(), 1, placeholder.size(), f) != placeholder.size()) {
        fclose(f);
        throw std::runtime_error("写入预留空间失败");
    }

    // ── 分块写入数据，同时记录每个 chunk 的文件偏移 ──────────
    std::vector<uint64_t> chunk_offsets(total_chunks, 0);
    std::vector<float>    chunk_buf(static_cast<size_t>(shape.cx) * shape.cy * shape.cz, 0.0f);

    for (uint32_t ix = 0; ix < nc_x; ++ix) {
        uint32_t x_start = ix * static_cast<uint32_t>(shape.cx);
        uint32_t x_end   = (std::min)(x_start + static_cast<uint32_t>(shape.cx), dim_x);

        for (uint32_t iy = 0; iy < nc_y; ++iy) {
            uint32_t y_start = iy * static_cast<uint32_t>(shape.cy);
            uint32_t y_end   = (std::min)(y_start + static_cast<uint32_t>(shape.cy), dim_y);

            for (uint32_t iz = 0; iz < nc_z; ++iz) {
                uint32_t z_start = iz * static_cast<uint32_t>(shape.cz);
                uint32_t z_end   = (std::min)(z_start + static_cast<uint32_t>(shape.cz), dim_z);

                uint64_t chunk_idx = static_cast<uint64_t>(ix) * nc_y * nc_z
                                   + static_cast<uint64_t>(iy) * nc_z
                                   + iz;

                // 记录当前 chunk 的文件偏移（公式计算，避免 ftell 32 位溢出）
                chunk_offsets[chunk_idx] = data_offset + chunk_idx * one_chunk_sz;

                // 填充 chunk 缓冲区（原始数据 + 边界补零）
                std::memset(chunk_buf.data(), 0, one_chunk_sz);
                for (uint32_t lx = x_start; lx < x_end; ++lx) {
                    uint32_t local_x = lx - x_start;
                    // 原始数据中该 x 行的起始偏移
                    size_t src_base_x = static_cast<size_t>(lx) * dim_y * dim_z;
                    // chunk 中该 local_x 行的起始偏移
                    size_t dst_base_x = static_cast<size_t>(local_x) * shape.cy * shape.cz;
                    for (uint32_t ly = y_start; ly < y_end; ++ly) {
                        uint32_t local_y = ly - y_start;
                        size_t src_base_y = src_base_x + static_cast<size_t>(ly) * dim_z;
                        size_t dst_base_y = dst_base_x + static_cast<size_t>(local_y) * shape.cz;
                        // 连续拷贝 z 方向（最内层，物理连续）
                        size_t z_count = z_end - z_start;
                        std::memcpy(
                            chunk_buf.data() + dst_base_y + (z_start - z_start),  // = dst_base_y
                            data.data()     + src_base_y + z_start,
                            z_count * sizeof(float));
                    }
                }

                // 写入该 chunk
                if (fwrite(chunk_buf.data(), 1, one_chunk_sz, f) != one_chunk_sz) {
                    fclose(f);
                    throw std::runtime_error("写入 chunk 数据失败");
                }
            }
        }
    }

    // ── 回写 Header ─────────────────────────────────────────
    C3DRHeader header;
    header.magic        = 0x52443343;
    header.version      = C3DR_VERSION;
    header.dim_x        = dim_x;
    header.dim_y        = dim_y;
    header.dim_z        = dim_z;
    header.chunk_shape  = shape;
    header.index_offset = index_offset;
    header.data_offset  = data_offset;

    rewind(f);
    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        fclose(f);
        throw std::runtime_error("写入 Header 失败");
    }

    // ── 回写 IndexTable 头 ──────────────────────────────────
    uint32_t idx_header[4] = { nc_x, nc_y, nc_z, 0 };
    if (fwrite(idx_header, sizeof(idx_header), 1, f) != 1) {
        fclose(f);
        throw std::runtime_error("写入 IndexTable 头失败");
    }

    // ── 回写 IndexTable 条目 ────────────────────────────────
    if (fwrite(chunk_offsets.data(), sizeof(uint64_t), total_chunks, f) != total_chunks) {
        fclose(f);
        throw std::runtime_error("写入 IndexTable 条目失败");
    }

    fclose(f);
    return header;
}

// ════════════════════════════════════════════════════════════════
// write_c3dr_file_stream — 流式写入（大文件支持，内存与数据总量解耦）
// ════════════════════════════════════════════════════════════════

C3DRHeader write_c3dr_file_stream(
    const std::string& out_path,
    RawFileReader& reader,
    const ChunkShape& shape)
{
    // ── 获取源文件维度与块形状 ──────────────────────────────
    uint64_t dim_x = reader.dim_x();
    uint64_t dim_y = reader.dim_y();
    uint64_t dim_z = reader.dim_z();
    uint32_t cx = static_cast<uint32_t>(shape.cx);
    uint32_t cy = static_cast<uint32_t>(shape.cy);
    uint32_t cz = static_cast<uint32_t>(shape.cz);

    // ── 计算各维度分块数量 ──────────────────────────────────
    uint32_t nc_x = ceil_div(dim_x, cx);
    uint32_t nc_y = ceil_div(dim_y, cy);
    uint32_t nc_z = ceil_div(dim_z, cz);
    uint64_t total_chunks = static_cast<uint64_t>(nc_x) * nc_y * nc_z;

    // ── 计算各区域偏移 ──────────────────────────────────────
    constexpr uint64_t HEADER_SIZE = sizeof(C3DRHeader);
    constexpr uint64_t INDEX_HEADER_SIZE = 16;
    uint64_t index_entries_size = total_chunks * sizeof(uint64_t);
    uint64_t index_offset = HEADER_SIZE;
    uint64_t data_offset = index_offset + INDEX_HEADER_SIZE + index_entries_size;
    size_t   one_chunk_sz = static_cast<size_t>(cx) * cy * cz * sizeof(float);

    // ── 打开输出文件 ────────────────────────────────────────
    FILE* f = fopen(out_path.c_str(), "wb");
    if (!f) {
        throw std::runtime_error("无法创建输出文件: " + out_path);
    }

    // ── 预留 Header + IndexTable 空间 ───────────────────────
    std::vector<uint8_t> placeholder(static_cast<size_t>(data_offset), 0);
    if (fwrite(placeholder.data(), 1, placeholder.size(), f) != placeholder.size()) {
        fclose(f);
        throw std::runtime_error("写入预留空间失败");
    }

    // ── 索引表（用公式预计算每个 chunk 的文件偏移） ────────
    std::vector<uint64_t> chunk_offsets(total_chunks, 0);

    // ── 分配子组缓冲数组（nc_z 个 chunk，内存仅保留一个子组） ──
    // chunk_bufs[nc_z]，每个 = cx × cy × cz × sizeof(float) 字节
    std::vector<std::vector<uint8_t>> chunk_bufs(nc_z);
    for (uint32_t iz = 0; iz < nc_z; ++iz) {
        chunk_bufs[iz].assign(one_chunk_sz, 0);
    }

    // 源文件句柄（从 reader 获取，直接读取避免 read_region 边界布局问题）
    FILE*    src = reader.file_handle();
    uint64_t src_data_offset = reader.data_offset();  // 文件头跳过字节数

    // 子组 Y 行临时缓冲区：一次性读取固定 x 下整个 y 区间的所有 z 数据
    // 大小 = y_len × dim_z × elem_size，最大 ~64 × 2500 × 4 = 640 KB
    std::vector<uint8_t> y_row_buf;
    {
        uint64_t max_y_row = static_cast<uint64_t>(cy) * dim_z * sizeof(float);
        y_row_buf.resize(static_cast<size_t>(max_y_row));
    }

    // ── 子组流式写入 ────────────────────────────────────────
    // 外循环 iy，中循环 ix：以 (ix, iy) 子组为单位工作
    for (uint32_t iy = 0; iy < nc_y; ++iy) {
        uint32_t y_start = iy * cy;
        uint32_t y_end   = (std::min)(y_start + cy, static_cast<uint32_t>(dim_y));
        uint32_t y_len   = y_end - y_start;

        for (uint32_t ix = 0; ix < nc_x; ++ix) {
            uint32_t x_start = ix * cx;
            uint32_t x_end   = (std::min)(x_start + cx, static_cast<uint32_t>(dim_x));
            uint32_t x_len   = x_end - x_start;

            size_t dst_slice_stride = static_cast<size_t>(cy) * cz * sizeof(float);
            size_t dst_row_stride   = static_cast<size_t>(cz) * sizeof(float);
            size_t y_row_bytes      = static_cast<size_t>(y_len) * static_cast<size_t>(dim_z) * sizeof(float);

            // 清零所有 nc_z 个 chunk 缓冲区
            for (uint32_t iz = 0; iz < nc_z; ++iz) {
                std::memset(chunk_bufs[iz].data(), 0, one_chunk_sz);
            }

            // ── 步骤 A：优化读取 —— 每 x 层一次 fread，再分散到 nc_z 个 chunk ──
            for (uint32_t lx = 0; lx < x_len; ++lx) {
                uint64_t x = static_cast<uint64_t>(x_start) + lx;

                // 源文件中固定 x 下 y ∈ [y_start, y_end) 的全部 z 数据
                // 在 row-major X-Y-Z 布局下，这是一段连续数据
                uint64_t src_offset = src_data_offset
                                    + (x * dim_y * dim_z
                                     + static_cast<uint64_t>(y_start) * dim_z)
                                     * sizeof(float);

                FSEEK64(src, static_cast<int64_t>(src_offset), SEEK_SET);
                size_t n = fread(y_row_buf.data(), 1, y_row_bytes, src);
                if (n != y_row_bytes) {
                    fclose(f);
                    throw std::runtime_error("读取源文件子组数据失败");
                }

                // 将 y_row_buf 中的数据分散写入 nc_z 个 chunk 缓冲区
                size_t dst_x_base = static_cast<size_t>(lx) * dst_slice_stride;
                for (uint32_t iz = 0; iz < nc_z; ++iz) {
                    uint32_t z_start = iz * cz;
                    uint32_t z_end   = (std::min)(z_start + cz, static_cast<uint32_t>(dim_z));
                    uint32_t z_len   = z_end - z_start;

                    for (uint32_t ly = 0; ly < y_len; ++ly) {
                        // y_row_buf 中第 ly 行、第 z_start 列
                        size_t src_pos = static_cast<size_t>(ly) * static_cast<size_t>(dim_z)
                                       + static_cast<size_t>(z_start);
                        // dst 中 (lx, ly, 0) 位置
                        size_t dst_pos = dst_x_base + static_cast<size_t>(ly) * dst_row_stride;

                        std::memcpy(chunk_bufs[iz].data() + dst_pos,
                                    y_row_buf.data() + src_pos * sizeof(float),
                                    static_cast<size_t>(z_len) * sizeof(float));
                    }
                }
            }

            // ── 步骤 B：按 chunk_idx 偏移写入 nc_z 个 chunk（Seek 写入） ──
            for (uint32_t iz = 0; iz < nc_z; ++iz) {
                uint64_t chunk_idx = static_cast<uint64_t>(ix) * nc_y * nc_z
                                   + static_cast<uint64_t>(iy) * nc_z
                                   + iz;
                // 用公式计算偏移，避免 ftell 32 位溢出
                uint64_t write_offset = data_offset + chunk_idx * one_chunk_sz;
                chunk_offsets[chunk_idx] = write_offset;

                // 由于子组遍历顺序 (iy, ix) 不等于 chunk_idx 的线性顺序，
                // 必须显式 seek 到正确位置再写入
                if (FSEEK64(f, static_cast<int64_t>(write_offset), SEEK_SET) != 0) {
                    fclose(f);
                    throw std::runtime_error("定位到 chunk 写入位置失败");
                }
                if (fwrite(chunk_bufs[iz].data(), 1, one_chunk_sz, f) != one_chunk_sz) {
                    fclose(f);
                    throw std::runtime_error("写入 chunk 数据失败");
                }
            }
        }
    }

    // ── 回写 Header ─────────────────────────────────────────
    C3DRHeader header;
    header.magic        = 0x52443343;
    header.version      = C3DR_VERSION;
    header.dim_x        = dim_x;
    header.dim_y        = dim_y;
    header.dim_z        = dim_z;
    header.chunk_shape  = shape;
    header.index_offset = index_offset;
    header.data_offset  = data_offset;

    rewind(f);
    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        fclose(f);
        throw std::runtime_error("写入 Header 失败");
    }

    // ── 回写 IndexTable 头 ──────────────────────────────────
    uint32_t idx_header[4] = { nc_x, nc_y, nc_z, 0 };
    if (fwrite(idx_header, sizeof(idx_header), 1, f) != 1) {
        fclose(f);
        throw std::runtime_error("写入 IndexTable 头失败");
    }

    // ── 回写 IndexTable 条目 ────────────────────────────────
    if (fwrite(chunk_offsets.data(), sizeof(uint64_t), total_chunks, f) != total_chunks) {
        fclose(f);
        throw std::runtime_error("写入 IndexTable 条目失败");
    }

    fclose(f);
    return header;
}

// ════════════════════════════════════════════════════════════════
// C3DRReader 实现
// ════════════════════════════════════════════════════════════════

C3DRReader::C3DRReader()
    : m_file(nullptr), m_nc_x(0), m_nc_y(0), m_nc_z(0)
{}

C3DRReader::~C3DRReader() {
    close();
}

bool C3DRReader::open(const std::string& path) {
    close();  // 确保之前的文件已关闭

    m_file = fopen(path.c_str(), "rb");
    if (!m_file) return false;

    // ── 读取 Header ─────────────────────────────────────────
    if (fread(&m_header, sizeof(m_header), 1, m_file) != 1) {
        close();
        return false;
    }

    // 校验 magic
    if (m_header.magic != 0x52443343) {
        close();
        return false;
    }

    // ── 定位到 IndexTable 并读取 ────────────────────────────
    if (FSEEK64(m_file, static_cast<int64_t>(m_header.index_offset), SEEK_SET) != 0) {
        close();
        return false;
    }

    uint32_t idx_header[4];
    if (fread(idx_header, sizeof(idx_header), 1, m_file) != 1) {
        close();
        return false;
    }
    m_nc_x = idx_header[0];
    m_nc_y = idx_header[1];
    m_nc_z = idx_header[2];

    uint64_t total_chunks = static_cast<uint64_t>(m_nc_x) * m_nc_y * m_nc_z;
    m_index.resize(total_chunks);

    // 读取每个索引条目（uint64_t offset）
    for (uint64_t i = 0; i < total_chunks; ++i) {
        uint64_t off;
        if (fread(&off, sizeof(off), 1, m_file) != 1) {
            close();
            return false;
        }
        m_index[i].offset = off;
    }

    return true;
}

void C3DRReader::close() {
    if (m_file) {
        fclose(m_file);
        m_file = nullptr;
    }
    m_index.clear();
    m_nc_x = m_nc_y = m_nc_z = 0;
}

// ─── 辅助：按 chunk 线性索引读取一个分块 ─────────────────────
std::vector<float> C3DRReader::read_chunk_by_index(uint64_t chunk_idx) {
    if (chunk_idx >= m_index.size()) return {};
    uint64_t offset = m_index[chunk_idx].offset;
    size_t   cx     = static_cast<size_t>(m_header.chunk_shape.cx);
    size_t   cy     = static_cast<size_t>(m_header.chunk_shape.cy);
    size_t   cz     = static_cast<size_t>(m_header.chunk_shape.cz);
    size_t   total  = cx * cy * cz;

    std::vector<float> buf(total, 0.0f);

    if (FSEEK64(m_file, static_cast<int64_t>(offset), SEEK_SET) != 0) return buf;
    fread(buf.data(), sizeof(float), total, m_file);
    return buf;
}

std::vector<float> C3DRReader::read_chunk(uint32_t ix, uint32_t iy, uint32_t iz) {
    if (ix >= m_nc_x || iy >= m_nc_y || iz >= m_nc_z) return {};
    uint64_t idx = static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                 + static_cast<uint64_t>(iy) * m_nc_z
                 + iz;
    return read_chunk_by_index(idx);
}

// ─── X 轴切面（固定 x，取整个 y-z 平面） ──────────────────────
// 需要读取 ix = x/cx 这一"列"的所有 chunk
std::vector<float> C3DRReader::read_x_slice(uint32_t x) {
    if (x >= m_header.dim_x) return {};

    uint32_t ix      = x / static_cast<uint32_t>(m_header.chunk_shape.cx);
    uint32_t local_x = x % static_cast<uint32_t>(m_header.chunk_shape.cx);
    size_t   cy = static_cast<size_t>(m_header.chunk_shape.cy);
    size_t   cz = static_cast<size_t>(m_header.chunk_shape.cz);

    uint64_t dim_y = m_header.dim_y;
    uint64_t dim_z = m_header.dim_z;
    std::vector<float> result(static_cast<size_t>(dim_y * dim_z), 0.0f);

    // 遍历 ix 列下的所有 (iy, iz) chunk
    for (uint32_t iy = 0; iy < m_nc_y; ++iy) {
        uint32_t y_start = iy * static_cast<uint32_t>(cy);

        for (uint32_t iz = 0; iz < m_nc_z; ++iz) {
            uint32_t z_start = iz * static_cast<uint32_t>(cz);

            uint64_t chunk_idx = static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z
                               + iz;

            auto chunk = read_chunk_by_index(chunk_idx);
            if (chunk.empty()) continue;

            // X 切面在该 chunk 中的位置: local_x * cy * cz
            // 这是一段连续内存（cy * cz 个 float）
            const float* src = chunk.data() + static_cast<size_t>(local_x) * cy * cz;

            // 将这段平面放置到输出缓冲的正确位置
            uint32_t y_end = (std::min)(y_start + static_cast<uint32_t>(cy),
                                        static_cast<uint32_t>(dim_y));
            uint32_t z_end = (std::min)(z_start + static_cast<uint32_t>(cz),
                                        static_cast<uint32_t>(dim_z));

            for (uint32_t yy = y_start; yy < y_end; ++yy) {
                uint32_t local_y = yy - y_start;
                size_t src_row = static_cast<size_t>(local_y) * cz;
                size_t dst_row = static_cast<size_t>(yy) * dim_z;
                size_t count = z_end - z_start;
                std::memcpy(result.data() + dst_row + z_start,
                            src + src_row,
                            count * sizeof(float));
            }
        }
    }

    return result;
}

// ─── Y 轴切面（固定 y，取 x-z 平面） ──────────────────────────
std::vector<float> C3DRReader::read_y_slice(uint32_t y) {
    if (y >= m_header.dim_y) return {};

    uint32_t iy      = y / static_cast<uint32_t>(m_header.chunk_shape.cy);
    uint32_t local_y = y % static_cast<uint32_t>(m_header.chunk_shape.cy);
    size_t   cx = static_cast<size_t>(m_header.chunk_shape.cx);
    size_t   cy = static_cast<size_t>(m_header.chunk_shape.cy);
    size_t   cz = static_cast<size_t>(m_header.chunk_shape.cz);

    uint64_t dim_x = m_header.dim_x;
    uint64_t dim_z = m_header.dim_z;
    std::vector<float> result(static_cast<size_t>(dim_x * dim_z), 0.0f);

    for (uint32_t ix = 0; ix < m_nc_x; ++ix) {
        uint32_t x_start = ix * static_cast<uint32_t>(cx);

        for (uint32_t iz = 0; iz < m_nc_z; ++iz) {
            uint32_t z_start = iz * static_cast<uint32_t>(cz);

            uint64_t chunk_idx = static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z
                               + iz;

            auto chunk = read_chunk_by_index(chunk_idx);
            if (chunk.empty()) continue;

            // Y 切面在该 chunk 中位于 local_y 行
            // 对于每个 local_x，偏移 = local_x * cy * cz + local_y * cz
            // 即每 cx 行取一行，跨度为 cy * cz；每行内连续 cz 个 float

            uint32_t x_end = (std::min)(x_start + static_cast<uint32_t>(cx),
                                        static_cast<uint32_t>(dim_x));
            uint32_t z_end = (std::min)(z_start + static_cast<uint32_t>(cz),
                                        static_cast<uint32_t>(dim_z));

            for (uint32_t xx = x_start; xx < x_end; ++xx) {
                uint32_t local_x = xx - x_start;
                const float* src = chunk.data()
                    + static_cast<size_t>(local_x) * cy * cz
                    + static_cast<size_t>(local_y) * cz;
                size_t dst_base = static_cast<size_t>(xx) * dim_z + z_start;
                size_t count = z_end - z_start;
                std::memcpy(result.data() + dst_base, src, count * sizeof(float));
            }
        }
    }

    return result;
}

// ─── Z 轴切面（固定 z，取 x-y 平面） ──────────────────────────
std::vector<float> C3DRReader::read_z_slice(uint32_t z) {
    if (z >= m_header.dim_z) return {};

    uint32_t iz      = z / static_cast<uint32_t>(m_header.chunk_shape.cz);
    uint32_t local_z = z % static_cast<uint32_t>(m_header.chunk_shape.cz);
    size_t   cx = static_cast<size_t>(m_header.chunk_shape.cx);
    size_t   cy = static_cast<size_t>(m_header.chunk_shape.cy);
    size_t   cz = static_cast<size_t>(m_header.chunk_shape.cz);

    uint64_t dim_x = m_header.dim_x;
    uint64_t dim_y = m_header.dim_y;
    std::vector<float> result(static_cast<size_t>(dim_x * dim_y), 0.0f);

    for (uint32_t ix = 0; ix < m_nc_x; ++ix) {
        uint32_t x_start = ix * static_cast<uint32_t>(cx);

        for (uint32_t iy = 0; iy < m_nc_y; ++iy) {
            uint32_t y_start = iy * static_cast<uint32_t>(cy);

            uint64_t chunk_idx = static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z
                               + iz;

            auto chunk = read_chunk_by_index(chunk_idx);
            if (chunk.empty()) continue;

            // Z 切面在该 chunk 中：需要逐元素跳跃读取
            // chunk[local_x][local_y][local_z]
            //   偏移 = local_x * cy * cz + local_y * cz + local_z
            // 对于固定 local_z，每个 (local_x, local_y) 的元素间距为 cz

            uint32_t x_end = (std::min)(x_start + static_cast<uint32_t>(cx),
                                        static_cast<uint32_t>(dim_x));
            uint32_t y_end = (std::min)(y_start + static_cast<uint32_t>(cy),
                                        static_cast<uint32_t>(dim_y));

            for (uint32_t xx = x_start; xx < x_end; ++xx) {
                uint32_t local_x = xx - x_start;
                size_t dst_row = static_cast<size_t>(xx) * dim_y + y_start;

                for (uint32_t yy = y_start; yy < y_end; ++yy) {
                    uint32_t local_y = yy - y_start;
                    result[dst_row + yy - y_start] = chunk[
                        static_cast<size_t>(local_x) * cy * cz
                        + static_cast<size_t>(local_y) * cz
                        + local_z];
                }
            }
        }
    }

    return result;
}
