#include "cache_reader.h"

#include <algorithm>
#include <cstring>

// ════════════════════════════════════════════════════════════════
// 生命周期
// ════════════════════════════════════════════════════════════════

C3DRCacheReader::C3DRCacheReader(size_t cache_size_bytes)
    : m_max_cache_bytes(cache_size_bytes < 2ULL * 1024 * 1024 * 1024
                        ? 2ULL * 1024 * 1024 * 1024 : cache_size_bytes)
{}

C3DRCacheReader::~C3DRCacheReader() {
    flush();
    clear_cache();
}

bool C3DRCacheReader::open(const std::string& path) {
    clear_cache();
    if (!C3DRReader::open(path)) return false;
    fclose(m_file);
    m_file = fopen(path.c_str(), "r+b");
    return m_file != nullptr;
}

void C3DRCacheReader::close() {
    flush();
    clear_cache();
    C3DRReader::close();
}

void C3DRCacheReader::clear_cache() {
    std::unique_lock lock(m_cache_mutex);
    m_cache.clear();
    m_lru_list.clear();
    m_current_cache_bytes.store(0, std::memory_order_relaxed);
    m_recent_requests.clear();
}

// ════════════════════════════════════════════════════════════════
// read_chunk_by_index — 缓存查找 + 磁盘加载
// ════════════════════════════════════════════════════════════════

std::shared_ptr<const std::vector<float>>
C3DRCacheReader::read_chunk_by_index(uint64_t chunk_idx) {
    {
        std::shared_lock lock(m_cache_mutex);
        auto it = m_cache.find(chunk_idx);
        if (it != m_cache.end()) {
            m_hits.fetch_add(1, std::memory_order_relaxed);
            m_lru_list.splice(m_lru_list.begin(), m_lru_list, it->second.lru_iter);
            return it->second.data;
        }
    }

    m_misses.fetch_add(1, std::memory_order_relaxed);

    auto disk_data = [&]() {
        std::lock_guard io_lock(m_io_mutex);
        return C3DRReader::read_chunk_by_index(chunk_idx);
    }();

    std::unique_lock lock(m_cache_mutex);

    auto it = m_cache.find(chunk_idx);
    if (it != m_cache.end()) {
        return it->second.data;
    }

    size_t chunk_mem = disk_data->size() * sizeof(float);
    evict_if_needed(chunk_mem);

    m_lru_list.push_front(chunk_idx);
    m_cache[chunk_idx] = { disk_data, m_lru_list.begin(), false };
    m_current_cache_bytes.fetch_add(chunk_mem, std::memory_order_relaxed);

    return disk_data;
}

// ════════════════════════════════════════════════════════════════
// evict_if_needed — LRU 淘汰（脏块先 flush）
// ════════════════════════════════════════════════════════════════

void C3DRCacheReader::evict_if_needed(size_t incoming_bytes) {
    while (m_current_cache_bytes.load(std::memory_order_relaxed) + incoming_bytes > m_max_cache_bytes
           && !m_lru_list.empty()) {
        uint64_t victim_idx = m_lru_list.back();
        auto it = m_cache.find(victim_idx);
        if (it == m_cache.end()) {
            m_lru_list.pop_back();
            continue;
        }

        auto& victim = it->second;

        if (victim.is_dirty) {
            std::lock_guard io_lock(m_io_mutex);
            uint64_t offset = m_index[victim_idx].offset;
            size_t total = victim.data->size();
            if (FSEEK64(m_file, static_cast<int64_t>(offset), SEEK_SET) == 0) {
                fwrite(victim.data->data(), sizeof(float), total, m_file);
            }
        }

        size_t victim_bytes = victim.data->size() * sizeof(float);
        m_current_cache_bytes.fetch_sub(victim_bytes, std::memory_order_relaxed);
        m_lru_list.pop_back();
        m_cache.erase(it);
    }
}

// ════════════════════════════════════════════════════════════════
// write_chunk_in_cache — 单 patch 写入
// ════════════════════════════════════════════════════════════════

