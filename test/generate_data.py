#!/usr/bin/env python3
"""生成三维 float32 测试数据并以原始二进制格式写入。

数据布局: 行优先 (row-major)，X 轴变化最慢，Z 轴变化最快。
  全局索引 = x * (dim_y * dim_z) + y * dim_z + z
"""

import argparse
import struct
import numpy as np


def generate_gradient(dim_x, dim_y, dim_z):
    """生成平滑的三维渐变数据 (确定性，可复现)。"""
    xs = np.linspace(0, 1, dim_x, dtype=np.float32)
    ys = np.linspace(0, 1, dim_y, dtype=np.float32)
    zs = np.linspace(0, 1, dim_z, dtype=np.float32)
    data = (xs[:, np.newaxis, np.newaxis] +
            ys[np.newaxis, :, np.newaxis] +
            zs[np.newaxis, np.newaxis, :])
    return data.astype(np.float32)


def generate_sin3d(dim_x, dim_y, dim_z):
    """生成基于正弦函数的三维数据 (平滑、非平凡)。"""
    xs = np.linspace(0, 4 * np.pi, dim_x, dtype=np.float32)
    ys = np.linspace(0, 4 * np.pi, dim_y, dtype=np.float32)
    zs = np.linspace(0, 4 * np.pi, dim_z, dtype=np.float32)
    data = (np.sin(xs[:, np.newaxis, np.newaxis]) *
            np.cos(ys[np.newaxis, :, np.newaxis]) *
            np.sin(zs[np.newaxis, np.newaxis, :] + 1.0))
    return data.astype(np.float32)


def generate_random(dim_x, dim_y, dim_z, seed=42):
    """生成随机 float32 数据 (通过 seed 可复现)。"""
    rng = np.random.default_rng(seed)
    return rng.random((dim_x, dim_y, dim_z), dtype=np.float32)


PATTERNS = {
    "gradient": generate_gradient,
    "sin3d":    generate_sin3d,
    "random":   generate_random,
}


def main():
    parser = argparse.ArgumentParser(
        description="生成三维 float32 测试数据")
    parser.add_argument("--dim-x", type=int, required=True,
                        help="X 维度 (变化最慢)")
    parser.add_argument("--dim-y", type=int, required=True,
                        help="Y 维度")
    parser.add_argument("--dim-z", type=int, required=True,
                        help="Z 维度 (变化最快)")
    parser.add_argument("--output", "-o", required=True,
                        help="输出原始二进制文件路径")
    parser.add_argument("--pattern", choices=list(PATTERNS), default="sin3d",
                        help="数据模式 (默认: sin3d)")
    parser.add_argument("--seed", type=int, default=42,
                        help="随机种子 (用于 random 模式)")
    args = parser.parse_args()

    generate_fn = PATTERNS[args.pattern]

    if args.pattern == "random":
        data = generate_fn(args.dim_x, args.dim_y, args.dim_z, args.seed)
    else:
        data = generate_fn(args.dim_x, args.dim_y, args.dim_z)

    # 验证布局: X 最慢, Z 最快 (C-order = 行优先)
    # NumPy 默认为 C-order: 最后一个索引变化最快
    # data.shape = (dim_x, dim_y, dim_z), 即 data[x,y,z] 中 z 最快
    # 与 C++ 布局一致: 全局索引 = x*(Y*Z) + y*Z + z
    assert data.shape == (args.dim_x, args.dim_y, args.dim_z)

    with open(args.output, "wb") as f:
        f.write(data.tobytes())

    size_mb = data.nbytes / (1024 * 1024)
    print(f"已生成 {args.dim_x}x{args.dim_y}x{args.dim_z} "
          f"float32 ({size_mb:.2f} MB) -> {args.output}")
    print(f"数据模式: {args.pattern}")
    print(f"值域: [{data.min():.6f}, {data.max():.6f}]")


if __name__ == "__main__":
    main()
