#include "chunk_optimizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

ChunkShape find_optimal_chunk_shape(
    int64_t X, int64_t Y, int64_t Z,
    int elem_size)
{
    ChunkShape best_shape = {64, 64, 64};
    double min_cost = std::numeric_limits<double>::max();

    for (size_t i = 0; i < CANDIDATES_COUNT; ++i) {
        int cx = CANDIDATES[i];
        for (size_t j = 0; j < CANDIDATES_COUNT; ++j) {
            int cy = CANDIDATES[j];
            for (size_t k = 0; k < CANDIDATES_COUNT; ++k) {
                int cz = CANDIDATES[k];

                // ── Volume constraint ──────────────────────────
                int64_t vol = static_cast<int64_t>(cx) * cy * cz * elem_size;
                if (vol < static_cast<int64_t>(MIN_CHUNK_BYTES) ||
                    vol > static_cast<int64_t>(MAX_CHUNK_BYTES)) {
                    continue;
                }

                // ── Storage red line ───────────────────────────
                int64_t pad_x = ((X + cx - 1) / cx) * cx;
                int64_t pad_y = ((Y + cy - 1) / cy) * cy;
                int64_t pad_z = ((Z + cz - 1) / cz) * cz;
                double ratio = static_cast<double>(pad_x * pad_y * pad_z)
                             / static_cast<double>(X * Y * Z);
                if (ratio > MAX_STORAGE_RATIO) {
                    continue;
                }

                // ── Macro I/O block counts ─────────────────────
                int64_t Nx = ((Y + cy - 1) / cy) * ((Z + cz - 1) / cz);
                int64_t Ny = ((X + cx - 1) / cx) * ((Z + cz - 1) / cz);
                int64_t Nz = ((X + cx - 1) / cx) * ((Y + cy - 1) / cy);

                // ── Memory-contiguity-aware time estimates ─────
                double Tx = static_cast<double>(Nx) * (W_IO + cy * W_MEM);
                double Ty = static_cast<double>(Ny) * (W_IO + cx * W_MEM);
                double Tz = static_cast<double>(Nz) * (W_IO + (cx * cy) * W_MEM);

                double max_T = std::max({Tx, Ty, Tz});
                double min_T = std::min({Tx, Ty, Tz});
                double cost = max_T / min_T;

                if (cost < min_cost) {
                    min_cost = cost;
                    best_shape = {cx, cy, cz};
                }
            }
        }
    }

    if (min_cost == std::numeric_limits<double>::max()) {
        std::fprintf(stderr,
            "[WARNING] find_optimal_chunk_shape: no valid candidate found "
            "for shape (%lld, %lld, %lld). Falling back to {64, 64, 64}.\n",
            static_cast<long long>(X),
            static_cast<long long>(Y),
            static_cast<long long>(Z));
    }

    return best_shape;
}
