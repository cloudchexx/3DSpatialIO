#include "file_io.h"

#include <algorithm>
#include <cstdio>
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