void C3DRCacheReader::write_chunk_in_cache(
    uint64_t chunk_idx,
    const std::vector<float>& patch,
    size_t offset_in_chunk)
{
    std::unique_lock lock(m_cache_mutex);

    auto it = m_cache.find(chunk_idx);

    if (it != m_cache.end()) {
        auto& entry = it->second;

        if (entry.data.use_count() == 1) {
            auto& mutable_data = const_cast<std::vector<float>&>(*entry.data);
            std::memcpy(mutable_data.data() + offset_in_chunk,
                        patch.data(), patch.size() * sizeof(float));
        } else {
            auto copy = std::make_shared<std::vector<float>>(*entry.data);
            std::memcpy(copy->data() + offset_in_chunk,
                        patch.data(), patch.size() * sizeof(float));
            entry.data = std::shared_ptr<const std::vector<float>>(std::move(copy));
        }

        entry.is_dirty = true;
        m_lru_list.splice(m_lru_list.begin(), m_lru_list, entry.lru_iter);
    } else {
        lock.unlock();
        auto disk_data = [&]() {
            std::lock_guard io_lock(m_io_mutex);
            return C3DRReader::read_chunk_by_index(chunk_idx);
        }();
        lock.lock();

        auto it2 = m_cache.find(chunk_idx);
        if (it2 != m_cache.end()) {
            auto& entry = it2->second;
            if (entry.data.use_count() == 1) {
                auto& mutable_data = const_cast<std::vector<float>&>(*entry.data);
                std::memcpy(mutable_data.data() + offset_in_chunk,
                            patch.data(), patch.size() * sizeof(float));
            } else {
                auto copy = std::make_shared<std::vector<float>>(*entry.data);
                std::memcpy(copy->data() + offset_in_chunk,
                            patch.data(), patch.size() * sizeof(float));
                entry.data = std::shared_ptr<const std::vector<float>>(std::move(copy));
            }
            entry.is_dirty = true;
            m_lru_list.splice(m_lru_list.begin(), m_lru_list, entry.lru_iter);
            return;
        }

        auto mutable_copy = std::make_shared<std::vector<float>>(*disk_data);
        std::memcpy(mutable_copy->data() + offset_in_chunk,
                    patch.data(), patch.size() * sizeof(float));

        size_t chunk_mem = mutable_copy->size() * sizeof(float);
        evict_if_needed(chunk_mem);

        m_lru_list.push_front(chunk_idx);
        m_cache[chunk_idx] = {
            std::shared_ptr<const std::vector<float>>(std::move(mutable_copy)),
            m_lru_list.begin(),
            true
        };
        m_current_cache_bytes.fetch_add(chunk_mem, std::memory_order_relaxed);
    }
}

// ════════════════════════════════════════════════════════════════
// write_chunk_in_cache_batch — 多 patch 批量写入（减少锁开销）
// ════════════════════════════════════════════════════════════════

void C3DRCacheReader::write_chunk_in_cache_batch(
    uint64_t chunk_idx,
    const std::vector<PatchItem>& patches)
{
    auto apply_patches = [&](std::vector<float>& target) {
        for (const auto& p : patches) {
            std::memcpy(target.data() + p.offset, p.data, p.count * sizeof(float));
        }
    };

    std::unique_lock lock(m_cache_mutex);

    auto it = m_cache.find(chunk_idx);

    if (it != m_cache.end()) {
        auto& entry = it->second;

        if (entry.data.use_count() == 1) {
            auto& mutable_data = const_cast<std::vector<float>&>(*entry.data);
            apply_patches(mutable_data);
        } else {
            auto copy = std::make_shared<std::vector<float>>(*entry.data);
            apply_patches(*copy);
            entry.data = std::shared_ptr<const std::vector<float>>(std::move(copy));
        }

        entry.is_dirty = true;
        m_lru_list.splice(m_lru_list.begin(), m_lru_list, entry.lru_iter);
    } else {
        lock.unlock();
        auto disk_data = [&]() {
            std::lock_guard io_lock(m_io_mutex);
            return C3DRReader::read_chunk_by_index(chunk_idx);
        }();
        lock.lock();

        auto it2 = m_cache.find(chunk_idx);
        if (it2 != m_cache.end()) {
            auto& entry = it2->second;
            if (entry.data.use_count() == 1) {
                auto& mutable_data = const_cast<std::vector<float>&>(*entry.data);
                apply_patches(mutable_data);
            } else {
                auto copy = std::make_shared<std::vector<float>>(*entry.data);
                apply_patches(*copy);
                entry.data = std::shared_ptr<const std::vector<float>>(std::move(copy));
            }
            entry.is_dirty = true;
            m_lru_list.splice(m_lru_list.begin(), m_lru_list, entry.lru_iter);
            return;
        }

        auto mutable_copy = std::make_shared<std::vector<float>>(*disk_data);
        apply_patches(*mutable_copy);

        size_t chunk_mem = mutable_copy->size() * sizeof(float);
        evict_if_needed(chunk_mem);

        m_lru_list.push_front(chunk_idx);
        m_cache[chunk_idx] = {
            std::shared_ptr<const std::vector<float>>(std::move(mutable_copy)),
            m_lru_list.begin(),
            true
        };
        m_current_cache_bytes.fetch_add(chunk_mem, std::memory_order_relaxed);
    }
}

