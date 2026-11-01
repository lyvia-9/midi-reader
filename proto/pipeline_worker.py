"""阅读器启动音频管线的入口：python pipeline_worker.py job request.json

用脚本而不是 `python -m reader_proto.pipeline`，是因为 Python 会把脚本所在目录放进 sys.path，
阅读器就不用设工作目录或 PYTHONPATH（JUCE 的 ChildProcess 两样都设不了）。
"""
import os
import sys

# 阅读器只接了 stdout：Windows 上 stderr 句柄是空的，sys.stderr 会是 None，
# 往里写东西（比如 argparse 报错）就会崩。正式日志由 job 模式写到 logFile。
if sys.stderr is None:
    sys.stderr = open(os.devnull, "w")

from reader_proto.pipeline.__main__ import main

if __name__ == "__main__":
    sys.exit(main())
