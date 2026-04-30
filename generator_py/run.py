#!/usr/bin/env python3
"""便捷启动脚本：python run.py 启动GUI，python run.py --cli 启动命令行模式"""
import sys
import os

# 确保可以导入同目录模块
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

if "--cli" in sys.argv:
    sys.argv.remove("--cli")
    from cli import main
    main()
else:
    from gui import GeneratorGUI
    app = GeneratorGUI()
    app.mainloop()
