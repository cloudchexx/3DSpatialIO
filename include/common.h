#pragma once

#include <cstdint>

constexpr uint32_t C3DR_VERSION = 1;

// ─── ChunkShape ────────────────────────────────────────────────
struct ChunkShape {
    int cx, cy, cz;
};

// ─── Phase 1 configurable constants ────────────────────────────
constexpr int    CANDIDATES[]        = {16, 32, 64, 128, 256, 512};
constexpr size_t CANDIDATES_COUNT    = 6;
constexpr size_t MIN_CHUNK_BYTES     = 512 * 1024;       // 512 KB
constexpr size_t MAX_CHUNK_BYTES     = 4 * 1024 * 1024;  // 4 MB
constexpr double MAX_STORAGE_RATIO   = 1.45;
constexpr double W_IO                = 1.0;
constexpr double W_MEM               = 0.005;
