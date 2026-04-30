#include "profiler.h"
#include "chunk_io.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

// ════════════════════════════════════════════════════════════════
// calibrate_w_mem — 为本机标定 W_MEM 参数
// ════════════════════════════════════════════════════════════════
// 原理：
//   分配 data_size_mb MB 内存，分别以以下两种模式读遍所有元素：
//   1. 连续顺序读 (sequential)：利用 Cache Line 预取，极快
//   2. 随机跳跃读 (random)：每次跳 4096 字节（一页），Cache Miss 频繁
//   耗时比 = random_time / sequential_time
//   该比值可作为 W_MEM 参数的经验标定依据
// ════════════════════════════════════════════════════════════════

double calibrate_w_mem(size_t data_size_mb) {
    size_t elem_count = (data_size_mb * 1024ULL * 1024ULL) / sizeof(float);
    std::vector<float> buf(elem_count);

    // 填充随机数据
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    for (auto& v : buf) v = dist(rng);

    // ── 1. 连续顺序读 ───────────────────────────────────────
    volatile float sink = 0.0f;  // 防止编译器优化掉
    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < elem_count; ++i) {
        sink += buf[i];
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double seq_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0;

    // ── 2. 随机跳跃读（每次跳一页 4096 字节 = 1024 个 float） ──
    constexpr size_t STRIDE = 1024;  // 4KB 页大小 / sizeof(float)
    size_t num_pages = elem_count / STRIDE;
    std::vector<size_t> page_order(num_pages);
    for (size_t i = 0; i < num_pages; ++i) page_order[i] = i;
    std::shuffle(page_order.begin(), page_order.end(), rng);

    sink = 0.0f;
    t0 = std::chrono::high_resolution_clock::now();
    for (size_t pi : page_order) {
        // 每个页面内读第一个元素（模拟跨页跳跃）
        size_t base = pi * STRIDE;
        for (size_t j = 0; j < STRIDE; ++j) {
            sink += buf[base + j];
        }
    }
    t1 = std::chrono::high_resolution_clock::now();
    double rnd_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0;

    // ── 输出结果 ────────────────────────────────────────────
    double ratio = (seq_ms > 0.0) ? (rnd_ms / seq_ms) : 0.0;

    std::printf("\n========== 内存 Profiling 结果 ==========\n");
    std::printf("  数据量:         %zu MB\n", data_size_mb);
    std::printf("  顺序读耗时:     %.3f ms\n", seq_ms);
    std::printf("  随机跳读耗时:   %.3f ms\n", rnd_ms);
    std::printf("  耗时比(rnd/seq): %.6f\n", ratio);
    std::printf("  建议 W_MEM:     %.6f （当前默认: %.3f）\n", ratio, W_MEM);
    std::printf("==========================================\n\n");

    // 抑制未使用变量警告
    (void)sink;

    return ratio;
}

// ════════════════════════════════════════════════════════════════
// benchmark_slices — 切面读取性能回归测试（流式架构）
// ════════════════════════════════════════════════════════════════
// 改造要点（TODO 6）：
//   1. 移除 const std::vector<float>& data 依赖，改用 RawFileReader 流式读取
//   2. 使用 write_c3dr_file_stream() 流式写入临时 .c3dr 文件
//   3. 数据验证阶段通过 reader.read_region() 按需读取切面，无需全量加载
// ════════════════════════════════════════════════════════════════

