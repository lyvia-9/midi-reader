"""音频管线的公共类型：能力、清单、产物、任务、运行上下文、provider 基类。

一个 provider 是“一个能做几件事的模型或服务”：本地命令行（MuScriptor、Demucs、
Basic Pitch）和远程 API（sunoapi.org）用同一个接口。格式说明见 docs/audio-pipeline.md。
"""
from __future__ import annotations

import hashlib
import threading
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable


class Capability(str, Enum):
    SEPARATE = "separate"      # 音频 → 分轨音频
    TRANSCRIBE = "transcribe"  # 音频 → MIDI
    COVER = "cover"            # 音频 + 风格 → 新音频（remix）
    GENERATE = "generate"      # 文字 → 音频（预留）
    EXTEND = "extend"          # 音频 → 续写的音频（预留）


class PipelineError(RuntimeError):
    pass


class ProviderUnavailable(PipelineError):
    """缺命令、缺 API key 之类：还没开始跑就知道跑不了。"""


class Cancelled(PipelineError):
    pass


@dataclass(frozen=True)
class Manifest:
    id: str
    version: str                          # 进缓存键：换版本就不复用旧结果
    kind: str                             # "local" | "remote"
    capabilities: tuple[Capability, ...]
    description: str = ""
    instruments: tuple[str, ...] = ()     # transcribe 能指定的乐器词表（空 = 不支持指定）
    stems: tuple[str, ...] = ()           # separate 能出的分轨名
    remoteInput: tuple[Capability, ...] = ()  # 这些能力只吃本 provider 自己产出的远程结果
    adapters: bool = False                # 能否加载微调权重 / LoRA（预留）

    def to_json(self) -> dict:
        return {
            "id": self.id, "version": self.version, "kind": self.kind,
            "capabilities": [c.value for c in self.capabilities], "description": self.description,
            "instruments": list(self.instruments), "stems": list(self.stems),
            "remoteInput": [c.value for c in self.remoteInput], "adapters": self.adapters,
        }


@dataclass
class Artifact:
    """一个产物：本地文件，或者留在远程服务上的结果（或两者都有）。"""
    kind: str                              # "audio" | "midi" | "json"
    path: Path | None = None
    meta: dict[str, Any] = field(default_factory=dict)    # 如 {"stem": "bass"}、{"instrument": "violin"}
    remote: dict[str, Any] | None = None  # 如 {"provider": "sunoapi", "taskId": "…", "audioId": "…"}

    def fingerprint(self) -> dict:
        """进缓存键的部分：本地文件按内容哈希，远程结果按引用。"""
        fp: dict[str, Any] = {"kind": self.kind, "meta": self.meta}
        if self.path is not None:
            fp["sha256"] = sha256_file(self.path)
        if self.remote is not None:
            fp["remote"] = self.remote
        return fp

    def to_json(self, relative_to: Path | None = None) -> dict:
        d: dict[str, Any] = {"kind": self.kind}
        if self.path is not None:
            p = Path(self.path)
            if relative_to is not None:
                try:
                    p = p.relative_to(relative_to)
                except ValueError:
                    pass
            d["path"] = p.as_posix()
        if self.meta:
            d["meta"] = self.meta
        if self.remote is not None:
            d["remote"] = self.remote
        return d

    @staticmethod
    def from_json(d: dict, base: Path | None = None) -> "Artifact":
        p = d.get("path")
        path = None if p is None else (base / p if base is not None and not Path(p).is_absolute() else Path(p))
        return Artifact(d["kind"], path, dict(d.get("meta", {})), d.get("remote"))


@dataclass
class Task:
    capability: Capability
    inputs: list[Artifact]
    params: dict[str, Any] = field(default_factory=dict)


ProgressFn = Callable[[float | None, str], None]


class Context:
    """provider 跑任务时能用的东西：工作目录、进度回报、取消、原始响应留档。"""

    def __init__(self, workdir: Path, config: dict[str, Any] | None = None,
                 progress: ProgressFn | None = None, cancel: threading.Event | None = None):
        self.workdir = Path(workdir)
        self.config = config or {}
        self._progress = progress
        self._cancel = cancel or threading.Event()

    def progress(self, fraction: float | None, message: str = "") -> None:
        if self._progress is not None:
            self._progress(fraction, message)

    @property
    def cancelled(self) -> bool:
        return self._cancel.is_set()

    def check_cancelled(self) -> None:
        if self._cancel.is_set():
            raise Cancelled("cancelled")

    def save_raw(self, name: str, text: str) -> Path:
        """远程 API 的原始响应存进工作目录，解析对不上时有据可查。"""
        raw = self.workdir / "raw"
        raw.mkdir(parents=True, exist_ok=True)
        p = raw / name
        p.write_text(text, encoding="utf-8")
        return p


class Provider(ABC):
    manifest: Manifest

    def __init__(self, config: dict[str, Any] | None = None):
        self.config = config or {}

    def problems(self) -> list[str]:
        """跑不了的原因（缺命令、缺 key），空 = 可用。不应有副作用、不应联网。"""
        return []

    def supports(self, cap: Capability) -> bool:
        return cap in self.manifest.capabilities

    def cache_identity(self) -> dict:
        """影响结果、所以要进缓存键的配置（模型大小等）；不放 key 之类的密钥。"""
        return {}

    @abstractmethod
    def run(self, task: Task, ctx: Context) -> list[Artifact]:
        """在 ctx.workdir 里把任务跑完，返回产物（路径在 workdir 里）。"""


def sha256_file(path: str | Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()
