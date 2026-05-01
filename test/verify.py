#!/usr/bin/env python3
"""验证原始 float32 二进制文件。

模式:
   1) inspect: 打印文件统计信息 (维度、最小值、最大值、校验和)
   2) compare: 验证两个原始文件在容差范围内一致

数据布局: 行优先 (row-major)，X 轴变化最慢，Z 轴变化最快。
  全局索引 = x * (dim_y * dim_z) + y * dim_z + z
"""

import argparse
import sys
import numpy as np


def read_raw(path: str, dims: tuple) -> np.ndarray:
    dim_x, dim_y, dim_z = dims
    expected = dim_x * dim_y * dim_z * 4
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != expected:
        print(f"错误: {path}: 预期 {expected} 字节, 实际 {len(data)} 字节")
        sys.exit(1)
    arr = np.frombuffer(data, dtype=np.float32).reshape((dim_x, dim_y, dim_z))
    return arr


def compute_checksum(arr: np.ndarray) -> int:
    raw = arr.ravel().view(np.uint32)
    h = int(0)
    for v in raw:
        h = (h * 31 + int(v)) & 0xFFFFFFFFFFFFFFFF
    return h


def cmd_inspect(args):
    arr = read_raw(args.file, (args.dim_x, args.dim_y, args.dim_z))
    print(f"文件:         {args.file}")
    print(f"维度:         {arr.shape[0]} x {arr.shape[1]} x {arr.shape[2]}")
    print(f"元素总数:     {arr.size}")
    print(f"值域:         [{float(arr.min()):.6f}, {float(arr.max()):.6f}]")
    ck = compute_checksum(arr)
    print(f"校验和:       {ck}")
    if args.expected_checksum is not None:
        if ck == args.expected_checksum:
            print("校验和:       匹配")
        else:
            print(f"校验和:       不匹配 (预期 {args.expected_checksum})")
            sys.exit(1)


def cmd_compare(args):
    dims = (args.dim_x, args.dim_y, args.dim_z)
    a = read_raw(args.file_a, dims)
    b = read_raw(args.file_b, dims)

    abs_diff = np.abs(a - b)
    max_abs = float(np.max(abs_diff))

    with np.errstate(divide="ignore", invalid="ignore"):
        rel_err = abs_diff / np.maximum(np.abs(a), 1e-9)
    max_rel = float(np.max(rel_err))

    print(f"最大绝对误差:  {max_abs:.6e}")
    print(f"最大相对误差:  {max_rel:.6e}")
    print(f"容差:          rtol={args.rtol}")
    if max_rel < args.rtol:
        print("结果:          通过")
    else:
        idx = int(np.argmax(rel_err.ravel()))
        pos = np.unravel_index(idx, a.shape)
        print(f"最差点:        index={pos}  a={float(a.ravel()[idx]):.6f}  b={float(b.ravel()[idx]):.6f}")
        print("结果:          失败")
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="验证原始 float32 三维二进制文件")
    sub = parser.add_subparsers(dest="cmd")

    p_inspect = sub.add_parser("inspect", help="打印文件统计信息")
    p_inspect.add_argument("--file", "-f", required=True, help="原始二进制文件路径")
    p_inspect.add_argument("--dim-x", type=int, required=True)
    p_inspect.add_argument("--dim-y", type=int, required=True)
    p_inspect.add_argument("--dim-z", type=int, required=True)
    p_inspect.add_argument("--expected-checksum", type=int, help="预期校验和 (可选)")

    p_cmp = sub.add_parser("compare", help="比较两个原始文件")
    p_cmp.add_argument("--file-a", "-a", required=True)
    p_cmp.add_argument("--file-b", "-b", required=True)
    p_cmp.add_argument("--dim-x", type=int, required=True)
    p_cmp.add_argument("--dim-y", type=int, required=True)
    p_cmp.add_argument("--dim-z", type=int, required=True)
    p_cmp.add_argument("--rtol", type=float, default=1e-3, help="相对容差")

    args = parser.parse_args()
    if args.cmd == "inspect":
        cmd_inspect(args)
    elif args.cmd == "compare":
        cmd_compare(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