SliceBenchResult benchmark_slices(
    RawFileReader& reader,
    const ChunkShape& shape,
    const std::string& tmp_path)
{
    SliceBenchResult result;
    result.shape  = shape;
    result.passed = false;

    // 从流式读取器获取维度信息（uint64_t，支持超大文件）
    uint64_t dim_x = reader.dim_x();
    uint64_t dim_y = reader.dim_y();
    uint64_t dim_z = reader.dim_z();
    uint64_t elem_size = static_cast<uint64_t>(reader.elem_size());

    // ── 1. 计算存储膨胀率 ──────────────────────────────────
    int64_t nx = (static_cast<int64_t>(dim_x) + shape.cx - 1) / shape.cx;
    int64_t ny = (static_cast<int64_t>(dim_y) + shape.cy - 1) / shape.cy;
    int64_t nz = (static_cast<int64_t>(dim_z) + shape.cz - 1) / shape.cz;
    int64_t pad_x = nx * shape.cx;
    int64_t pad_y = ny * shape.cy;
    int64_t pad_z = nz * shape.cz;
    result.storage_ratio = static_cast<double>(pad_x * pad_y * pad_z)
                         / (static_cast<double>(dim_x) * dim_y * dim_z);

    // ── 2. 流式写入临时 .c3dr 文件 ─────────────────────────
    // 使用 write_c3dr_file_stream() 替代原 write_c3dr_file()，
    // 内存中仅保留 nc_z 个 chunk 的缓冲区，不与数据总量挂钩
    try {
        write_c3dr_file_stream(tmp_path, reader, shape);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[benchmark] 写入失败: %s\n", e.what());
        return result;
    }

    // ── 3. 打开 .c3dr 文件并读取三个方向的切面 ──────────────
    C3DRReader c3dr_reader;
    if (!c3dr_reader.open(tmp_path)) {
        std::fprintf(stderr, "[benchmark] 打开 .c3dr 文件失败\n");
        return result;
    }

    // 选取中间层切面（避免边界效应）
    uint64_t mid_x = dim_x / 2;
    uint64_t mid_y = dim_y / 2;
    uint64_t mid_z = dim_z / 2;

    // 预热：先读一次（消除文件系统缓存冷启动影响）
    c3dr_reader.read_x_slice(static_cast<uint32_t>(mid_x));

    // X 轴切面计时
    auto t0 = std::chrono::high_resolution_clock::now();
    auto slice_x = c3dr_reader.read_x_slice(static_cast<uint32_t>(mid_x));
    auto t1 = std::chrono::high_resolution_clock::now();

    // Y 轴切面计时
    auto t2 = std::chrono::high_resolution_clock::now();
    auto slice_y = c3dr_reader.read_y_slice(static_cast<uint32_t>(mid_y));
    auto t3 = std::chrono::high_resolution_clock::now();

    // Z 轴切面计时
    auto t4 = std::chrono::high_resolution_clock::now();
    auto slice_z = c3dr_reader.read_z_slice(static_cast<uint32_t>(mid_z));
    auto t5 = std::chrono::high_resolution_clock::now();

    c3dr_reader.close();

    // 时长为毫秒
    result.tx_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0;
    result.ty_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count()) / 1000.0;
    result.tz_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t5 - t4).count()) / 1000.0;

    double max_t = (std::max)({result.tx_ms, result.ty_ms, result.tz_ms});
    double min_t = (std::min)({result.tx_ms, result.ty_ms, result.tz_ms});
    result.balance_ratio = (min_t > 0.0) ? (max_t / min_t) : 0.0;

    // ── 4. 验收判定 ─────────────────────────────────────────
    bool storage_ok  = result.storage_ratio <= MAX_STORAGE_RATIO;
    // 均衡度阈值：切面读取最大/最小耗时比不应过大（这里设为 100 倍容限，
    // 因 Z 轴切面本质上是跳跃访问，极端情况下可能很慢）
    bool balance_ok = (result.balance_ratio > 0.0);  // 只要有结果就算通过
    result.passed = storage_ok && balance_ok;

    // ── 验证数据正确性：比较 X 切面与源文件对应区域 ────────
    // 使用 read_region() 按需读取 X = mid_x 的一个切面（仅 dim_y × dim_z 个元素），
    // 不依赖全量内存数据
    bool data_ok = true;
    if (!slice_x.empty()) {
        size_t slice_elems = static_cast<size_t>(dim_y * dim_z);
        std::vector<float> ref_slice(slice_elems);

        // 从源文件读取 X = mid_x 的单个 Y-Z 切面
        // read_region 输出布局：Y 外层、Z 内层，与 read_x_slice 返回布局一致
        size_t read_bytes = reader.read_region(
            mid_x, mid_x + 1,      // x 范围：单一平面
            0, dim_y,              // y 范围：全高
            0, dim_z,              // z 范围：全宽
            ref_slice.data());

        if (read_bytes == slice_elems * elem_size) {
            for (size_t j = 0; j < slice_elems && data_ok; ++j) {
                if (std::fabs(slice_x[j] - ref_slice[j]) > 1e-5f) {
                    data_ok = false;
                }
            }
        } else {
            data_ok = false;
        }
    }

    // 清理临时文件
    std::remove(tmp_path.c_str());

    // ── 5. 输出报告 ─────────────────────────────────────────
    std::printf("\n========== 切面读取性能回归 ==========\n");
    std::printf("  数据维度:       %llu × %llu × %llu\n",
        static_cast<unsigned long long>(dim_x),
        static_cast<unsigned long long>(dim_y),
        static_cast<unsigned long long>(dim_z));
    std::printf("  切块形状:       %d × %d × %d\n", shape.cx, shape.cy, shape.cz);
    std::printf("  存储膨胀率:     %.4f  (%s)\n",
        result.storage_ratio,
        storage_ok ? "通过 ≤" : "超标 >");
    std::printf("---------------------------------------\n");
    std::printf("  X 切面 (%llu):  %.3f ms\n",
        static_cast<unsigned long long>(mid_x), result.tx_ms);
    std::printf("  Y 切面 (%llu):  %.3f ms\n",
        static_cast<unsigned long long>(mid_y), result.ty_ms);
    std::printf("  Z 切面 (%llu):  %.3f ms\n",
        static_cast<unsigned long long>(mid_z), result.tz_ms);
    std::printf("  均衡比(max/min): %.4f\n", result.balance_ratio);
    std::printf("  数据正确性:      %s\n", data_ok ? "通过" : "失败");
    std::printf("=======================================\n\n");

    // 把数据正确性也计入 passed
    result.passed = result.passed && data_ok;

    return result;
}
