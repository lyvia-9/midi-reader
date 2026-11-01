"""Basic Pitch：单轨复音扒谱（对照 basic-pitch 0.4.0 的命令行）。

  basic-pitch <outdir> <audio>   →  <outdir>/<文件名>_basic_pitch.mid
不分乐器，一次扒一件乐器效果最好；不扒鼓。
"""
from __future__ import annotations

from pathlib import Path

from ..base import Artifact, Capability, Context, Manifest, PipelineError, Provider, Task
from . import _proc

PINNED = "0.4.0"
# 2026-11 实测：TensorFlow 不兼容 numpy 2，resampy 还要 pkg_resources（setuptools 81 起去掉了）
DEFAULT_COMMAND = ["uvx", "--python", "3.11", "--with", "numpy<2", "--with", "setuptools<81", f"basic-pitch@{PINNED}"]
PASSTHROUGH = {"onsetThreshold": "--onset-threshold", "frameThreshold": "--frame-threshold",
               "minimumNoteLength": "--minimum-note-length", "midiTempo": "--midi-tempo"}


class BasicPitch(Provider):
    manifest = Manifest(
        id="basic-pitch", version=PINNED, kind="local", capabilities=(Capability.TRANSCRIBE,),
        description="单轨扒谱（不分乐器、不扒鼓），配合分离用",
    )

    def _cmd(self) -> list[str]:
        return _proc.command(self.config, DEFAULT_COMMAND)

    def problems(self) -> list[str]:
        return _proc.missing_command(self._cmd())

    def cache_identity(self) -> dict:
        return {k: self.config[k] for k in PASSTHROUGH if k in self.config}

    def build_command(self, task: Task, workdir: Path) -> list[str]:
        audio = task.inputs[0]
        if audio.kind != "audio" or audio.path is None:
            raise PipelineError("basic-pitch 需要一个本地音频文件")
        cmd = self._cmd() + [str(workdir), str(audio.path)]
        for key, flag in PASSTHROUGH.items():
            if key in self.config:
                cmd += [flag, str(self.config[key])]
        return cmd

    def run(self, task: Task, ctx: Context) -> list[Artifact]:
        audio = task.inputs[0]
        _proc.run(self.build_command(task, ctx.workdir), ctx)
        out = ctx.workdir / f"{Path(audio.path).stem}_basic_pitch.mid"
        if not out.exists():
            raise PipelineError(f"basic-pitch 没有写出 {out.name}")
        return [Artifact("midi", out, dict(audio.meta))]
