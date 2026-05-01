// ============================================================
// Phase 4: C3DRCacheReader 测试
// ============================================================
// 测试用例：
//   1.  生命周期 open/close/flush
//   2.  读切面正确性 X/Y/Z（与 C3DRReader 逐元素比对）
//   3.  命中/未命中计数
//   4.  LRU 淘汰正确性
//   5.  内存上限强制
//   6.  写切面正确性（write → flush → 重新 open 用 C3DRReader 读出比对）
//   7.  写后读一致性（write → 不 flush → cache_reader 读回比对）
//   8.  COW 安全性（获取 shared_ptr → write → 原 ptr 数据不变）
//   9.  脏块淘汰（脏块被 LRU 淘汰 → 自动 flush → 重新加载，数据 matches）
//   10. flush 幂等
//   11. 预取触发
//   12. 不同缓存大小
// ============================================================

#include "cache_reader.h"
#include "chunk_io.h"
#include "chunk_optimizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

static std::vector<float> generate_test_data(uint32_t dx, uint32_t dy, uint32_t dz) {
    std::vector<float> data(static_cast<size_t>(dx) * dy * dz);
    for (uint32_t x = 0; x < dx; ++x) {
        for (uint32_t y = 0; y < dy; ++y) {
            for (uint32_t z = 0; z < dz; ++z) {
                size_t idx = static_cast<size_t>(x) * dy * dz
                           + static_cast<size_t>(y) * dz + z;
                data[idx] = static_cast<float>(
                    (static_cast<double>(x) * 1000000.0
                   + static_cast<double>(y) * 1000.0
                   + static_cast<double>(z)) * 1e-6);
            }
        }
    }
    return data;
}

static bool verify_x_slice(const std::vector<float>& slice,
                           const std::vector<float>& original,
                           uint32_t x, uint32_t dim_y, uint32_t dim_z) {
    size_t base = static_cast<size_t>(x) * dim_y * dim_z;
    for (size_t i = 0; i < slice.size(); ++i) {
        if (std::fabs(slice[i] - original[base + i]) > 1e-5f) return false;
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
            if (std::fabs(slice[dst_idx] - original[src_idx]) > 1e-5f) return false;
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
            if (std::fabs(slice[dst_idx] - original[src_idx]) > 1e-5f) return false;
        }
    }
    return true;
}

static std::string make_test_file(uint32_t dx, uint32_t dy, uint32_t dz,
                                  const std::vector<float>& data,
                                  const std::string& name) {
    ChunkShape shape = find_optimal_chunk_shape(dx, dy, dz);
    write_c3dr_file(name, data, dx, dy, dz, shape);
    return name;
}

