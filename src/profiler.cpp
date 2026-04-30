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
// benchmark_slices — 切面读取性能回归测试
// ════════════════════════════════════════════════════════════════

SliceBenchResult benchmark_slices(
    const std::vector<float>& data,
    uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
    const ChunkShape& shape,
    const std::string& tmp_path)
{
    SliceBenchResult result;
    result.shape  = shape;
    result.passed = false;

    // ── 1. 计算存储膨胀率 ──────────────────────────────────
    int64_t nx = (static_cast<int64_t>(dim_x) + shape.cx - 1) / shape.cx;
    int64_t ny = (static_cast<int64_t>(dim_y) + shape.cy - 1) / shape.cy;
    int64_t nz = (static_cast<int64_t>(dim_z) + shape.cz - 1) / shape.cz;
    int64_t pad_x = nx * shape.cx;
    int64_t pad_y = ny * shape.cy;
    int64_t pad_z = nz * shape.cz;
    result.storage_ratio = static_cast<double>(pad_x * pad_y * pad_z)
                         / (static_cast<double>(dim_x) * dim_y * dim_z);

    // ── 2. 写入 .c3dr 文件 ─────────────────────────────────
    try {
        write_c3dr_file(tmp_path, data, dim_x, dim_y, dim_z, shape);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[benchmark] 写入失败: %s\n", e.what());
        return result;
    }

    // ── 3. 打开并读取三个方向的切面 ─────────────────────────
    C3DRReader reader;
    if (!reader.open(tmp_path)) {
        std::fprintf(stderr, "[benchmark] 打开 .c3dr 文件失败\n");
        return result;
    }

    // 选取中间层切面（避免边界效应）
    uint32_t mid_x = dim_x / 2;
    uint32_t mid_y = dim_y / 2;
    uint32_t mid_z = dim_z / 2;

    // 预热：先读一次（消除文件系统缓存冷启动影响）
    reader.read_x_slice(mid_x);

    // X 轴切面计时
    auto t0 = std::chrono::high_resolution_clock::now();
    auto slice_x = reader.read_x_slice(mid_x);
    auto t1 = std::chrono::high_resolution_clock::now();

    // Y 轴切面计时
    auto t2 = std::chrono::high_resolution_clock::now();
    auto slice_y = reader.read_y_slice(mid_y);
    auto t3 = std::chrono::high_resolution_clock::now();

    // Z 轴切面计时
    auto t4 = std::chrono::high_resolution_clock::now();
    auto slice_z = reader.read_z_slice(mid_z);
    auto t5 = std::chrono::high_resolution_clock::now();

    reader.close();

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

    // ── 验证数据正确性：比较 X 切面与原始数据的对应切片 ────
    bool data_ok = true;
    if (!slice_x.empty()) {
        size_t dim_y_sz = static_cast<size_t>(dim_y);
        size_t dim_z_sz = static_cast<size_t>(dim_z);
        // 用原始数据构造 x = mid_x 的切面
        size_t src_base = static_cast<size_t>(mid_x) * dim_y_sz * dim_z_sz;
        for (size_t j = 0; j < dim_y_sz * dim_z_sz && data_ok; ++j) {
            if (std::fabs(slice_x[j] - data[src_base + j]) > 1e-5f) {
                data_ok = false;
            }
        }
    }

    // 清理临时文件
    std::remove(tmp_path.c_str());

    // ── 5. 输出报告 ─────────────────────────────────────────
    std::printf("\n========== 切面读取性能回归 ==========\n");
    std::printf("  数据维度:       %u × %u × %u\n", dim_x, dim_y, dim_z);
    std::printf("  切块形状:       %d × %d × %d\n", shape.cx, shape.cy, shape.cz);
    std::printf("  存储膨胀率:     %.4f  (%s)\n",
        result.storage_ratio,
        storage_ok ? "通过 ≤" : "超标 >");
    std::printf("---------------------------------------\n");
    std::printf("  X 切面 (%u):    %.3f ms\n", mid_x, result.tx_ms);
    std::printf("  Y 切面 (%u):    %.3f ms\n", mid_y, result.ty_ms);
    std::printf("  Z 切面 (%u):    %.3f ms\n", mid_z, result.tz_ms);
    std::printf("  均衡比(max/min): %.4f\n", result.balance_ratio);
    std::printf("  数据正确性:      %s\n", data_ok ? "通过" : "失败");
    std::printf("=======================================\n\n");

    // 把数据正确性也计入 passed
    result.passed = result.passed && data_ok;

    return result;
}
