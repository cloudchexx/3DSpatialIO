// bench_e2e.cpp - 端到端评分测试程序
// 用法: bench_e2e <c3dr_file> [--output out.bin] [--cache-size 2048]
//
// 测试内容:
//   - 随机切片: X/Y/Z 各 100 次，取平均耗时
//   - 连续切片: X/Y/Z 各 10 次，取平均耗时
//   - 所有切片写入合并输出文件

#include "cache_reader.h"
#include "chunk_io.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static void print_usage(const char* prog) {
    std::printf("用法: %s <c3dr_file> [选项]\n", prog);
    std::printf("选项:\n");
    std::printf("  --output <path>    输出合并文件路径 (默认: contest_output.bin)\n");
    std::printf("  --cache-size <MB>  缓存大小 MB (默认: 2048)\n");
    std::printf("  --help             显示帮助信息\n");
}

struct BenchArgs {
    std::string c3dr_path;
    std::string output_path = "contest_output.bin";
    size_t cache_size_mb = 2048;
    bool show_help = false;
};

static BenchArgs parse_args(int argc, char* argv[]) {
    BenchArgs args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            args.show_help = true;
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            args.output_path = argv[++i];
        } else if (std::strcmp(argv[i], "--cache-size") == 0 && i + 1 < argc) {
            args.cache_size_mb = static_cast<size_t>(std::stoull(argv[++i]));
        } else if (argv[i][0] != '-') {
            args.c3dr_path = argv[i];
        }
    }
    return args;
}

