#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""评分基准测试脚本

用法:
    python test/contest_bench.py \
        --input test.dat \
        --dim-x 800 --dim-y 2404 --dim-z 2500 \
        --skip-bytes 48 \
        --cache-size 2048

流程:
    1. 调用 cc_3Dreader 完成切块落盘 (如尚未完成)
    2. 调用 bench_e2e 运行完整评分测试
    3. 输出测试结果
"""

import argparse
import io
import os
import subprocess
import sys
from datetime import datetime

log_dir = "logs"
os.makedirs(log_dir, exist_ok=True)

if sys.stdout.encoding != 'utf-8':
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8')


def write_both(log_fp, text=""):
    print(text)
    log_fp.write(text + "\n")
    log_fp.flush()


def run_cmd(cmd, log_fp):
    write_both(log_fp, f"执行: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.stdout:
        write_both(log_fp, result.stdout.rstrip())
    if result.stderr:
        write_both(log_fp, result.stderr.rstrip())
    return result


def main():
    parser = argparse.ArgumentParser(
        description="三维数据评分基准测试",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
    python test/contest_bench.py --input test.dat --dim-x 800 --dim-y 2404 --dim-z 2500 --skip-bytes 48
        """)
    parser.add_argument("--input", "-i", required=True, help="输入数据文件路径")
    parser.add_argument("--dim-x", type=int, required=True, help="X 维度")
    parser.add_argument("--dim-y", type=int, required=True, help="Y 维度")
    parser.add_argument("--dim-z", type=int, required=True, help="Z 维度")
    parser.add_argument("--skip-bytes", type=int, default=0, help="跳过文件头字节数 (默认: 0)")
    parser.add_argument("--cache-size", type=int, default=8192, help="缓存大小 MB (默认: 2048)")
    parser.add_argument("--output", "-o", default="contest_output.bin", help="输出文件路径 (默认: contest_output.bin)")
    parser.add_argument("--exe-dir", default="build/bin/Release", help="可执行文件目录 (默认: build/bin/Release)")
    parser.add_argument("--no-build", action="store_true", help="跳过编译检查")
    args = parser.parse_args()

    exe_dir = args.exe_dir
    cc_exe = os.path.join(exe_dir, "cc_3Dreader.exe" if sys.platform == "win32" else "cc_3Dreader")
    bench_exe = os.path.join(exe_dir, "bench_e2e.exe" if sys.platform == "win32" else "bench_e2e")

    if not args.no_build:
        if not os.path.exists(cc_exe) or not os.path.exists(bench_exe):
            print(f"[!] 可执行文件不存在，请先编译:")
            print(f"    cmake -B build -S .")
            print(f"    cmake --build build --config Release")
            sys.exit(1)

    c3dr_path = args.input + ".c3dr"
    log_path = os.path.join(log_dir, args.input + "_results_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".txt")

    with open(log_path, "w", encoding="utf-8") as log_fp:
        write_both(log_fp, "=" * 50)
        write_both(log_fp, "三维数据评分基准测试")
        write_both(log_fp, f"测试时间:   {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        write_both(log_fp, "=" * 50)
        write_both(log_fp, f"输入文件:   {args.input}")
        write_both(log_fp, f"数据维度:   {args.dim_x} × {args.dim_y} × {args.dim_z}")
        write_both(log_fp, f"缓存大小:   {args.cache_size} MB")
        write_both(log_fp, f"输出文件:   {args.output}")
        write_both(log_fp, f"日志文件:   {log_path}")
        write_both(log_fp, "=" * 50)

        write_both(log_fp)

        # Step 1: 落盘 (如尚未存在)
        if not os.path.exists(c3dr_path):
            write_both(log_fp, "[1/2] 切块落盘...")
            cmd = [
                cc_exe, "-i", args.input,
                "--dim-x", str(args.dim_x),
                "--dim-y", str(args.dim_y),
                "--dim-z", str(args.dim_z),
                "--skip-bytes", str(args.skip_bytes)
            ]
            result = run_cmd(cmd, log_fp)
            if result.returncode != 0:
                write_both(log_fp, "[!] 落盘失败")
                sys.exit(1)
        else:
            write_both(log_fp, "[1/2] 落盘文件已存在: " + c3dr_path)

        write_both(log_fp)

        # Step 2: 评分测试
        write_both(log_fp, "[2/2] 运行评分测试...")
        cmd = [
            bench_exe, c3dr_path,
            "--output", args.output,
            "--cache-size", str(args.cache_size)
        ]
        result = run_cmd(cmd, log_fp)
        if result.returncode != 0:
            write_both(log_fp, "[!] 测试失败")
            sys.exit(1)

        write_both(log_fp)

        # 检查输出文件
        if os.path.exists(args.output):
            size_bytes = os.path.getsize(args.output)
            size_gb = size_bytes / (1024 ** 3)
            write_both(log_fp, f"输出文件大小: {size_gb:.2f} GB ({size_bytes} bytes)")

        write_both(log_fp)
        write_both(log_fp, "完成!")


if __name__ == "__main__":
    main()
