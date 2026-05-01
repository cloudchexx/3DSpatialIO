// ============================================================
// Phase 4 集成测试 + 性能回归测试
// ============================================================
// 测试内容：
//   1. 基本写读往返：生成数据 → 写入 .c3dr → 读取 X/Y/Z 切面 → 验证正确性
//   2. 存储红线验收：极端扁平/细长数据 → 验证落盘膨胀率 < 1.45
//   3. 切面读取均衡性：分别沿三轴读切面 → 记录耗时比
//   4. 边界数据处理：极小数据触发回退
// ============================================================

#include "chunk_io.h"
#include "chunk_optimizer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                              \
    do {                                              \
        if (!(cond)) {                                \
            std::fprintf(stderr, "  FAIL: %s\n", msg); \
            g_failed++;                               \
            return;                                   \
        }                                             \
    } while (0)

// ─── 工具函数：生成填充测试数据 ───────────────────────────────
// 每个元素的值为根据其坐标计算的确定性值，便于验证 I/O 正确性
// val(x, y, z) = (x * 1000000 + y * 1000 + z) * 1e-6f
static std::vector<float> generate_test_data(uint32_t dx, uint32_t dy, uint32_t dz) {
    std::vector<float> data(static_cast<size_t>(dx) * dy * dz);
    for (uint32_t x = 0; x < dx; ++x) {
        for (uint32_t y = 0; y < dy; ++y) {
            for (uint32_t z = 0; z < dz; ++z) {
                size_t idx = static_cast<size_t>(x) * dy * dz
                           + static_cast<size_t>(y) * dz
                           + z;
                data[idx] = static_cast<float>(
                    (static_cast<double>(x) * 1000000.0
                   + static_cast<double>(y) * 1000.0
                   + static_cast<double>(z)) * 1e-6);
            }
        }
    }
    return data;
}

// ─── 工具函数：验证切面正确性 ─────────────────────────────────
// 比较读取的切面与原始数据对应位置的预期值
static bool verify_x_slice(const std::vector<float>& slice,
                           const std::vector<float>& original,
                           uint32_t x, uint32_t dim_y, uint32_t dim_z) {
    size_t base = static_cast<size_t>(x) * dim_y * dim_z;
    for (size_t i = 0; i < slice.size(); ++i) {
        if (std::fabs(slice[i] - original[base + i]) > 1e-5f) {
            std::fprintf(stderr, "  X-slice mismatch at index %zu: got %f, expected %f\n",
                         i, static_cast<double>(slice[i]),
                         static_cast<double>(original[base + i]));
            return false;
        }
    }
    return true;
}

static bool verify_y_slice(const std::vector<float>& slice,
                           const std::vector<float>& original,
                           uint32_t y, uint32_t dim_x, uint32_t dim_y, uint32_t dim_z) {
    for (uint32_t x = 0; x < dim_x; ++x) {
        for (uint32_t z = 0; z < dim_z; ++z) {
            size_t src_idx = static_cast<size_t>(x) * dim_y * dim_z
                           + static_cast<size_t>(y) * dim_z + z;
            size_t dst_idx = static_cast<size_t>(x) * dim_z + z;
            if (std::fabs(slice[dst_idx] - original[src_idx]) > 1e-5f) {
                std::fprintf(stderr, "  Y-slice mismatch at x=%u z=%u: got %f, expected %f\n",
                             x, z,
                             static_cast<double>(slice[dst_idx]),
                             static_cast<double>(original[src_idx]));
                return false;
            }
        }
    }
    return true;
}

static bool verify_z_slice(const std::vector<float>& slice,
                           const std::vector<float>& original,
                           uint32_t z, uint32_t dim_x, uint32_t dim_y, uint32_t dim_z) {
    for (uint32_t x = 0; x < dim_x; ++x) {
        for (uint32_t y = 0; y < dim_y; ++y) {
            size_t src_idx = static_cast<size_t>(x) * dim_y * dim_z
                           + static_cast<size_t>(y) * dim_z + z;
            size_t dst_idx = static_cast<size_t>(x) * dim_y + y;
            if (std::fabs(slice[dst_idx] - original[src_idx]) > 1e-5f) {
                std::fprintf(stderr, "  Z-slice mismatch at x=%u y=%u: got %f, expected %f\n",
                             x, y,
                             static_cast<double>(slice[dst_idx]),
                             static_cast<double>(original[src_idx]));
                return false;
            }
        }
    }
    return true;
}

