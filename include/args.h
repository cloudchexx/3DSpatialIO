#pragma once

#include <cstdint>
#include <string>

struct Args {
    std::string input_path;
    std::string output_path;
    uint32_t    dim_x = 0;
    uint32_t    dim_y = 0;
    uint32_t    dim_z = 0;
    int         elem_size = 4;     // 元素字节数（默认 float = 4）
    uint64_t    skip_bytes = 0;    // 跳过文件头字节数（默认 0）
    bool        show_help  = false;
    uint64_t    cache_size_mb = 2048;  // 内存缓存大小(MB)，最小 2048，默认 2048
    bool        do_profile = false;  // 运行内存 Profiling
    bool        do_bench   = false;  // 运行切面读取性能回归
};

Args parse_args(int argc, char* argv[]);
void print_usage(const char* prog_name);
