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

                // 记录当前 chunk 的文件偏移
                long current_pos = ftell(f);
                if (current_pos < 0) {
                    fclose(f);
                    throw std::runtime_error("获取文件位置失败");
                }
                chunk_offsets[chunk_idx] = static_cast<uint64_t>(current_pos);

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
    if (fseek(m_file, static_cast<long>(m_header.index_offset), SEEK_SET) != 0) {
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

    if (fseek(m_file, static_cast<long>(offset), SEEK_SET) != 0) return buf;
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
