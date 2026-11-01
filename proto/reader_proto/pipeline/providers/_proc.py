"""跑外部命令：stderr 逐行当进度转出去，可以取消。"""
from __future__ import annotations

import os
import shutil
import subprocess
import threading
from collections import deque
from pathlib import Path

from ..base import Cancelled, Context, PipelineError


def command(cfg: dict, default: list[str]) -> list[str]:
    cmd = cfg.get("command", default)
    return [cmd] if isinstance(cmd, str) else list(cmd)


def missing_command(cmd: list[str]) -> list[str]:
    return [] if shutil.which(cmd[0]) else [f"找不到命令 {cmd[0]!r}（在 providers.json 里用 \"command\" 指定）"]


def run(cmd: list[str], ctx: Context, cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    ctx.progress(None, "$ " + " ".join(cmd))
    full_env = None
    if env:
        full_env = dict(os.environ)
        full_env.update(env)
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=full_env,
                            stdin=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace")
    tail: deque[str] = deque(maxlen=20)

    def pump() -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            line = line.rstrip()
            if line:
                tail.append(line)
                ctx.progress(None, line)

    t = threading.Thread(target=pump, daemon=True)
    t.start()
    while True:
        try:
            proc.wait(timeout=0.2)
            break
        except subprocess.TimeoutExpired:
            if ctx.cancelled:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                raise Cancelled("cancelled")
    t.join(timeout=2)
    if proc.returncode != 0:
        raise PipelineError(f"{cmd[0]} 退出码 {proc.returncode}：\n" + "\n".join(tail))