int main(int argc, char* argv[]) {
    BenchArgs args = parse_args(argc, argv);

    if (args.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    if (args.c3dr_path.empty()) {
        std::fprintf(stderr, "错误: 必须指定 c3dr 文件路径\n");
        print_usage(argv[0]);
        return 1;
    }

    size_t cache_bytes = args.cache_size_mb * 1024ULL * 1024ULL;

    C3DRCacheReader reader(cache_bytes);
    if (!reader.open(args.c3dr_path)) {
        std::fprintf(stderr, "错误: 无法打开 c3dr 文件: %s\n", args.c3dr_path.c_str());
        return 1;
    }

    FILE* fout = std::fopen(args.output_path.c_str(), "wb");
    if (!fout) {
        std::fprintf(stderr, "错误: 无法创建输出文件: %s\n", args.output_path.c_str());
        reader.close();
        return 1;
    }

    uint64_t dim_x = reader.dim_x();
    uint64_t dim_y = reader.dim_y();
    uint64_t dim_z = reader.dim_z();
    ChunkShape shape = reader.chunk_shape();

    std::mt19937 rng(42);

    double random_x_ms = 0.0, random_y_ms = 0.0, random_z_ms = 0.0;
    double seq_x_ms = 0.0, seq_y_ms = 0.0, seq_z_ms = 0.0;

    using Clock = std::chrono::high_resolution_clock;

    auto t0 = Clock::now();
    auto t1 = Clock::now();

    std::printf("\n========== 评分基准测试 ==========\n");
    std::printf("数据维度:       %llu × %llu × %llu\n",
        static_cast<unsigned long long>(dim_x),
        static_cast<unsigned long long>(dim_y),
        static_cast<unsigned long long>(dim_z));
    std::printf("切块形状:       %d × %d × %d\n", shape.cx, shape.cy, shape.cz);
    std::printf("缓存容量:       %zu MB\n", args.cache_size_mb);
    std::printf("输出文件:       %s\n", args.output_path.c_str());
    std::printf("----------------------------------------\n");

    std::printf("运行随机切片测试 (X/Y/Z 各 100 次)...\n");

    t0 = Clock::now();
    for (int i = 0; i < 100; ++i) {
        uint32_t x = static_cast<uint32_t>(rng() % dim_x);
        auto slice = reader.read_x_slice(x);
        if (!slice.empty()) {
            std::fwrite(slice.data(), sizeof(float), slice.size(), fout);
        }
        if (i % 20 == 19) std::printf("  X 轴: %d/100\n", i + 1);
    }
    t1 = Clock::now();
    random_x_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0 / 100.0;

    t0 = Clock::now();
    for (int i = 0; i < 100; ++i) {
        uint32_t y = static_cast<uint32_t>(rng() % dim_y);
        auto slice = reader.read_y_slice(y);
        if (!slice.empty()) {
            std::fwrite(slice.data(), sizeof(float), slice.size(), fout);
        }
        if (i % 20 == 19) std::printf("  Y 轴: %d/100\n", i + 1);
    }
    t1 = Clock::now();
    random_y_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0 / 100.0;

    t0 = Clock::now();
    for (int i = 0; i < 100; ++i) {
        uint32_t z = static_cast<uint32_t>(rng() % dim_z);
        auto slice = reader.read_z_slice(z);
        if (!slice.empty()) {
            std::fwrite(slice.data(), sizeof(float), slice.size(), fout);
        }
        if (i % 20 == 19) std::printf("  Z 轴: %d/100\n", i + 1);
    }
    t1 = Clock::now();
    random_z_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0 / 100.0;

    std::printf("----------------------------------------\n");
    std::printf("运行连续切片测试 (X/Y/Z 各 10 次)...\n");

    uint32_t start_x = static_cast<uint32_t>(rng() % (dim_x > 10 ? dim_x - 10 : 1));
    t0 = Clock::now();
    for (int i = 0; i < 10; ++i) {
        auto slice = reader.read_x_slice(start_x + i);
        if (!slice.empty()) {
            std::fwrite(slice.data(), sizeof(float), slice.size(), fout);
        }
    }
    t1 = Clock::now();
    seq_x_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0 / 10.0;

    uint32_t start_y = static_cast<uint32_t>(rng() % (dim_y > 10 ? dim_y - 10 : 1));
    t0 = Clock::now();
    for (int i = 0; i < 10; ++i) {
        auto slice = reader.read_y_slice(start_y + i);
        if (!slice.empty()) {
            std::fwrite(slice.data(), sizeof(float), slice.size(), fout);
        }
    }
    t1 = Clock::now();
    seq_y_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0 / 10.0;

    uint32_t start_z = static_cast<uint32_t>(rng() % (dim_z > 10 ? dim_z - 10 : 1));
    t0 = Clock::now();
    for (int i = 0; i < 10; ++i) {
        auto slice = reader.read_z_slice(start_z + i);
        if (!slice.empty()) {
            std::fwrite(slice.data(), sizeof(float), slice.size(), fout);
        }
    }
    t1 = Clock::now();
    seq_z_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0 / 10.0;

    std::fflush(fout);
    std::fclose(fout);

    size_t hits = reader.cache_hits();
    size_t misses = reader.cache_misses();
    size_t total_access = hits + misses;
    double hit_rate = (total_access > 0) ? (static_cast<double>(hits) / total_access * 100.0) : 0.0;

    double total_random_ms = random_x_ms + random_y_ms + random_z_ms;
    double total_seq_ms = seq_x_ms + seq_y_ms + seq_z_ms;
    double total_ms = (total_random_ms + total_seq_ms) / 6.0;

    std::printf("========================================\n");
    std::printf("========== 测试结果 ==========\n");
    std::printf("随机切片 (avg/slice):\n");
    std::printf("  X 轴:  %.3f ms\n", random_x_ms);
    std::printf("  Y 轴:  %.3f ms\n", random_y_ms);
    std::printf("  Z 轴:  %.3f ms\n", random_z_ms);
    std::printf("连续切片 (avg/slice):\n");
    std::printf("  X 轴:  %.3f ms\n", seq_x_ms);
    std::printf("  Y 轴:  %.3f ms\n", seq_y_ms);
    std::printf("  Z 轴:  %.3f ms\n", seq_z_ms);
    std::printf("----------------------------------------\n");
    std::printf("综合平均耗时:     %.3f ms\n", total_ms);
    std::printf("缓存命中:         %zu / %zu (%.1f%%)\n", hits, total_access, hit_rate);
    std::printf("缓存内存使用:     %.1f MB\n",
        static_cast<double>(reader.cache_memory_usage()) / (1024.0 * 1024.0));
    std::printf("========================================\n\n");

    reader.close();

    return 0;
}
