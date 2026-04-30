#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
