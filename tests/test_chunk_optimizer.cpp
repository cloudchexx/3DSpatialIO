#include "chunk_optimizer.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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

static bool in_candidates(int v) {
    for (size_t i = 0; i < CANDIDATES_COUNT; ++i) {
        if (CANDIDATES[i] == v) return true;
    }
    return false;
}

static double storage_ratio(int64_t X, int64_t Y, int64_t Z, const ChunkShape& s) {
    int64_t pad_x = ((X + s.cx - 1) / s.cx) * s.cx;
    int64_t pad_y = ((Y + s.cy - 1) / s.cy) * s.cy;
    int64_t pad_z = ((Z + s.cz - 1) / s.cz) * s.cz;
    return static_cast<double>(pad_x * pad_y * pad_z)
         / static_cast<double>(X * Y * Z);
}

// ─── Test 1: Cube data (512×512×512) ─────────────────────────
void test_cube_data() {
    ChunkShape s = find_optimal_chunk_shape(512, 512, 512);

    CHECK(in_candidates(s.cx), "cx not in CANDIDATES");
    CHECK(in_candidates(s.cy), "cy not in CANDIDATES");
    CHECK(in_candidates(s.cz), "cz not in CANDIDATES");

    int64_t vol = static_cast<int64_t>(s.cx) * s.cy * s.cz * 4;
    CHECK(vol >= static_cast<int64_t>(MIN_CHUNK_BYTES),
          "chunk volume below minimum");
    CHECK(vol <= static_cast<int64_t>(MAX_CHUNK_BYTES),
          "chunk volume above maximum");

    double ratio = storage_ratio(512, 512, 512, s);
    CHECK(ratio <= MAX_STORAGE_RATIO, "storage ratio exceeds limit");

    CHECK(s.cx >= s.cy && s.cy >= s.cz,
          "expected cx >= cy >= cz for balanced I/O (row-major layout)");
}

// ─── Test 2: Flat data (10000×10000×100) ─────────────────────
void test_flat_data() {
    ChunkShape s = find_optimal_chunk_shape(10000, 10000, 100);

    CHECK(in_candidates(s.cx), "cx not in CANDIDATES");
    CHECK(in_candidates(s.cy), "cy not in CANDIDATES");
    CHECK(in_candidates(s.cz), "cz not in CANDIDATES");

    int64_t vol = static_cast<int64_t>(s.cx) * s.cy * s.cz * 4;
    CHECK(vol >= static_cast<int64_t>(MIN_CHUNK_BYTES),
          "chunk volume below minimum");
    CHECK(vol <= static_cast<int64_t>(MAX_CHUNK_BYTES),
          "chunk volume above maximum");

    double ratio = storage_ratio(10000, 10000, 100, s);
    CHECK(ratio <= MAX_STORAGE_RATIO, "storage ratio exceeds limit");

    CHECK(s.cz < s.cx && s.cz < s.cy,
          "expected smaller cz for flat data (Z axis is short)");
}

// ─── Test 3: Elongated data (100×10000×100) ──────────────────
void test_elongated_data() {
    ChunkShape s = find_optimal_chunk_shape(100, 10000, 100);

    CHECK(in_candidates(s.cx), "cx not in CANDIDATES");
    CHECK(in_candidates(s.cy), "cy not in CANDIDATES");
    CHECK(in_candidates(s.cz), "cz not in CANDIDATES");

    int64_t vol = static_cast<int64_t>(s.cx) * s.cy * s.cz * 4;
    CHECK(vol >= static_cast<int64_t>(MIN_CHUNK_BYTES),
          "chunk volume below minimum");
    CHECK(vol <= static_cast<int64_t>(MAX_CHUNK_BYTES),
          "chunk volume above maximum");

    double ratio = storage_ratio(100, 10000, 100, s);
    CHECK(ratio <= MAX_STORAGE_RATIO, "storage ratio exceeds limit");

    CHECK(s.cy > s.cx && s.cy > s.cz,
          "expected larger cy for elongated Y-axis data");
}

// ─── Test 4: Tiny data (30×30×30) → fallback ─────────────────
void test_tiny_data() {
    char buf[4096] = {};
    std::string captured;

    // Redirect stderr fd so we can capture the fallback warning
#ifdef _WIN32
    int saved_fd = _dup(2);
#else
    int saved_fd = dup(2);
#endif

    FILE* tmpf = std::tmpfile();
    if (tmpf && saved_fd >= 0) {
#ifdef _WIN32
        int tmp_fd = _fileno(tmpf);
        _dup2(tmp_fd, 2);
#else
        int tmp_fd = fileno(tmpf);
        dup2(tmp_fd, 2);
#endif

        ChunkShape s = find_optimal_chunk_shape(30, 30, 30);

        std::fflush(stderr);

#ifdef _WIN32
        _dup2(saved_fd, 2);
        _close(saved_fd);
#else
        dup2(saved_fd, 2);
        close(saved_fd);
#endif

        std::rewind(tmpf);
        size_t n = std::fread(buf, 1, sizeof(buf) - 1, tmpf);
        buf[n] = '\0';
        captured = buf;
        std::fclose(tmpf);

        CHECK(s.cx == 64 && s.cy == 64 && s.cz == 64,
              "fallback shape should be {64, 64, 64}");

        CHECK(captured.find("WARNING") != std::string::npos,
              "expected stderr WARNING for fallback");
    } else {
        // Fallback: test without capture (still validate shape + ratio)
        ChunkShape s = find_optimal_chunk_shape(30, 30, 30);
        CHECK(s.cx == 64 && s.cy == 64 && s.cz == 64,
              "fallback shape should be {64, 64, 64}");
    }

    double ratio = storage_ratio(30, 30, 30, {64, 64, 64});
    CHECK(ratio > MAX_STORAGE_RATIO,
          "{64,64,64} exceeds ratio limit for tiny data, fallback is expected");
}

// ─── Test 5: Large values (no overflow / no crash) ───────────
void test_large_values() {
    ChunkShape s = find_optimal_chunk_shape(1000000, 1000000, 1000);

    CHECK(in_candidates(s.cx), "cx not in CANDIDATES");
    CHECK(in_candidates(s.cy), "cy not in CANDIDATES");
    CHECK(in_candidates(s.cz), "cz not in CANDIDATES");

    int64_t vol = static_cast<int64_t>(s.cx) * s.cy * s.cz * 4;
    CHECK(vol >= static_cast<int64_t>(MIN_CHUNK_BYTES),
          "chunk volume below minimum");
    CHECK(vol <= static_cast<int64_t>(MAX_CHUNK_BYTES),
          "chunk volume above maximum");

    double ratio = storage_ratio(1000000, 1000000, 1000, s);
    CHECK(ratio <= MAX_STORAGE_RATIO, "storage ratio exceeds limit");
}

// ─── Test runner ──────────────────────────────────────────────
int main() {
    std::printf("\n=== Phase 3 Unit Tests ===\n\n");

    auto run = [](void (*fn)(), const char* name) {
        std::printf("  RUN   %s\n", name);
        int before = g_failed;
        fn();
        if (g_failed == before) {
            std::printf("  PASS  %s\n", name);
            g_passed++;
        }
    };

    run(test_cube_data,       "test_cube_data");
    run(test_flat_data,       "test_flat_data");
    run(test_elongated_data,  "test_elongated_data");
    run(test_tiny_data,       "test_tiny_data");
    run(test_large_values,    "test_large_values");

    std::printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
