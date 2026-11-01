"""把 provider 串成管线：每一步查缓存、跑、留档；一次运行一个目录、一个 run.json。

  song.wav → song.wav.pipeline/<时间>-<名字>/
               run.json              来源记录：源音频指纹、预设、每一步的 provider/版本/参数/缓存键
               01-separate/…         每一步的产物（从缓存硬链接或复制过来）
               song.mid              最终 MIDI（阅读器打开这个）
               song.mid.reader.json  按预设整理好的 sidecar

原 MIDI 不动的原则照旧：扒出来的 MIDI 就是这个目录里的“原 MIDI”，sidecar 放在它旁边。
"""
from __future__ import annotations

import json
import os
import shutil
import tempfile
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

from .. import analyze as A
from .. import sidecar as S
from ..smf import read_smf
from . import merge as M
from . import presets as PR
from . import providers as REG
from .base import Artifact, Capability, Context, PipelineError, Provider, ProviderUnavailable, Task, sha256_file
from .cache import Cache, cache_key
from .config import Config

RUN_FORMAT = "midi-reader-pipeline-run"
RUN_VERSION = 1
RUN_FILE = "run.json"

# 事件：{"event": "step"|"progress"|"done", ...}；worker 原样转给界面
EventFn = Callable[[dict], None]


@dataclass
class TranscribeResult:
    run_dir: Path
    midi: Path
    sidecar: Path


class Runner:
    def __init__(self, config: Config | None = None, cache: Cache | None = None,
                 on_event: EventFn | None = None, cancel: threading.Event | None = None,
                 providers: dict[str, Provider] | None = None):
        self.config = config or Config()
        self.cache = cache if cache is not None else Cache(self.config.cache_dir)
        self.on_event = on_event or (lambda e: None)
        self.cancel = cancel or threading.Event()
        self._providers = dict(providers or {})

    def provider(self, pid: str) -> Provider:
        if pid not in self._providers:
            self._providers[pid] = REG.create(pid, self.config)
        return self._providers[pid]

    # ---- 一步 ----
    def step(self, run: "RunRecord", pid: str, task: Task, label: str) -> list[Artifact]:
        p = self.provider(pid)
        if not p.supports(task.capability):
            raise PipelineError(f"{pid} 不会 {task.capability.value}")
        problems = p.problems()
        if problems:
            raise ProviderUnavailable(f"{pid}：" + "；".join(problems))
        step_id = f"{len(run.steps) + 1:02d}-{label}"
        key = cache_key(p, task)
        self.on_event({"event": "step", "step": step_id, "provider": pid, "capability": task.capability.value})
        t0 = time.monotonic()
        arts = self.cache.get(key)
        cached = arts is not None
        if arts is None:
            if self.cache.root is not None:
                work = self.cache.begin(key)
            else:
                work = Path(tempfile.mkdtemp(prefix="midi-reader-"))
            ctx = Context(work, p.config, cancel=self.cancel,
                          progress=lambda f, m: self.on_event({"event": "progress", "step": step_id,
                                                                "fraction": f, "message": m}))
            try:
                arts = p.run(task, ctx)
            except BaseException:
                if self.cache.root is not None:
                    self.cache.abort(work)
                raise
            if self.cache.root is not None:
                arts = self.cache.commit(key, work, arts, {"provider": pid, "version": p.manifest.version})
        local = [_place(a, run.dir / step_id) for a in arts]
        run.steps.append({
            "id": step_id, "provider": pid, "version": p.manifest.version, "capability": task.capability.value,
            "params": task.params, "inputs": [_brief(a, run.dir) for a in task.inputs],
            "outputs": [a.to_json(run.dir) for a in local], "cacheKey": key, "cached": cached,
            "seconds": round(time.monotonic() - t0, 2),
        })
        run.save()
        return local

    # ---- 配方 ----
    def transcribe(self, source: Artifact, preset: PR.Preset, route: str | None = None,
                   transcriber: str | None = None, separator: str | None = None,
                   out_dir: Path | None = None) -> TranscribeResult:
        route = route or preset.route
        if route not in PR.ROUTES:
            raise PipelineError(f"route 只能是 {' / '.join(PR.ROUTES)}")
        run = RunRecord.create(source, "transcribe", preset.id, out_dir)
        run.data.update({"preset": preset.id, "route": route})
        stem_name = Path(source.path).stem if source.path else "audio"
        midi = run.dir / f"{stem_name}.mid"

        if route == "full":
            pid = transcriber or preset.transcriber
            params: dict[str, Any] = {}
            if self.provider(pid).manifest.instruments and preset.muscriptor_instruments:
                params["instruments"] = preset.muscriptor_instruments
            out = self.step(run, pid, Task(Capability.TRANSCRIBE, [source], params), "transcribe")
            shutil.copyfile(_first(out, "midi").path, midi)
        else:
            sep = separator or preset.separator
            stems = self.step(run, sep, Task(Capability.SEPARATE, [source], {"stems": preset.stems}), "separate")
            by_stem = {a.meta.get("stem"): a for a in stems}
            parts = []
            for slot in preset.slots:
                audio = by_stem.get(slot.stem)
                if audio is None:
                    raise PipelineError(f"{sep} 没有分出 {slot.stem}（声部 {slot.lane}）")
                pid = slot.transcriber or "basic-pitch"
                params = {}
                if self.provider(pid).manifest.instruments and slot.muscriptor:
                    params["instruments"] = slot.muscriptor
                out = self.step(run, pid, Task(Capability.TRANSCRIBE, [audio], params), f"transcribe-{slot.stem}")
                parts.append(M.Part(_first(out, "midi").path, slot.lane, drum=slot.instrument == "drums"))
            M.merge(parts, midi)

        smf = read_smf(midi)
        sc = PR.apply(smf, A.analyze(smf), preset)
        sc_path = S.sidecar_path_for(midi)
        S.save(sc, sc_path)
        run.data["result"] = {"midi": midi.name, "sidecar": sc_path.name}
        run.save()
        self.on_event({"event": "done", "run": str(run.dir), "midi": str(midi)})
        return TranscribeResult(run.dir, midi, sc_path)

    def cover(self, source: Artifact, params: dict[str, Any], provider: str = "sunoapi",
              out_dir: Path | None = None) -> tuple[Path, list[Artifact]]:
        run = RunRecord.create(source, "cover", provider, out_dir)
        out = self.step(run, provider, Task(Capability.COVER, [source], params), "cover")
        run.data["result"] = {"audio": [a.to_json(run.dir) for a in out]}
        run.save()
        self.on_event({"event": "done", "run": str(run.dir), "audio": [str(a.path) for a in out]})
        return run.dir, out