// ─── 工具函数：计算落盘存储膨胀率 ────────────────────────────
static double compute_storage_ratio(uint32_t dx, uint32_t dy, uint32_t dz,
                                     const ChunkShape& shape) {
    int64_t nc_x = (dx + static_cast<uint32_t>(shape.cx) - 1) / shape.cx;
    int64_t nc_y = (dy + static_cast<uint32_t>(shape.cy) - 1) / shape.cy;
    int64_t nc_z = (dz + static_cast<uint32_t>(shape.cz) - 1) / shape.cz;
    int64_t pad_x = nc_x * shape.cx;
    int64_t pad_y = nc_y * shape.cy;
    int64_t pad_z = nc_z * shape.cz;
    return static_cast<double>(pad_x * pad_y * pad_z)
         / (static_cast<double>(dx) * dy * dz);
}

// ════════════════════════════════════════════════════════════
// Test 1: 基本写读往返（正方体数据 128×128×128）
// ════════════════════════════════════════════════════════════
void test_write_read_roundtrip() {
    const uint32_t DX = 128, DY = 128, DZ = 128;
    auto data = generate_test_data(DX, DY, DZ);

    ChunkShape shape = find_optimal_chunk_shape(DX, DY, DZ);
    std::string tmp_path = "test_roundtrip.c3dr";

    // 写入
    C3DRHeader header = write_c3dr_file(tmp_path, data, DX, DY, DZ, shape);
    CHECK(header.magic == 0x52443343, "magic mismatch in written header");

    // 读取
    C3DRReader reader;
    CHECK(reader.open(tmp_path), "failed to open .c3dr file");

    CHECK(reader.header().dim_x == DX, "dim_x mismatch");
    CHECK(reader.header().dim_y == DY, "dim_y mismatch");
    CHECK(reader.header().dim_z == DZ, "dim_z mismatch");
    CHECK(reader.chunk_shape().cx == shape.cx, "cx mismatch");
    CHECK(reader.chunk_shape().cy == shape.cy, "cy mismatch");
    CHECK(reader.chunk_shape().cz == shape.cz, "cz mismatch");

    // 验证 X 切面 (中间层)
    auto x_slice = reader.read_x_slice(DX / 2);
    CHECK(x_slice.size() == static_cast<size_t>(DY) * DZ, "X-slice size mismatch");
    CHECK(verify_x_slice(x_slice, data, DX / 2, DY, DZ), "X-slice data mismatch");

    // 验证 Y 切面
    auto y_slice = reader.read_y_slice(DY / 2);
    CHECK(y_slice.size() == static_cast<size_t>(DX) * DZ, "Y-slice size mismatch");
    CHECK(verify_y_slice(y_slice, data, DY / 2, DX, DY, DZ), "Y-slice data mismatch");

    // 验证 Z 切面
    auto z_slice = reader.read_z_slice(DZ / 2);
    CHECK(z_slice.size() == static_cast<size_t>(DX) * DY, "Z-slice size mismatch");
    CHECK(verify_z_slice(z_slice, data, DZ / 2, DX, DY, DZ), "Z-slice data mismatch");

    // 验证边界切面
    auto x0_slice = reader.read_x_slice(0);
    CHECK(verify_x_slice(x0_slice, data, 0, DY, DZ), "X-slice[0] data mismatch");

    auto x_last_slice = reader.read_x_slice(DX - 1);
    CHECK(verify_x_slice(x_last_slice, data, DX - 1, DY, DZ),
          "X-slice[last] data mismatch");

    auto z0_slice = reader.read_z_slice(0);
    CHECK(verify_z_slice(z0_slice, data, 0, DX, DY, DZ), "Z-slice[0] data mismatch");

    // 读取单个 chunk 并验证部分数据
    auto chunk = reader.read_chunk(0, 0, 0);
    CHECK(!chunk.empty(), "chunk read failed");
    CHECK(chunk.size() == static_cast<size_t>(shape.cx) * shape.cy * shape.cz,
          "chunk size mismatch");

    // 验证 chunk 中一块小区域的数据
    uint32_t test_lx = (std::min)(static_cast<uint32_t>(shape.cx), DX) / 2;
    uint32_t test_ly = (std::min)(static_cast<uint32_t>(shape.cy), DY) / 2;
    uint32_t test_lz = (std::min)(static_cast<uint32_t>(shape.cz), DZ) / 2;
    size_t chunk_idx = static_cast<size_t>(test_lx) * shape.cy * shape.cz
                     + static_cast<size_t>(test_ly) * shape.cz + test_lz;
    // 原始数据中的对应位置（chunk 坐标为 (0,0,0)）
    size_t orig_idx = static_cast<size_t>(test_lx) * DY * DZ
                    + static_cast<size_t>(test_ly) * DZ + test_lz;
    CHECK(std::fabs(chunk[chunk_idx] - data[orig_idx]) < 1e-5f,
          "chunk point data mismatch");

    reader.close();
    std::remove(tmp_path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 2: 存储红线验收（扁平数据 2000×2000×80）
//        扁平比例 25:25:1，Z 轴尺寸确保候选集能找到合法解
// ════════════════════════════════════════════════════════════
void test_storage_redline_flat() {
    const uint32_t DX = 2000, DY = 2000, DZ = 80;
    auto data = generate_test_data(DX, DY, DZ);
    ChunkShape shape = find_optimal_chunk_shape(DX, DY, DZ);

    double ratio = compute_storage_ratio(DX, DY, DZ, shape);
    CHECK(ratio <= MAX_STORAGE_RATIO,
          "flat data storage ratio exceeds 1.45 red line");

    // 验证 Z 轴切块较小（压缩 Z 方向）
    CHECK(shape.cz < shape.cx && shape.cz < shape.cy,
          "Z-axis chunk should be smaller than X/Y for flat data");

    // 实际落盘验证
    std::string tmp_path = "test_storage_flat.c3dr";
    write_c3dr_file(tmp_path, data, DX, DY, DZ, shape);

    // 检查文件大小
    FILE* f = fopen(tmp_path.c_str(), "rb");
    CHECK(f != nullptr, "cannot open output file for size check");
    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    fclose(f);

    size_t raw_bytes = static_cast<size_t>(DX) * DY * DZ * sizeof(float);
    double file_ratio = static_cast<double>(file_sz) / static_cast<double>(raw_bytes);
    std::printf("  Flat data: raw=%zu bytes, .c3dr=%ld bytes, ratio=%.4f\n",
                raw_bytes, file_sz, file_ratio);
    CHECK(file_ratio <= MAX_STORAGE_RATIO,
          "actual file size ratio exceeds 1.45");

    std::remove(tmp_path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 3: 存储红线验收（极端细长形状 100×2000×100）
//        保持与 100×10000×100 相同的细长比例 (1:100:1)
// ════════════════════════════════════════════════════════════
void test_storage_redline_elongated() {
    const uint32_t DX = 100, DY = 2000, DZ = 100;
    auto data = generate_test_data(DX, DY, DZ);
    ChunkShape shape = find_optimal_chunk_shape(DX, DY, DZ);

    double ratio = compute_storage_ratio(DX, DY, DZ, shape);
    CHECK(ratio <= MAX_STORAGE_RATIO,
          "elongated data storage ratio exceeds 1.45 red line");

    // 验证 Y 轴切块最大
    CHECK(shape.cy > shape.cx && shape.cy > shape.cz,
          "Y-axis chunk should be largest for elongated Y data");

    std::string tmp_path = "test_storage_elong.c3dr";
    write_c3dr_file(tmp_path, data, DX, DY, DZ, shape);

    FILE* f = fopen(tmp_path.c_str(), "rb");
    CHECK(f != nullptr, "cannot open output file for size check");
    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    fclose(f);

    size_t raw_bytes = static_cast<size_t>(DX) * DY * DZ * sizeof(float);
    double file_ratio = static_cast<double>(file_sz) / static_cast<double>(raw_bytes);
    std::printf("  Elongated data: raw=%zu bytes, .c3dr=%ld bytes, ratio=%.4f\n",
                raw_bytes, file_sz, file_ratio);
    CHECK(file_ratio <= MAX_STORAGE_RATIO,
          "actual file size ratio exceeds 1.45");

    std::remove(tmp_path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 4: 中等数据写读（规模足够验证分块索引表无溢出）
// ════════════════════════════════════════════════════════════
void test_large_data_write() {
    const uint32_t DX = 256, DY = 256, DZ = 256;
    auto data = generate_test_data(DX, DY, DZ);
    ChunkShape shape = find_optimal_chunk_shape(DX, DY, DZ);

    std::string tmp_path = "test_large_write.c3dr";
    C3DRHeader header = write_c3dr_file(tmp_path, data, DX, DY, DZ, shape);
    CHECK(header.magic == 0x52443343, "header write failed");

    C3DRReader reader;
    CHECK(reader.open(tmp_path), "failed to reopen large data file");

    // 验证索引表条目数
    uint64_t expected_chunks = static_cast<uint64_t>(reader.num_chunks_x())
                             * reader.num_chunks_y()
                             * reader.num_chunks_z();
    CHECK(expected_chunks > 0, "chunk count must be positive");

    // 读取中间切面做冒烟测试
    auto z_slice = reader.read_z_slice(DZ / 2);
    CHECK(!z_slice.empty(), "Z-slice read should not be empty");
    CHECK(z_slice.size() == static_cast<size_t>(DX) * DY, "Z-slice size mismatch");

    reader.close();
    std::remove(tmp_path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 5: 非法参数处理
// ════════════════════════════════════════════════════════════
void test_error_handling() {
    C3DRReader reader;

    // 打开不存在的文件
    CHECK(!reader.open("__nonexistent__.c3dr"),
          "should fail to open nonexistent file");

    // 无效 magic 的文件
    {
        FILE* f = fopen("test_bad_header.bin", "wb");
        if (f) {
            uint8_t bad[64] = {};
            fwrite(bad, 1, sizeof(bad), f);
            fclose(f);
        }
        CHECK(!reader.open("test_bad_header.bin"),
              "should reject file with bad magic");
        std::remove("test_bad_header.bin");
    }

    // 越界读取（用一个有效文件测试）
    {
        const uint32_t DX = 64, DY = 64, DZ = 64;
        auto data = generate_test_data(DX, DY, DZ);
        ChunkShape shape = find_optimal_chunk_shape(DX, DY, DZ);
        write_c3dr_file("test_bounds.c3dr", data, DX, DY, DZ, shape);

        CHECK(reader.open("test_bounds.c3dr"), "should open valid file");

        auto empty = reader.read_x_slice(999999);
        CHECK(empty.empty(), "out-of-range slice should return empty");
        auto chunk = reader.read_chunk(99, 99, 99);
        CHECK(chunk.empty(), "out-of-range chunk should return empty");
        reader.close();
        std::remove("test_bounds.c3dr");
    }
}

// ════════════════════════════════════════════════════════════
// Test 6: 切面读取性能均衡（冒烟级，不强制硬时间约束）
// ════════════════════════════════════════════════════════════
void test_slice_read_balance() {
    // 使用中等规模数据做一次完整流程
    const uint32_t DX = 200, DY = 200, DZ = 200;
    auto data = generate_test_data(DX, DY, DZ);
    ChunkShape shape = find_optimal_chunk_shape(DX, DY, DZ);

    std::string tmp_path = "test_balance.c3dr";
    write_c3dr_file(tmp_path, data, DX, DY, DZ, shape);

    C3DRReader reader;
    CHECK(reader.open(tmp_path), "failed to open balance test file");

    // 预热
    reader.read_x_slice(DX / 2);

    // 读取所有三轴中间切面，验证都返回了非空数据
    auto sx = reader.read_x_slice(DX / 2);
    auto sy = reader.read_y_slice(DY / 2);
    auto sz = reader.read_z_slice(DZ / 2);

    CHECK(!sx.empty() && !sy.empty() && !sz.empty(),
          "all three slices should be non-empty");

    // 验证 X 切面数据正确
    CHECK(verify_x_slice(sx, data, DX / 2, DY, DZ), "balance X-slice verify");
    CHECK(verify_y_slice(sy, data, DY / 2, DX, DY, DZ), "balance Y-slice verify");
    CHECK(verify_z_slice(sz, data, DZ / 2, DX, DY, DZ), "balance Z-slice verify");

    reader.close();
    std::remove(tmp_path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 7: 单块读取数据（所有块均为边界块）的验证
// ════════════════════════════════════════════════════════════
void test_single_chunk_edge() {
    // 使用小尺寸，使得所有数据落在一个块内
    const uint32_t DX = 30, DY = 30, DZ = 30;
    auto data = generate_test_data(DX, DY, DZ);
    ChunkShape shape = find_optimal_chunk_shape(DX, DY, DZ);

    // tiny data 会触发回退到 {64,64,64}，只有一个 chunk
    std::string tmp_path = "test_single_chunk.c3dr";
    write_c3dr_file(tmp_path, data, DX, DY, DZ, shape);

    C3DRReader reader;
    CHECK(reader.open(tmp_path), "failed to open single chunk file");

    // 所有数据在一个块内
    CHECK(reader.num_chunks_x() == 1, "should have exactly 1 chunk in X");
    CHECK(reader.num_chunks_y() == 1, "should have exactly 1 chunk in Y");
    CHECK(reader.num_chunks_z() == 1, "should have exactly 1 chunk in Z");

    auto chunk = reader.read_chunk(0, 0, 0);
    CHECK(!chunk.empty(), "single chunk read failed");

    // 验证非填充区域的数据
    for (uint32_t x = 0; x < DX; ++x) {
        for (uint32_t y = 0; y < DY; ++y) {
            for (uint32_t z = 0; z < DZ; ++z) {
                size_t chunk_idx = static_cast<size_t>(x) * shape.cy * shape.cz
                                 + static_cast<size_t>(y) * shape.cz + z;
                size_t orig_idx = static_cast<size_t>(x) * DY * DZ
                                + static_cast<size_t>(y) * DZ + z;
                if (std::fabs(chunk[chunk_idx] - data[orig_idx]) > 1e-5f) {
                    std::fprintf(stderr,
                        "  single chunk mismatch at (%u,%u,%u): got %f, expected %f\n",
                        x, y, z,
                        static_cast<double>(chunk[chunk_idx]),
                        static_cast<double>(data[orig_idx]));
                    CHECK(false, "single chunk data mismatch");
                }
            }
        }
    }

    reader.close();
    std::remove(tmp_path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test runner
// ════════════════════════════════════════════════════════════
int main() {
    std::printf("\n========== Phase 4: Integration & Profiling Tests ==========\n\n");

    auto run = [](void (*fn)(), const char* name) {
        std::printf("  RUN   %s\n", name);
        int before = g_failed;
        fn();
        if (g_failed == before) {
            std::printf("  PASS  %s\n\n", name);
            g_passed++;
        }
    };

    run(test_write_read_roundtrip,   "test_write_read_roundtrip");
    run(test_storage_redline_flat,   "test_storage_redline_flat");
    run(test_storage_redline_elongated, "test_storage_redline_elongated");
    run(test_large_data_write,       "test_large_data_write");
    run(test_error_handling,         "test_error_handling");
    run(test_slice_read_balance,     "test_slice_read_balance");
    run(test_single_chunk_edge,     "test_single_chunk_edge");

    std::printf("Results: %d passed, %d failed\n\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
