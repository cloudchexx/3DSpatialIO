#include "args.h"
#include "chunk_io.h"
#include "chunk_optimizer.h"
#include "file_io.h"
#include "profiler.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

// 根据输入路径自动生成输出路径
static std::string make_output_path(const std::string& input_path) {
    std::string out = input_path;
    // 去掉可能的扩展名，加上 .c3dr
    auto dot = out.rfind('.');
    if (dot != std::string::npos && dot > out.find_last_of("/\\")) {
        out = out.substr(0, dot);
    }
    return out + ".c3dr";
}

int main(int argc, char* argv[]) {
    auto args = parse_args(argc, argv);

    if (args.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    // ── 仅 Profiling 模式（不需要输入文件） ─────────────────
    if (args.do_profile && args.input_path.empty()) {
        double measured = calibrate_w_mem();
        std::printf("测得的 W_MEM 建议值: %.6f\n", measured);
        return 0;
    }

    if (args.input_path.empty()) {
        std::cerr << "错误: 必须指定 --input 参数\n";
        print_usage(argv[0]);
        return 1;
    }
    if (args.dim_x == 0 || args.dim_y == 0 || args.dim_z == 0) {
        std::cerr << "错误: --dim-x, --dim-y, --dim-z 必须为正整数\n";
        return 1;
    }

    // 自动推导输出路径
    if (args.output_path.empty()) {
        args.output_path = make_output_path(args.input_path);
    }

    try {
        // ── 1. 打开源文件（流式校验，不分配全量内存） ──────────
        RawFileReader reader;
        if (!reader.open(args.input_path, args.dim_x, args.dim_y, args.dim_z,
                          args.elem_size, args.skip_bytes)) {
            std::cerr << "错误: 无法打开或校验输入文件\n";
            return 1;
        }

        double size_mb = static_cast<double>(reader.total_elements() * reader.elem_size()) / (1024.0 * 1024.0);

        std::cout << "========== 输入数据 ==========\n";
        std::cout << "文件:       " << args.input_path << "\n";
        std::cout << "维度:       " << reader.dim_x() << " × "
                  << reader.dim_y() << " × " << reader.dim_z() << "\n";
        std::cout << "元素总数:   " << reader.total_elements() << "\n";
        std::cout << "数据量:     " << size_mb << " MB\n";
        std::cout << "值域:       （大文件模式，省略统计）\n\n";

        // ── 2. 运行切块优化器（纯几何计算，不碰数据） ──────────
        ChunkShape best = find_optimal_chunk_shape(
            static_cast<int64_t>(reader.dim_x()),
            static_cast<int64_t>(reader.dim_y()),
            static_cast<int64_t>(reader.dim_z()),
            args.elem_size);

        std::cout << "========== 切块优化结果 ==========\n";
        std::cout << "最优形状:   " << best.cx << " × "
                  << best.cy << " × " << best.cz << "\n";

        int64_t chunk_bytes = static_cast<int64_t>(best.cx) * best.cy * best.cz * args.elem_size;
        std::cout << "单块体积:   " << chunk_bytes / 1024 << " KB"
                  << " (" << static_cast<double>(chunk_bytes) / (1024*1024) << " MB)\n";

        int64_t nc_x = (static_cast<int64_t>(reader.dim_x()) + best.cx - 1) / best.cx;
        int64_t nc_y = (static_cast<int64_t>(reader.dim_y()) + best.cy - 1) / best.cy;
        int64_t nc_z = (static_cast<int64_t>(reader.dim_z()) + best.cz - 1) / best.cz;
        std::cout << "分块数:     " << nc_x << " × " << nc_y << " × " << nc_z
                  << " = " << (nc_x * nc_y * nc_z) << "\n";

        // 存储膨胀率
        int64_t pad_x = nc_x * best.cx;
        int64_t pad_y = nc_y * best.cy;
        int64_t pad_z = nc_z * best.cz;
        double ratio = static_cast<double>(pad_x * pad_y * pad_z)
                     / (static_cast<double>(reader.dim_x()) * reader.dim_y() * reader.dim_z());
        std::cout << "存储膨胀率: " << ratio << (ratio <= MAX_STORAGE_RATIO ? " (通过)" : " (超标)") << "\n\n";

        // ── 3. 流式写入 .c3dr 文件 ──────────────────────────
        std::cout << "========== 落盘（流式） ==========\n";
        C3DRHeader header = write_c3dr_file_stream(
            args.output_path, reader, best);
        std::cout << "输出文件:   " << args.output_path << "\n";
        std::cout << "Header:     magic=0x" << std::hex << header.magic << std::dec
                  << " version=" << header.version
                  << " index_off=" << header.index_offset
                  << " data_off=" << header.data_offset << "\n";
        std::cout << "完成!\n\n";

        // ── 可选: 内存 Profiling ────────────────────────────
        if (args.do_profile) {
            calibrate_w_mem();
        }

        // ── 可选: 切面读取性能回归（流式架构，TODO 6） ──────
        if (args.do_bench) {
            // 使用流式 reader 写入临时 .c3dr 并测量各轴切面读取耗时
            // 内存占用仅 nc_z 个 chunk 缓冲区 + 一个切面大小，不与数据总量挂钩
            benchmark_slices(reader, best, "temp_bench.c3dr");
        }

        // 所有操作完成后关闭源文件读取器
        reader.close();

    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