// ════════════════════════════════════════════════════════════════
// write_x_slice — X 轴写切面
// ════════════════════════════════════════════════════════════════

void C3DRCacheReader::write_x_slice(uint32_t x, const std::vector<float>& data) {
    if (x >= m_header.dim_x) return;
    if (data.size() != static_cast<size_t>(m_header.dim_y * m_header.dim_z)) return;

    uint32_t ix      = x / static_cast<uint32_t>(m_header.chunk_shape.cx);
    uint32_t local_x = x % static_cast<uint32_t>(m_header.chunk_shape.cx);
    size_t   cy = static_cast<size_t>(m_header.chunk_shape.cy);
    size_t   cz = static_cast<size_t>(m_header.chunk_shape.cz);
    uint64_t dim_y = m_header.dim_y;
    uint64_t dim_z = m_header.dim_z;

    for (uint32_t iy = 0; iy < m_nc_y; ++iy) {
        uint32_t y_start = iy * static_cast<uint32_t>(cy);
        for (uint32_t iz = 0; iz < m_nc_z; ++iz) {
            uint32_t z_start = iz * static_cast<uint32_t>(cz);

            uint64_t chunk_idx = static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z
                               + iz;

            uint32_t y_end = (std::min)(y_start + static_cast<uint32_t>(cy),
                                        static_cast<uint32_t>(dim_y));
            uint32_t z_end = (std::min)(z_start + static_cast<uint32_t>(cz),
                                        static_cast<uint32_t>(dim_z));

            size_t offset_in_chunk = static_cast<size_t>(local_x) * cy * cz;

            size_t y_count = y_end - y_start;
            std::vector<float> patch(y_count * cz, 0.0f);
            for (uint32_t yy = y_start; yy < y_end; ++yy) {
                uint32_t local_y = yy - y_start;
                size_t src_row = static_cast<size_t>(yy) * dim_z + z_start;
                size_t dst_row = static_cast<size_t>(local_y) * cz;
                size_t count = z_end - z_start;
                std::memcpy(patch.data() + dst_row,
                            data.data() + src_row,
                            count * sizeof(float));
            }

            write_chunk_in_cache(chunk_idx, patch, offset_in_chunk);
        }
    }
}

// ════════════════════════════════════════════════════════════════
// write_y_slice — Y 轴写切面（批量 per chunk）
// ════════════════════════════════════════════════════════════════

void C3DRCacheReader::write_y_slice(uint32_t y, const std::vector<float>& data) {
    if (y >= m_header.dim_y) return;
    if (data.size() != static_cast<size_t>(m_header.dim_x * m_header.dim_z)) return;

    uint32_t iy      = y / static_cast<uint32_t>(m_header.chunk_shape.cy);
    uint32_t local_y = y % static_cast<uint32_t>(m_header.chunk_shape.cy);
    size_t   cx = static_cast<size_t>(m_header.chunk_shape.cx);
    size_t   cy = static_cast<size_t>(m_header.chunk_shape.cy);
    size_t   cz = static_cast<size_t>(m_header.chunk_shape.cz);
    uint64_t dim_x = m_header.dim_x;
    uint64_t dim_z = m_header.dim_z;

    for (uint32_t ix = 0; ix < m_nc_x; ++ix) {
        uint32_t x_start = ix * static_cast<uint32_t>(cx);
        for (uint32_t iz = 0; iz < m_nc_z; ++iz) {
            uint32_t z_start = iz * static_cast<uint32_t>(cz);

            uint64_t chunk_idx = static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z
                               + iz;

            uint32_t x_end = (std::min)(x_start + static_cast<uint32_t>(cx),
                                        static_cast<uint32_t>(dim_x));
            uint32_t z_end = (std::min)(z_start + static_cast<uint32_t>(cz),
                                        static_cast<uint32_t>(dim_z));
            size_t z_count = z_end - z_start;

            std::vector<PatchItem> patches;
            for (uint32_t xx = x_start; xx < x_end; ++xx) {
                uint32_t local_x = xx - x_start;
                size_t src_row = static_cast<size_t>(xx) * dim_z + z_start;
                size_t chunk_offset = static_cast<size_t>(local_x) * cy * cz
                                    + static_cast<size_t>(local_y) * cz;
                patches.push_back({chunk_offset, data.data() + src_row, z_count});
            }

            write_chunk_in_cache_batch(chunk_idx, patches);
        }
    }
}

