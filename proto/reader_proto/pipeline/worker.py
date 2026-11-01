"""给阅读器（C++）用的进程外接口：stdin/stdout 上一行一个 JSON。

阅读器启动 `python -m reader_proto.pipeline worker`，然后：

  → {"id": 1, "method": "providers"}
  ← {"id": 1, "result": [{"id": "muscriptor", ..., "problems": []}, ...]}
  → {"id": 2, "method": "transcribe", "params": {"audio": "song.wav", "preset": "pop-band"}}
  ← {"id": 2, "event": "step", "step": "01-separate", "provider": "demucs", ...}
  ← {"id": 2, "event": "progress", "step": "01-separate", "fraction": null, "message": "..."}
  ← {"id": 2, "result": {"run": "...", "midi": "...", "sidecar": "..."}}
  → {"id": 3, "method": "cancel", "params": {"id": 2}}
  ← {"id": 2, "error": {"type": "cancelled", "message": "cancelled"}}

每个任务一个线程，所以跑着的时候也能收 cancel。stdout 只写协议，日志一律走 stderr。
"""
from __future__ import annotations

import json
import sys
import threading
import traceback
from pathlib import Path
from typing import IO, Any

from . import presets as PR
from . import providers as REG
from .base import Artifact, Cancelled, PipelineError, ProviderUnavailable
from .config import Config
from .run import Runner

PROTOCOL = 1


class Worker:
    def __init__(self, out: IO[str], config: Config | None = None, runner_factory=None):
        self.out = out
        self.config = config or Config.load()
        self.lock = threading.Lock()
        self.jobs: dict[Any, threading.Event] = {}
        self.threads: list[threading.Thread] = []
        self.runner_factory = runner_factory or (lambda on_event, cancel: Runner(self.config, on_event=on_event, cancel=cancel))

    def send(self, msg: dict) -> None:
        with self.lock:
            self.out.write(json.dumps(msg, ensure_ascii=False) + "\n")
            self.out.flush()

    def handle(self, line: str) -> None:
        try:
            req = json.loads(line)
        except ValueError as e:
            self.send({"id": None, "error": {"type": "badRequest", "message": str(e)}})
            return
        rid, method, params = req.get("id"), req.get("method"), req.get("params") or {}
        try:
            if method == "hello":
                self.send({"id": rid, "result": {"protocol": PROTOCOL}})
            elif method == "providers":
                self.send({"id": rid, "result": [
                    {**p.manifest.to_json(), "problems": p.problems()}
                    for p in (REG.create(pid, self.config) for pid in REG.BUILTIN)]})
            elif method == "presets":
                self.send({"id": rid, "result": [
                    {"id": p.id, "name": p.name, "route": p.route, "description": p.description,
                     "lanes": [s.lane for s in p.slots]} for p in PR.builtin().values()]})
            elif method in ("transcribe", "cover"):
                self._start(rid, method, params)
            elif method == "cancel":
                ev = self.jobs.get(params.get("id"))
                if ev is not None:
                    ev.set()
                self.send({"id": rid, "result": {"cancelled": ev is not None}})
            else:
                self.send({"id": rid, "error": {"type": "badRequest", "message": f"unknown method {method!r}"}})
        except Exception as e:  # noqa: BLE001 —— 协议层兜底，别让一个坏请求弄死 worker
            self.send({"id": rid, "error": _error(e)})

    def _start(self, rid: Any, method: str, params: dict) -> None:
        cancel = threading.Event()
        self.jobs[rid] = cancel

        def job() -> None:
            try:
                runner = self.runner_factory(lambda e: self.send({"id": rid, **e}), cancel)
                src = Artifact("audio", Path(params["audio"]))
                out_dir = Path(params["outDir"]) if params.get("outDir") else None
                if method == "transcribe":
                    r = runner.transcribe(src, PR.resolve(params["preset"]), params.get("route"),
                                          params.get("transcriber"), params.get("separator"), out_dir)
                    self.send({"id": rid, "result": {"run": str(r.run_dir), "midi": str(r.midi), "sidecar": str(r.sidecar)}})
                else:
                    run_dir, arts = runner.cover(src, params.get("options") or {}, params.get("provider", "sunoapi"), out_dir)
                    self.send({"id": rid, "result": {"run": str(run_dir), "audio": [str(a.path) for a in arts]}})
            except Exception as e:  # noqa: BLE001
                if not isinstance(e, (PipelineError, KeyError)):
                    traceback.print_exc(file=sys.stderr)
                self.send({"id": rid, "error": _error(e)})
            finally:
                self.jobs.pop(rid, None)

        t = threading.Thread(target=job, daemon=True)
        self.threads.append(t)
        t.start()

    def serve(self, inp: IO[str]) -> None:
        for line in inp:
            if line.strip():
                self.handle(line)
        for t in self.threads:
            t.join()


def _error(e: Exception) -> dict:
    kind = ("cancelled" if isinstance(e, Cancelled) else "unavailable" if isinstance(e, ProviderUnavailable)
            else "failed" if isinstance(e, PipelineError) else "badRequest" if isinstance(e, (KeyError, ValueError))
            else "internal")
    return {"type": kind, "message": str(e)}


def main() -> int:
    Worker(sys.stdout).serve(sys.stdin)
    return 0
