#pragma once

#include <cstdint>
#include <string>

#include "common.h"

// ─── C3DRHeader ────────────────────────────────────────────────
// Custom .c3dr file header (little-endian, packed layout expected at binary level)
struct C3DRHeader {
    uint32_t    magic = 0x52443343;            // "C3DR" in little-endian
    uint32_t    version = C3DR_VERSION;
    uint64_t    dim_x, dim_y, dim_z;           // original data dimensions
    ChunkShape  chunk_shape = {64, 64, 64};    // optimal chunk shape
    uint64_t    index_offset = 0;              // byte offset of IndexTable
    uint64_t    data_offset = 0;               // byte offset of chunk data
};

static_assert(sizeof(C3DRHeader) == 64, "C3DRHeader should be 64 bytes for alignment");

// ─── Optimizer interface ───────────────────────────────────────
// Find the optimal chunk shape that minimizes X/Y/Z slice-read imbalance
// while satisfying storage-ratio and volume constraints.
ChunkShape find_optimal_chunk_shape(
    int64_t X, int64_t Y, int64_t Z,
    int elem_size = 4);
