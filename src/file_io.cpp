#include "file_io.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

RawDataInfo read_raw_float_file(
    const std::string& path,
    uint32_t dim_x,
    uint32_t dim_y,
    uint32_t dim_z,
    std::vector<float>& out_data)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        throw std::runtime_error("无法打开输入文件: " + path);
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    size_t expected_bytes = static_cast<size_t>(dim_x) * dim_y * dim_z * sizeof(float);

    if (static_cast<size_t>(file_size) != expected_bytes) {
        fclose(f);
        throw std::runtime_error(
            "文件大小不匹配: 按维度 " + std::to_string(dim_x) + "x" +
            std::to_string(dim_y) + "x" + std::to_string(dim_z) +
            " 预期 " + std::to_string(expected_bytes) +
            " 字节, 实际 " + std::to_string(file_size) + " 字节");
    }

    size_t num_floats = expected_bytes / sizeof(float);
    out_data.resize(num_floats);

    size_t read_count = fread(out_data.data(), sizeof(float), num_floats, f);
    fclose(f);

    if (read_count != num_floats) {
        throw std::runtime_error("从文件读取数据不完整");
    }

    float min_val = std::numeric_limits<float>::max();
    float max_val = std::numeric_limits<float>::lowest();
    for (auto v : out_data) {
        if (v < min_val) min_val = v;
        if (v > max_val) max_val = v;
    }
    if (min_val > max_val) {
        min_val = 0.0f;
        max_val = 0.0f;
    }

    RawDataInfo info;
    info.dim_x = dim_x;
    info.dim_y = dim_y;
    info.dim_z = dim_z;
    info.total_elements = num_floats;
    info.value_min = min_val;
    info.value_max = max_val;
    return info;
}

// ════════════════════ RawFileReader 实现 ════════════════════

bool RawFileReader::open(const std::string& path,
                         uint64_t dim_x, uint64_t dim_y, uint64_t dim_z,
                         int elem_size,
                         uint64_t skip_bytes)
{
    m_file = fopen(path.c_str(), "rb");
    if (!m_file) return false;

    // 用 64 位 API 获取文件大小，避免 2GB 上限问题
    FSEEK64(m_file, 0, SEEK_END);
    int64_t file_size = FTELL64(m_file);
    FSEEK64(m_file, 0, SEEK_SET);

    m_dim_x = dim_x;
    m_dim_y = dim_y;
    m_dim_z = dim_z;
    m_elem_size = elem_size;
    m_total_elements = dim_x * dim_y * dim_z;
    m_data_offset = skip_bytes;

    uint64_t expected = m_total_elements * static_cast<uint64_t>(elem_size) + skip_bytes;
    if (static_cast<uint64_t>(file_size) != expected) {
        close();
        return false;
    }
    return true;
}

void RawFileReader::close()
{
    if (m_file) {
        fclose(m_file);
        m_file = nullptr;
    }
}

size_t RawFileReader::read_region(uint64_t x0, uint64_t x1,
                                   uint64_t y0, uint64_t y1,
                                   uint64_t z0, uint64_t z1,
                                   void* dst)
{
    if (!m_file || !dst) return 0;

    uint64_t x_len = x1 - x0;
    uint64_t y_len = y1 - y0;
    uint64_t z_len = z1 - z0;

    if (x_len == 0 || y_len == 0 || z_len == 0) return 0;

    uint8_t*       dst_bytes = static_cast<uint8_t*>(dst);
    size_t          total_read = 0;
    const uint64_t  row_bytes = z_len * static_cast<uint64_t>(m_elem_size);
    // dst 中一行 (y_len × z_len) 的跨度（按 Z 最内层排列）
    const size_t    dst_slice_stride = static_cast<size_t>(y_len * z_len * m_elem_size);

    for (uint64_t lx = 0; lx < x_len; ++lx) {
        uint64_t x = x0 + lx;
        for (uint64_t ly = 0; ly < y_len; ++ly) {
            uint64_t y = y0 + ly;

            // 源文件偏移量: row-major X-Y-Z 顺序，加上文件头偏移
            uint64_t src_offset = m_data_offset
                                + (x * m_dim_y * m_dim_z + y * m_dim_z + z0) * m_elem_size;

            // dst 缓冲区偏移量: 布局 = (lx, ly, lz) → lx*(y_len*z_len) + ly*z_len
            size_t dst_offset = static_cast<size_t>(lx) * dst_slice_stride
                              + static_cast<size_t>(ly * z_len * m_elem_size);

            FSEEK64(m_file, static_cast<int64_t>(src_offset), SEEK_SET);
            size_t n = fread(dst_bytes + dst_offset, 1, static_cast<size_t>(row_bytes), m_file);
            total_read += n;
            if (n != static_cast<size_t>(row_bytes)) {
                return total_read;  // 源文件读取不完整，提前返回
            }
        }
    }
    return total_read;
}