// ════════════════════════════════════════════════════════════════
// write_z_slice — Z 轴写切面（批量 per chunk）
// ════════════════════════════════════════════════════════════════

void C3DRCacheReader::write_z_slice(uint32_t z, const std::vector<float>& data) {
    if (z >= m_header.dim_z) return;
    if (data.size() != static_cast<size_t>(m_header.dim_x * m_header.dim_y)) return;

    uint32_t iz      = z / static_cast<uint32_t>(m_header.chunk_shape.cz);
    uint32_t local_z = z % static_cast<uint32_t>(m_header.chunk_shape.cz);
    size_t   cx = static_cast<size_t>(m_header.chunk_shape.cx);
    size_t   cy = static_cast<size_t>(m_header.chunk_shape.cy);
    size_t   cz = static_cast<size_t>(m_header.chunk_shape.cz);
    uint64_t dim_x = m_header.dim_x;
    uint64_t dim_y = m_header.dim_y;

    for (uint32_t ix = 0; ix < m_nc_x; ++ix) {
        uint32_t x_start = ix * static_cast<uint32_t>(cx);
        for (uint32_t iy = 0; iy < m_nc_y; ++iy) {
            uint32_t y_start = iy * static_cast<uint32_t>(cy);

            uint64_t chunk_idx = static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z
                               + iz;

            uint32_t x_end = (std::min)(x_start + static_cast<uint32_t>(cx),
                                        static_cast<uint32_t>(dim_x));
            uint32_t y_end = (std::min)(y_start + static_cast<uint32_t>(cy),
                                        static_cast<uint32_t>(dim_y));

            size_t x_count = x_end - x_start;
            size_t y_count = y_end - y_start;
            std::vector<float> scatter(x_count * y_count);
            std::vector<PatchItem> patches;
            patches.reserve(x_count * y_count);

            size_t si = 0;
            for (uint32_t xx = x_start; xx < x_end; ++xx) {
                uint32_t local_x = xx - x_start;
                for (uint32_t yy = y_start; yy < y_end; ++yy) {
                    uint32_t local_y = yy - y_start;
                    scatter[si] = data[static_cast<size_t>(xx) * dim_y + yy];
                    size_t chunk_offset = static_cast<size_t>(local_x) * cy * cz
                                        + static_cast<size_t>(local_y) * cz
                                        + local_z;
                    patches.push_back({chunk_offset, &scatter[si], 1});
                    ++si;
                }
            }

            write_chunk_in_cache_batch(chunk_idx, patches);
        }
    }
}

// ════════════════════════════════════════════════════════════════
// flush — 脏块落盘
// ════════════════════════════════════════════════════════════════

void C3DRCacheReader::flush() {
    std::unique_lock cache_lock(m_cache_mutex);
    std::lock_guard io_lock(m_io_mutex);

    for (auto& [idx, entry] : m_cache) {
        if (!entry.is_dirty) continue;

        uint64_t offset = m_index[idx].offset;
        size_t total = entry.data->size();

        if (FSEEK64(m_file, static_cast<int64_t>(offset), SEEK_SET) != 0) continue;
        fwrite(entry.data->data(), sizeof(float), total, m_file);

        entry.is_dirty = false;
    }
}

void C3DRCacheReader::flush_chunk(uint64_t chunk_idx) {
    std::unique_lock cache_lock(m_cache_mutex);
    auto it = m_cache.find(chunk_idx);
    if (it == m_cache.end() || !it->second.is_dirty) return;

    std::lock_guard io_lock(m_io_mutex);
    uint64_t offset = m_index[chunk_idx].offset;
    size_t total = it->second.data->size();

    if (FSEEK64(m_file, static_cast<int64_t>(offset), SEEK_SET) == 0) {
        fwrite(it->second.data->data(), sizeof(float), total, m_file);
    }
    it->second.is_dirty = false;
}

// ════════════════════════════════════════════════════════════════
// dirty_count — 统计脏块数
// ════════════════════════════════════════════════════════════════

size_t C3DRCacheReader::dirty_count() const {
    std::shared_lock lock(m_cache_mutex);
    size_t count = 0;
    for (const auto& [idx, entry] : m_cache) {
        if (entry.is_dirty) ++count;
    }
    return count;
}

