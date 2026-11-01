"""假 provider：不装任何模型也能把整条管线跑通（测试和界面联调用）。

separate：每个请求的分轨写一段很短的静音 wav。
transcribe：按指定的乐器（或分轨名）各写一条轨，轨名照 MuScriptor 的样子（下划线换空格）。
"""
from __future__ import annotations

import wave

from ..base import Artifact, Capability, Context, Manifest, Provider, Task
from ..midiwrite import TrackOut, write_smf
from .muscriptor import INSTRUMENTS, track_name

STEMS = ("vocals", "drums", "bass", "guitar", "piano", "other")


class Fake(Provider):
    manifest = Manifest(
        id="fake", version="1", kind="local",
        capabilities=(Capability.SEPARATE, Capability.TRANSCRIBE, Capability.COVER),
        description="测试用：不跑模型，产出形状正确的假结果", instruments=INSTRUMENTS, stems=STEMS,
    )

    def run(self, task: Task, ctx: Context) -> list[Artifact]:
        self.calls = getattr(self, "calls", 0) + 1
        ctx.progress(0.5, f"fake {task.capability.value}")
        if task.capability == Capability.SEPARATE:
            out = []
            for stem in task.params.get("stems") or STEMS:
                p = ctx.workdir / f"{stem}.wav"
                _silence(p)
                out.append(Artifact("audio", p, {"stem": stem}))
            return out
        if task.capability == Capability.COVER:
            p = ctx.workdir / "cover_1.wav"
            _silence(p)
            return [Artifact("audio", p, {"variant": 1}, {"provider": "fake", "taskId": "t", "audioId": "a"})]
        names = task.params.get("instruments") or [task.inputs[0].meta.get("stem", "acoustic_piano")]
        tracks = []
        for i, name in enumerate(names):
            drum = name == "drums"
            base = 36 if drum else 48 + 2 * i
            notes = [(k * 480, k * 480 + 400, base + (0 if drum else k % 5), 90) for k in range(8)]
            tracks.append(TrackOut(track_name(name), notes, drum=drum))
        p = ctx.workdir / "transcription.mid"
        write_smf(p, tracks)
        return [Artifact("midi", p, {"instruments": list(names)})]


def _silence(path, seconds: float = 0.1, rate: int = 8000) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"\x00\x00" * int(seconds * rate))