class RunRecord:
    def __init__(self, directory: Path, data: dict):
        self.dir = directory
        self.data = data

    @property
    def steps(self) -> list[dict]:
        return self.data["steps"]

    @staticmethod
    def create(source: Artifact, kind: str, label: str, out_dir: Path | None) -> "RunRecord":
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        if out_dir is None:
            if source.path is None:
                raise PipelineError("远程输入要指定输出目录")
            base = Path(source.path).with_name(Path(source.path).name + ".pipeline")
            out_dir = base / f"{stamp}-{kind}-{label}"
            n = 2
            while out_dir.exists():
                out_dir = base / f"{stamp}-{kind}-{label}-{n}"
                n += 1
        out_dir.mkdir(parents=True, exist_ok=True)
        src: dict[str, Any] = {}
        if source.path is not None:
            src = {"path": str(Path(source.path).resolve()), "sha256": sha256_file(source.path)}
        if source.remote:
            src["remote"] = source.remote
        r = RunRecord(out_dir, {
            "format": RUN_FORMAT, "version": RUN_VERSION, "kind": kind,
            "createdAt": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "source": src, "steps": [],
        })
        r.save()
        return r

    def save(self) -> None:
        (self.dir / RUN_FILE).write_text(json.dumps(self.data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _place(a: Artifact, dest: Path) -> Artifact:
    """缓存里的产物放进运行目录：能硬链接就硬链接（不占双份空间），不行就复制。"""
    if a.path is None:
        return a
    dest.mkdir(parents=True, exist_ok=True)
    target = dest / Path(a.path).name
    if target.exists():
        target.unlink()
    try:
        os.link(a.path, target)
    except OSError:
        shutil.copyfile(a.path, target)
    return Artifact(a.kind, target, dict(a.meta), a.remote)


def _brief(a: Artifact, base: Path) -> dict:
    return a.to_json(base)


def _first(arts: list[Artifact], kind: str) -> Artifact:
    for a in arts:
        if a.kind == kind and a.path is not None:
            return a
    raise PipelineError(f"这一步没有产出 {kind}")
