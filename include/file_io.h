#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ──────────────── 64 位文件定位宏 ────────────────
#ifdef _WIN32
    #define FSEEK64 _fseeki64
    #define FTELL64 _ftelli64
#else
    #define FSEEK64 fseeko64
    #define FTELL64 ftello64
#endif

// ──────────────── 原有：小文件全量读取 ────────────────

struct RawDataInfo {
    uint32_t dim_x;
    uint32_t dim_y;
    uint32_t dim_z;
    size_t   total_elements;
    float    value_min;
    float    value_max;
};

RawDataInfo read_raw_float_file(
    const std::string& path,
    uint32_t dim_x,
    uint32_t dim_y,
    uint32_t dim_z,
    std::vector<float>& out_data);

// ──────────────── 新增：流式源文件读取器 ────────────────

// 大文件流式读取器，内存占用与数据总量解耦。
// 按 chunk 逻辑区域逐块读取，避免一次性全量加载到内存。
class RawFileReader {
public:
    // 打开源文件并校验总字节数（用 64 位 API）
    // elem_size 默认 4（float32）
    // skip_bytes 指定文件头跳过字节数（默认 0，即纯数据无头）
    bool open(const std::string& path,
              uint64_t dim_x, uint64_t dim_y, uint64_t dim_z,
              int elem_size = 4,
              uint64_t skip_bytes = 0);

    // 关闭文件句柄
    void close();

    // 读取一个逻辑 chunk 区域 [x0, x1) × [y0, y1) × [z0, z1)
    // dst 必须预分配 (x1-x0)*(y1-y0)*(z1-z0)*elem_size 字节
    // dst 布局：按 Z 最内层排列（与 chunk 内部布局一致）
    // 返回实际读取字节数，出错返回 0
    size_t read_region(uint64_t x0, uint64_t x1,
                       uint64_t y0, uint64_t y1,
                       uint64_t z0, uint64_t z1,
                       void* dst);

    // ── 内联 getter ──
    uint64_t dim_x()          const { return m_dim_x; }
    uint64_t dim_y()          const { return m_dim_y; }
    uint64_t dim_z()          const { return m_dim_z; }
    int      elem_size()      const { return m_elem_size; }
    uint64_t total_elements() const { return m_total_elements; }
    uint64_t data_offset()    const { return m_data_offset; }     // 文件头跳过字节数
    FILE*    file_handle()    const { return m_file; }

private:
    FILE*    m_file = nullptr;
    uint64_t m_dim_x = 0;
    uint64_t m_dim_y = 0;
    uint64_t m_dim_z = 0;
    int      m_elem_size = 4;
    uint64_t m_total_elements = 0;
    uint64_t m_data_offset = 0;  // 文件头跳过字节数，数据实际起始偏移
};
