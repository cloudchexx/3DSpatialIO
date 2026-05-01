#pragma once

#include "chunk_io.h"

#include <atomic>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

class C3DRCacheReader : public C3DRReader {
public:
    explicit C3DRCacheReader(size_t cache_size_bytes = 2ULL * 1024 * 1024 * 1024);
    ~C3DRCacheReader() override;

    bool open(const std::string& path);
    void close();

    std::vector<float> read_x_slice(uint32_t x);
    std::vector<float> read_y_slice(uint32_t y);
    std::vector<float> read_z_slice(uint32_t z);

    void write_x_slice(uint32_t x, const std::vector<float>& data);
    void write_y_slice(uint32_t y, const std::vector<float>& data);
    void write_z_slice(uint32_t z, const std::vector<float>& data);

    void flush();
    void flush_chunk(uint64_t idx);

    void clear_cache();

    size_t cache_hits()          const { return m_hits.load(std::memory_order_relaxed); }
    size_t cache_misses()        const { return m_misses.load(std::memory_order_relaxed); }
    size_t cache_memory_usage()  const { return m_current_cache_bytes.load(std::memory_order_relaxed); }
    size_t cache_capacity()      const { return m_max_cache_bytes; }
    size_t dirty_count()         const;

    std::shared_ptr<const std::vector<float>>
        get_chunk(uint64_t chunk_idx) { return read_chunk_by_index(chunk_idx); }

protected:
    std::shared_ptr<const std::vector<float>>
        read_chunk_by_index(uint64_t chunk_idx) override;

private:
    struct CacheEntry {
        std::shared_ptr<const std::vector<float>> data;
        std::list<uint64_t>::iterator             lru_iter;
        bool                                      is_dirty = false;
    };

    std::unordered_map<uint64_t, CacheEntry> m_cache;
    std::list<uint64_t>                      m_lru_list;
    std::atomic<size_t>                      m_current_cache_bytes{0};
    size_t                                   m_max_cache_bytes;

    mutable std::shared_mutex m_cache_mutex;
    mutable std::mutex        m_io_mutex;

    std::atomic<size_t> m_hits{0};
    std::atomic<size_t> m_misses{0};

    struct SliceRequest { int axis; uint32_t position; };
    std::deque<SliceRequest> m_recent_requests;
    static constexpr size_t  PREFETCH_WINDOW = 3;

    void evict_if_needed(size_t incoming_bytes);
    void maybe_prefetch(int axis, uint32_t position);
    std::vector<uint64_t> compute_needed_chunks(int axis, uint32_t pos);
    void do_prefetch(const std::vector<uint64_t>& indices);

    void write_chunk_in_cache(uint64_t chunk_idx,
                              const std::vector<float>& patch,
                              size_t offset_in_chunk);

    struct PatchItem { size_t offset; const float* data; size_t count; };
    void write_chunk_in_cache_batch(uint64_t chunk_idx,
                                    const std::vector<PatchItem>& patches);
};
