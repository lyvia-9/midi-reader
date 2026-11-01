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

阅读器实际用的是单次模式 `job`（见 run_job）：JUCE 的 ChildProcess 只能读子进程的 stdout，
写不了 stdin，所以请求放在一个 JSON 文件里，取消靠建一个约定好的文件，日志写到另一个文件。
"""
from __future__ import annotations

import json
import sys
import threading
import time
import traceback
from pathlib import Path
from typing import IO, Any

from . import presets as PR
from . import providers as REG
from .base import Artifact, Cancelled, PipelineError, ProviderUnavailable
from .config import Config, config_path
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
                self.send({"id": rid, "result": self._providers()})
            elif method == "presets":
                self.send({"id": rid, "result": _presets()})
            elif method == "catalog":
                # 阅读器打开音频时要的一切：预设、provider 能不能用、配置文件在哪
                self.send({"id": rid, "result": {
                    "protocol": PROTOCOL, "presets": _presets(), "providers": self._providers(),
                    "config": str(self.config.path or config_path()), "cache": str(self.config.cache_dir)}})
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

    def _providers(self) -> list[dict]:
        return [{**p.manifest.to_json(), "problems": p.problems()}
                for p in (REG.create(pid, self.config) for pid in REG.BUILTIN)]

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
                    result: dict = {"run": str(run_dir), "audio": [str(a.path) for a in arts]}
                    then = params.get("thenTranscribe")
                    if then:   # 和命令行的 --then-transcribe 一样：拿第一个版本接着扒
                        r = runner.transcribe(arts[0], PR.resolve(then["preset"]), then.get("route"),
                                              then.get("transcriber"), then.get("separator"),
                                              run_dir / f"transcribe-{PR.resolve(then['preset']).id}")
                        result.update(midi=str(r.midi), sidecar=str(r.sidecar), transcribeRun=str(r.run_dir))
                    self.send({"id": rid, "result": result})
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


def _presets() -> list[dict]:
    out = []
    for p in PR.builtin().values():
        # 分轨路线要每个声部都指定了分哪一轨；阅读器据此决定能不能选“分轨”，以及要检查哪些 provider
        stems_ok = all(s.stem for s in p.slots)
        out.append({"id": p.id, "name": p.name, "route": p.route, "description": p.description,
                    "lanes": [s.lane for s in p.slots], "transcriber": p.transcriber, "separator": p.separator,
                    "stemTranscribers": sorted({s.transcriber or "basic-pitch" for s in p.slots}) if stems_ok else None})
    return out


def run_job(request_path: Path, out: IO[str] = sys.stdout, config: Config | None = None,
            runner_factory=None, poll: float = 0.2) -> int:
    """单次模式：读一个请求文件，跑完一个请求就退出。

    请求文件：{"method": ..., "params": {...}, "cancelFile": "...", "logFile": "..."}
    stdout 和 serve() 一样一行一个 JSON（id 固定是 1）。cancelFile 出现就取消；
    logFile 给了就把 stderr（包括 Python 的异常栈）写过去，免得混进 stdout。
    """
    req = json.loads(Path(request_path).read_text(encoding="utf-8"))
    log = None
    if req.get("logFile"):
        log = open(req["logFile"], "a", encoding="utf-8", buffering=1)
        sys.stderr = log
    try:
        w = Worker(out, config, runner_factory)
        w.handle(json.dumps({"id": 1, "method": req.get("method"), "params": req.get("params") or {}}))
        cancel_file = Path(req["cancelFile"]) if req.get("cancelFile") else None
        while any(t.is_alive() for t in w.threads):
            if cancel_file is not None and cancel_file.exists() and 1 in w.jobs:
                w.jobs[1].set()
            time.sleep(poll)
        for t in w.threads:
            t.join()
        return 0
    finally:
        if log is not None:
            sys.stderr = sys.__stderr__
            log.close()


def _error(e: Exception) -> dict:
    kind = ("cancelled" if isinstance(e, Cancelled) else "unavailable" if isinstance(e, ProviderUnavailable)
            else "failed" if isinstance(e, PipelineError) else "badRequest" if isinstance(e, (KeyError, ValueError))
            else "internal")
    return {"type": kind, "message": str(e)}


def main() -> int:
    Worker(sys.stdout).serve(sys.stdin)
    return 0
