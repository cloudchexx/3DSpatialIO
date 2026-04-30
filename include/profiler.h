#pragma once

// ============================================================
// Profiling 工具（阶段四）
// ============================================================
// calibrate_w_mem()：  测量本机"连续大块读"与"随机跳跃读"的耗时比，
//                      用于校正 cost 函数中的 W_MEM 参数。
// benchmark_slices()： 写入 .c3dr → 沿 X/Y/Z 三轴分别读取切面，
//                      记录耗时并计算 max/min 均衡比。
// ============================================================

#include "chunk_optimizer.h"

#include <cstdint>
#include <vector>

// ─── 切片性能回归结果 ──────────────────────────────────────────
struct SliceBenchResult {
    double    tx_ms;           // X 轴切面读取耗时 (ms)
    double    ty_ms;           // Y 轴切面读取耗时 (ms)
    double    tz_ms;           // Z 轴切面读取耗时 (ms)
    double    balance_ratio;   // max(tx,ty,tz) / min(tx,ty,tz)，越接近 1 越均衡
    ChunkShape shape;          // 使用的切块形状
    double    storage_ratio;   // 落盘存储膨胀率
    bool      passed;          // 存储红线 + 均衡度验收是否通过
};

// ─── 内存 Profiling ────────────────────────────────────────────
// 分配大块内存，分别做"连续顺序读"和"随机跨大步读"，
// 计算随机跳读 / 连续读的耗时比，作为 W_MEM 的标定参考。
// 返回值：测得的 random/sequential 耗时比（建议作为下次 W_MEM 初始值）
double calibrate_w_mem(size_t data_size_mb = 1024);

// ─── 切面读取性能回归 ──────────────────────────────────────────
// 使用给定的三维数据 + 切块形状，写入临时 .c3dr 文件，
// 然后分别沿 X / Y / Z 轴各读一个中间层切面，测量耗时。
//   data  : 原始 float32 数据（行优先，X 最外层）
//   dim_x/y/z : 原始数据尺寸
//   shape : 切块形状 (通常由 find_optimal_chunk_shape 给出)
//   tmp_path : 临时 .c3dr 文件路径
SliceBenchResult benchmark_slices(
    const std::vector<float>& data,
    uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
    const ChunkShape& shape,
    const std::string& tmp_path);
