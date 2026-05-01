#pragma once

#include "chunk_optimizer.h"
#include "file_io.h"

#include <cstdint>
#include <vector>

struct SliceBenchResult {
    double    tx_ms;
    double    ty_ms;
    double    tz_ms;
    double    balance_ratio;
    ChunkShape shape;
    double    storage_ratio;
    bool      passed;
};

struct CacheSliceBenchResult : public SliceBenchResult {
    size_t    hits            = 0;
    size_t    misses          = 0;
    size_t    dirty_chunks    = 0;
    size_t    cache_mem_usage = 0;
    size_t    cache_capacity  = 0;
    double    write_ms        = 0.0;
    double    flush_ms        = 0.0;
    bool      write_ok        = false;
    bool      cow_ok          = false;
};

double calibrate_w_mem(size_t data_size_mb = 1024);

SliceBenchResult benchmark_slices(
    RawFileReader& reader,
    const ChunkShape& shape,
    const std::string& tmp_path);

CacheSliceBenchResult benchmark_slices_cache(
    RawFileReader& reader,
    const ChunkShape& shape,
    const std::string& tmp_path,
    size_t cache_size_bytes);
