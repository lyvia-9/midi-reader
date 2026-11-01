"""HT-Demucs：音轨分离（对照 demucs 4.1.0 的命令行）。

  demucs -n htdemucs_6s -o <work> --filename "{stem}.{ext}" <audio>
输出在 <work>/<模型名>/<分轨名>.wav。htdemucs_6s 比 4 轨版多出 piano、guitar，但这两轨质量一般。
"""
from __future__ import annotations

from ..base import Artifact, Capability, Context, Manifest, PipelineError, Provider, Task
from . import _proc

PINNED = "4.1.0"
STEMS = {
    "htdemucs": ("vocals", "drums", "bass", "other"),
    "htdemucs_ft": ("vocals", "drums", "bass", "other"),
    "htdemucs_6s": ("vocals", "drums", "bass", "guitar", "piano", "other"),
}
DEFAULT_MODEL = "htdemucs_6s"
# 2026-11 实测：Python 3.13 下缺 numpy，所以钉 3.12 并显式带上 numpy、soundfile
DEFAULT_COMMAND = ["uvx", "--python", "3.12", "--with", "numpy", "--with", "soundfile", f"demucs@{PINNED}"]


class Demucs(Provider):
    manifest = Manifest(
        id="demucs", version=PINNED, kind="local", capabilities=(Capability.SEPARATE,),
        description="HT-Demucs 音轨分离（4 轨或 6 轨）", stems=STEMS[DEFAULT_MODEL],
    )

    @property
    def model(self) -> str:
        return self.config.get("model", DEFAULT_MODEL)

    def _cmd(self) -> list[str]:
        return _proc.command(self.config, DEFAULT_COMMAND)

    def problems(self) -> list[str]:
        p = _proc.missing_command(self._cmd())
        if self.model not in STEMS:
            p.append(f"不认识的 demucs 模型 {self.model!r}（可选：{', '.join(STEMS)}）")
        return p

    def cache_identity(self) -> dict:
        return {"model": self.model}

    def build_command(self, task: Task, workdir) -> list[str]:
        audio = task.inputs[0]
        if audio.kind != "audio" or audio.path is None:
            raise PipelineError("demucs 需要一个本地音频文件")
        return self._cmd() + ["-n", self.model, "-o", str(workdir), "--filename", "{stem}.{ext}", str(audio.path)]

    def run(self, task: Task, ctx: Context) -> list[Artifact]:
        want = task.params.get("stems") or list(STEMS[self.model])
        bad = [s for s in want if s not in STEMS[self.model]]
        if bad:
            raise PipelineError(f"{self.model} 分不出这些轨：{', '.join(bad)}")
        _proc.run(self.build_command(task, ctx.workdir), ctx)
        out = []
        for stem in want:
            p = ctx.workdir / self.model / f"{stem}.wav"
            if not p.exists():
                raise PipelineError(f"demucs 没有写出 {p.name}")
            out.append(Artifact("audio", p, {"stem": stem}))
        return out
