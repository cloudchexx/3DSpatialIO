#include "args.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

Args parse_args(int argc, char* argv[]) {
    Args args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            args.show_help = true;
            return args;
        }

        if (arg == "--input" || arg == "-i") {
            if (i + 1 < argc) args.input_path = argv[++i];
            continue;
        }
        if (arg == "--output" || arg == "-o") {
            if (i + 1 < argc) args.output_path = argv[++i];
            continue;
        }

        if (arg == "--dim-x") {
            if (i + 1 < argc) args.dim_x = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            continue;
        }
        if (arg == "--dim-y") {
            if (i + 1 < argc) args.dim_y = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            continue;
        }
        if (arg == "--dim-z") {
            if (i + 1 < argc) args.dim_z = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            continue;
        }
        if (arg == "--elem-size") {
            if (i + 1 < argc) args.elem_size = std::atoi(argv[++i]);
            continue;
        }

        if (arg == "--profile") {
            args.do_profile = true;
            continue;
        }
        if (arg == "--benchmark") {
            args.do_bench = true;
            continue;
        }

        std::cerr << "未知参数: " << arg << "\n";
    }

    return args;
}

void print_usage(const char* prog_name) {
    std::cout
        << "cc_3Dreader — 三维数据存储引擎 (阶段四: 集成与 Profiling)\n\n"
        << "用法:\n"
        << "  " << prog_name << " --input <文件> --dim-x <N> --dim-y <N> --dim-z <N> [选项]\n"
        << "  " << prog_name << " --help\n\n"
        << "参数:\n"
        << "  -i, --input <路径>         原始 float32 二进制输入文件 (必填)\n"
        << "  -o, --output <路径>        输出 .c3dr 文件路径 (默认: <input>.c3dr)\n"
        << "  --dim-x, --dim-y, --dim-z  数据各维度尺寸 (必填)\n"
        << "  --elem-size <N>            元素字节数 (默认: 4 = float32)\n"
        << "  --profile                  运行内存 Profiling，标定 W_MEM 参数\n"
        << "  --benchmark                落盘后运行切面读取性能回归测试\n"
        << "  -h, --help                 显示本帮助信息\n";
}