// ════════════════════════════════════════════════════════════════
// 读切面（隐藏基类 + maybe_prefetch）
// ════════════════════════════════════════════════════════════════

std::vector<float> C3DRCacheReader::read_x_slice(uint32_t x) {
    auto result = C3DRReader::read_x_slice(x);
    maybe_prefetch(0, x);
    return result;
}

std::vector<float> C3DRCacheReader::read_y_slice(uint32_t y) {
    auto result = C3DRReader::read_y_slice(y);
    maybe_prefetch(1, y);
    return result;
}

std::vector<float> C3DRCacheReader::read_z_slice(uint32_t z) {
    auto result = C3DRReader::read_z_slice(z);
    maybe_prefetch(2, z);
    return result;
}

// ════════════════════════════════════════════════════════════════
// maybe_prefetch — 连续模式检测 + 预取
// ════════════════════════════════════════════════════════════════

void C3DRCacheReader::maybe_prefetch(int axis, uint32_t position) {
    std::unique_lock lock(m_cache_mutex);

    m_recent_requests.push_back({axis, position});
    while (m_recent_requests.size() > PREFETCH_WINDOW) {
        m_recent_requests.pop_front();
    }

    if (m_recent_requests.size() < PREFETCH_WINDOW) return;

    int delta = static_cast<int>(m_recent_requests.back().position)
              - static_cast<int>(m_recent_requests[m_recent_requests.size() - 2].position);
    if (delta == 0 || delta > 1 || delta < -1) return;

    for (size_t i = 1; i < PREFETCH_WINDOW; ++i) {
        if (m_recent_requests[i].axis != axis) return;
        int d = static_cast<int>(m_recent_requests[i].position)
              - static_cast<int>(m_recent_requests[i - 1].position);
        if (d != delta) return;
    }

    lock.unlock();

    uint32_t next_pos = static_cast<uint32_t>(
        static_cast<int>(position) + delta);

    auto cx = m_header.chunk_shape.cx;
    auto cy = m_header.chunk_shape.cy;
    auto cz = m_header.chunk_shape.cz;

    bool at_boundary = false;
    if (axis == 0 && next_pos % cx == 0) at_boundary = true;
    if (axis == 1 && next_pos % cy == 0) at_boundary = true;
    if (axis == 2 && next_pos % cz == 0) at_boundary = true;

    if (!at_boundary) return;

    auto needed = compute_needed_chunks(axis, next_pos);
    do_prefetch(needed);
}

// ════════════════════════════════════════════════════════════════
// compute_needed_chunks
// ════════════════════════════════════════════════════════════════

std::vector<uint64_t> C3DRCacheReader::compute_needed_chunks(int axis, uint32_t pos) {
    std::vector<uint64_t> needed;
    auto cx = m_header.chunk_shape.cx;
    auto cy = m_header.chunk_shape.cy;
    auto cz = m_header.chunk_shape.cz;

    if (axis == 0) {
        uint32_t ix = pos / cx;
        for (uint32_t iy = 0; iy < m_nc_y; ++iy)
            for (uint32_t iz = 0; iz < m_nc_z; ++iz)
                needed.push_back(static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z + iz);
    } else if (axis == 1) {
        uint32_t iy = pos / cy;
        for (uint32_t ix = 0; ix < m_nc_x; ++ix)
            for (uint32_t iz = 0; iz < m_nc_z; ++iz)
                needed.push_back(static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z + iz);
    } else {
        uint32_t iz = pos / cz;
        for (uint32_t ix = 0; ix < m_nc_x; ++ix)
            for (uint32_t iy = 0; iy < m_nc_y; ++iy)
                needed.push_back(static_cast<uint64_t>(ix) * m_nc_y * m_nc_z
                               + static_cast<uint64_t>(iy) * m_nc_z + iz);
    }
    return needed;
}

// ════════════════════════════════════════════════════════════════
// do_prefetch
// ════════════════════════════════════════════════════════════════

void C3DRCacheReader::do_prefetch(const std::vector<uint64_t>& indices) {
    std::vector<uint64_t> to_load;
    {
        std::shared_lock lock(m_cache_mutex);
        for (uint64_t idx : indices) {
            if (m_cache.find(idx) == m_cache.end()) {
                to_load.push_back(idx);
            }
        }
    }
    for (uint64_t idx : to_load) {
        read_chunk_by_index(idx);
    }
}