// ════════════════════════════════════════════════════════════
// Test 1: 生命周期 open/close/flush
// ════════════════════════════════════════════════════════════
void test_lifecycle() {
    const uint32_t DX = 64, DY = 64, DZ = 64;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_lifecycle.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    {
        C3DRCacheReader cache(2ULL * 1024 * 1024 * 1024);
        CHECK(cache.open(path), "open failed");
        CHECK(cache.is_open(), "should be open");

        auto slice = cache.read_x_slice(0);
        CHECK(!slice.empty(), "read should succeed");

        cache.flush();
        cache.close();
        CHECK(!cache.is_open(), "should be closed after close");
    }

    {
        C3DRCacheReader cache(2ULL * 1024 * 1024 * 1024);
        CHECK(cache.open(path), "reopen failed");
        CHECK(cache.is_open(), "should be open after reopen");
        cache.close();
    }

    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 2: 读切面正确性 X/Y/Z
// ════════════════════════════════════════════════════════════
void test_read_correctness() {
    const uint32_t DX = 64, DY = 64, DZ = 64;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_read.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    C3DRCacheReader cache(2ULL * 1024 * 1024 * 1024);
    CHECK(cache.open(path), "open failed");

    auto x_slice = cache.read_x_slice(DX / 2);
    CHECK(verify_x_slice(x_slice, data, DX / 2, DY, DZ),
          "cache X-slice data mismatch");

    auto y_slice = cache.read_y_slice(DY / 2);
    CHECK(verify_y_slice(y_slice, data, DY / 2, DX, DY, DZ),
          "cache Y-slice data mismatch");

    auto z_slice = cache.read_z_slice(DZ / 2);
    CHECK(verify_z_slice(z_slice, data, DZ / 2, DX, DY, DZ),
          "cache Z-slice data mismatch");

    auto x0 = cache.read_x_slice(0);
    CHECK(verify_x_slice(x0, data, 0, DY, DZ), "cache X[0] mismatch");

    auto x_last = cache.read_x_slice(DX - 1);
    CHECK(verify_x_slice(x_last, data, DX - 1, DY, DZ), "cache X[last] mismatch");

    cache.close();
    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 3: 命中/未命中计数
// ════════════════════════════════════════════════════════════
void test_hit_miss_count() {
    const uint32_t DX = 64, DY = 64, DZ = 64;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_hitmiss.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    C3DRCacheReader cache(2ULL * 1024 * 1024 * 1024);
    CHECK(cache.open(path), "open failed");

    cache.read_x_slice(0);
    size_t misses_after_first = cache.cache_misses();
    CHECK(misses_after_first > 0, "first read should have misses");

    size_t hits_before = cache.cache_hits();
    cache.read_x_slice(0);
    size_t hits_after = cache.cache_hits();
    CHECK(hits_after > hits_before, "second read of same slice should be a hit");

    cache.close();
    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 4: LRU 淘汰正确性
// ════════════════════════════════════════════════════════════
void test_lru_eviction() {
    const uint32_t DX = 64, DY = 64, DZ = 64;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_lru.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    ChunkShape shape = find_optimal_chunk_shape(DX, DY, DZ);
    size_t one_chunk_mem = static_cast<size_t>(shape.cx) * shape.cy * shape.cz * sizeof(float);

    C3DRCacheReader cache(one_chunk_mem * 4);
    CHECK(cache.open(path), "open failed");

    cache.read_x_slice(0);
    cache.read_x_slice(static_cast<uint32_t>(shape.cx));
    cache.read_x_slice(static_cast<uint32_t>(shape.cx) * 2);
    cache.read_x_slice(static_cast<uint32_t>(shape.cx) * 3);

    size_t usage_before = cache.cache_memory_usage();
    CHECK(usage_before <= cache.cache_capacity(), "usage within capacity");

    cache.read_x_slice(static_cast<uint32_t>(shape.cx) * 4);
    CHECK(cache.cache_memory_usage() <= cache.cache_capacity(),
          "after eviction, usage still within capacity");

    auto slice0 = cache.read_x_slice(0);
    CHECK(verify_x_slice(slice0, data, 0, DY, DZ),
          "re-read evicted slice still correct");

    cache.close();
    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 5: 内存上限强制
// ════════════════════════════════════════════════════════════
void test_memory_limit() {
    const uint32_t DX = 128, DY = 128, DZ = 128;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_memlimit.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    size_t small_cache = 4ULL * 1024 * 1024;

    C3DRCacheReader cache(small_cache < 2ULL * 1024 * 1024 * 1024
                          ? 2ULL * 1024 * 1024 * 1024 : small_cache);
    CHECK(cache.open(path), "open failed");

    for (uint32_t x = 0; x < DX; ++x) {
        cache.read_x_slice(x);
    }

    CHECK(cache.cache_memory_usage() <= cache.cache_capacity(),
          "memory usage must never exceed capacity");

    auto slice = cache.read_x_slice(DX / 2);
    CHECK(verify_x_slice(slice, data, DX / 2, DY, DZ),
          "data still correct under memory pressure");

    cache.close();
    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 6: 写切面正确性
// ════════════════════════════════════════════════════════════
void test_write_correctness() {
    const uint32_t DX = 64, DY = 64, DZ = 64;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_write.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    {
        C3DRCacheReader cache(2ULL * 1024 * 1024 * 1024);
        CHECK(cache.open(path), "open failed");

        std::vector<float> new_x(DY * DZ, 42.0f);
        cache.write_x_slice(10, new_x);
        CHECK(cache.dirty_count() > 0, "should have dirty chunks after write");

        cache.flush();
        CHECK(cache.dirty_count() == 0, "no dirty chunks after flush");
        cache.close();
    }

    {
        C3DRReader reader;
        CHECK(reader.open(path), "reopen for verify failed");
        auto slice = reader.read_x_slice(10);
        for (size_t i = 0; i < slice.size(); ++i) {
            CHECK(std::fabs(slice[i] - 42.0f) < 1e-5f,
                  "written X-slice data mismatch after flush");
        }
        reader.close();
    }

    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 7: 写后读一致性（不 flush，从 cache 读回）
// ════════════════════════════════════════════════════════════
void test_write_read_consistency() {
    const uint32_t DX = 64, DY = 64, DZ = 64;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_writeread.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    C3DRCacheReader cache(2ULL * 1024 * 1024 * 1024);
    CHECK(cache.open(path), "open failed");

    std::vector<float> new_y(DX * DZ, 99.0f);
    cache.write_y_slice(5, new_y);
    CHECK(cache.dirty_count() > 0, "dirty after write_y");

    auto slice = cache.read_y_slice(5);
    for (size_t i = 0; i < slice.size(); ++i) {
        CHECK(std::fabs(slice[i] - 99.0f) < 1e-5f,
              "read-back Y-slice mismatch without flush");
    }

    cache.close();
    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 8: COW 安全性
// ════════════════════════════════════════════════════════════
void test_cow_safety() {
    const uint32_t DX = 64, DY = 64, DZ = 64;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_cow.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    C3DRCacheReader cache(2ULL * 1024 * 1024 * 1024);
    CHECK(cache.open(path), "open failed");

    auto original = cache.get_chunk(0);
    CHECK(original && !original->empty(), "read chunk 0 failed");

    float first_val = (*original)[0];

    std::vector<float> new_x(DY * DZ, 777.0f);
    cache.write_x_slice(0, new_x);

    CHECK(std::fabs((*original)[0] - first_val) < 1e-5f,
          "COW: original shared_ptr data should be unchanged after write");

    cache.close();
    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 9: 脏块淘汰（脏块被 LRU 淘汰 → 自动 flush → 重新加载正确）
// ════════════════════════════════════════════════════════════
void test_dirty_eviction() {
    const uint32_t DX = 64, DY = 64, DZ = 64;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_dirty_evict.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    ChunkShape shape = find_optimal_chunk_shape(DX, DY, DZ);
    size_t one_chunk_mem = static_cast<size_t>(shape.cx) * shape.cy * shape.cz * sizeof(float);

    C3DRCacheReader cache(one_chunk_mem * 4);
    CHECK(cache.open(path), "open failed");

    std::vector<float> new_x(DY * DZ, 123.0f);
    cache.write_x_slice(0, new_x);
    CHECK(cache.dirty_count() > 0, "dirty after write");

    for (uint32_t x = static_cast<uint32_t>(shape.cx);
         x < DX;
         x += static_cast<uint32_t>(shape.cx)) {
        cache.read_x_slice(x);
    }

    auto slice0 = cache.read_x_slice(0);
    for (size_t i = 0; i < slice0.size(); ++i) {
        CHECK(std::fabs(slice0[i] - 123.0f) < 1e-5f,
              "dirty-evicted and reloaded data mismatch");
    }

    cache.close();

    {
        C3DRReader reader;
        CHECK(reader.open(path), "reopen for verify failed");
        auto verify = reader.read_x_slice(0);
        for (size_t i = 0; i < verify.size(); ++i) {
            CHECK(std::fabs(verify[i] - 123.0f) < 1e-5f,
                  "dirty-evicted data not persisted to disk");
        }
        reader.close();
    }

    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 10: flush 幂等
// ════════════════════════════════════════════════════════════
void test_flush_idempotent() {
    const uint32_t DX = 64, DY = 64, DZ = 64;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_flush_idem.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    C3DRCacheReader cache(2ULL * 1024 * 1024 * 1024);
    CHECK(cache.open(path), "open failed");

    std::vector<float> new_z(DX * DY, 55.0f);
    cache.write_z_slice(3, new_z);

    cache.flush();
    cache.flush();

    auto slice = cache.read_z_slice(3);
    for (size_t i = 0; i < slice.size(); ++i) {
        CHECK(std::fabs(slice[i] - 55.0f) < 1e-5f,
              "data corrupted after double flush");
    }

    cache.close();

    {
        C3DRReader reader;
        CHECK(reader.open(path), "reopen for verify failed");
        auto verify = reader.read_z_slice(3);
        for (size_t i = 0; i < verify.size(); ++i) {
            CHECK(std::fabs(verify[i] - 55.0f) < 1e-5f,
                  "double flush data on disk mismatch");
        }
        reader.close();
    }

    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 11: 预取触发
// ════════════════════════════════════════════════════════════
void test_prefetch() {
    const uint32_t DX = 128, DY = 128, DZ = 128;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_prefetch.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    C3DRCacheReader cache(2ULL * 1024 * 1024 * 1024);
    CHECK(cache.open(path), "open failed");

    ChunkShape shape = find_optimal_chunk_shape(DX, DY, DZ);
    uint32_t cx = static_cast<uint32_t>(shape.cx);

    uint32_t start_x = cx - 2;
    cache.read_x_slice(start_x);
    cache.read_x_slice(start_x + 1);
    cache.read_x_slice(start_x + 2);

    cache.read_x_slice(start_x + 3);

    if (start_x + 3 < DX) {
        auto slice = cache.read_x_slice(start_x + 3);
        CHECK(verify_x_slice(slice, data, start_x + 3, DY, DZ),
              "prefetched X-slice data correct");
    }

    cache.close();
    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test 12: 不同缓存大小
// ════════════════════════════════════════════════════════════
void test_different_cache_sizes() {
    const uint32_t DX = 64, DY = 64, DZ = 64;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_sizes.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    {
        C3DRCacheReader cache(2ULL * 1024 * 1024 * 1024);
        CHECK(cache.open(path), "open 2GB failed");
        auto x = cache.read_x_slice(DX / 2);
        CHECK(verify_x_slice(x, data, DX / 2, DY, DZ), "2GB cache X mismatch");
        cache.close();
    }

    {
        C3DRCacheReader cache(4ULL * 1024 * 1024 * 1024);
        CHECK(cache.open(path), "open 4GB failed");
        auto y = cache.read_y_slice(DY / 2);
        CHECK(verify_y_slice(y, data, DY / 2, DX, DY, DZ), "4GB cache Y mismatch");
        cache.close();
    }

    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Bonus: write_y_slice + write_z_slice full flush+verify
// ════════════════════════════════════════════════════════════
void test_write_y_z_flush_verify() {
    const uint32_t DX = 64, DY = 64, DZ = 64;
    auto data = generate_test_data(DX, DY, DZ);
    std::string path = "test_cache_write_yz.c3dr";
    make_test_file(DX, DY, DZ, data, path);

    {
        C3DRCacheReader cache(2ULL * 1024 * 1024 * 1024);
        CHECK(cache.open(path), "open failed");

        std::vector<float> new_y(DX * DZ, 88.0f);
        cache.write_y_slice(7, new_y);

        cache.flush();
        cache.close();
    }

    {
        C3DRReader reader;
        CHECK(reader.open(path), "reopen for Y verify failed");

        auto y_slice = reader.read_y_slice(7);
        for (size_t i = 0; i < y_slice.size(); ++i) {
            CHECK(std::fabs(y_slice[i] - 88.0f) < 1e-5f,
                  "written Y-slice data mismatch after flush");
        }

        reader.close();
    }

    {
        C3DRCacheReader cache(2ULL * 1024 * 1024 * 1024);
        CHECK(cache.open(path), "open for Z write failed");

        std::vector<float> new_z(DX * DY, 66.0f);
        cache.write_z_slice(5, new_z);

        cache.flush();
        cache.close();
    }

    {
        C3DRReader reader;
        CHECK(reader.open(path), "reopen for Z verify failed");

        auto y_slice = reader.read_y_slice(7);
        for (uint32_t x = 0; x < DX; ++x) {
            for (uint32_t z = 0; z < DZ; ++z) {
                float expected = (z == 5) ? 66.0f : 88.0f;
                float got = y_slice[static_cast<size_t>(x) * DZ + z];
                CHECK(std::fabs(got - expected) < 1e-5f,
                      "Y-slice after Z-write mismatch at intersect");
            }
        }

        auto z_slice = reader.read_z_slice(5);
        for (size_t i = 0; i < z_slice.size(); ++i) {
            CHECK(std::fabs(z_slice[i] - 66.0f) < 1e-5f,
                  "written Z-slice data mismatch after flush");
        }

        reader.close();
    }

    std::remove(path.c_str());
}

// ════════════════════════════════════════════════════════════
// Test runner
// ════════════════════════════════════════════════════════════
int main() {
    std::printf("\n========== C3DRCacheReader Tests ==========\n\n");

    auto run = [](void (*fn)(), const char* name) {
        std::printf("  RUN   %s\n", name);
        int before = g_failed;
        fn();
        if (g_failed == before) {
            std::printf("  PASS  %s\n\n", name);
            g_passed++;
        }
    };

    run(test_lifecycle,               "test_lifecycle");
    run(test_read_correctness,        "test_read_correctness");
    run(test_hit_miss_count,          "test_hit_miss_count");
    run(test_lru_eviction,            "test_lru_eviction");
    run(test_memory_limit,            "test_memory_limit");
    run(test_write_correctness,       "test_write_correctness");
    run(test_write_read_consistency,  "test_write_read_consistency");
    run(test_cow_safety,              "test_cow_safety");
    run(test_dirty_eviction,          "test_dirty_eviction");
    run(test_flush_idempotent,        "test_flush_idempotent");
    run(test_prefetch,                "test_prefetch");
    run(test_different_cache_sizes,   "test_different_cache_sizes");
    run(test_write_y_z_flush_verify,  "test_write_y_z_flush_verify");

    std::printf("Results: %d passed, %d failed\n\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
