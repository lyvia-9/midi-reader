"""MuScriptor：全曲多乐器扒谱（github.com/muscriptor/muscriptor）。

用法对照 muscriptor 0.4.0 的命令行：
  muscriptor transcribe <audio> -o <out.mid> --format midi --model medium --instruments a,b
输出的 MIDI 一件乐器一轨，轨名是乐器组名把下划线换成空格（“string ensemble”）。
权重是 CC BY-NC 4.0（只许非商业用途），放在 Hugging Face 上且是 gated：要先在
huggingface.co/MuScriptor/muscriptor-<small|medium|large> 页面同意许可，再在 providers.json 里填
"hfToken"（或设环境变量 HF_TOKEN，或在本机跑一次 `uvx hf auth login`）。
Windows 上用 NVIDIA 显卡：命令改成 ["uvx", "--torch-backend=cu128", "muscriptor@0.4.0"]。
"""
from __future__ import annotations

from ..base import Artifact, Capability, Context, Manifest, PipelineError, Provider, Task
from . import _proc

PINNED = "0.4.0"

# muscriptor 0.4.0 tokenizer/mt3.py 的 MT3_FULL_PLUS_GROUP_NAMES，`muscriptor list-instruments` 同此
INSTRUMENTS = (
    "acoustic_piano", "electric_piano", "chromatic_percussion", "organ", "acoustic_guitar",
    "clean_electric_guitar", "distorted_electric_guitar", "acoustic_bass", "electric_bass",
    "violin", "viola", "cello", "contrabass", "orchestral_harp", "timpani", "string_ensemble",
    "synth_strings", "voice", "orchestra_hit", "trumpet", "trombone", "tuba", "french_horn",
    "brass_section", "soprano_and_alto_sax", "tenor_sax", "baritone_sax", "oboe", "english_horn",
    "bassoon", "clarinet", "flutes", "synth_lead", "synth_pad", "drums",
)

# 透传给命令行的可选项：配置字段 → 参数
PASSTHROUGH = {"device": "--device", "dtype": "--dtype", "bpm": "--bpm", "beatsPerBar": "--beats-per-bar",
               "grid": "--grid", "fillGaps": "--fill-gaps"}


def track_name(group: str) -> str:
    """MuScriptor 写进 MIDI 的轨名。"""
    return group.replace("_", " ")


class MuScriptor(Provider):
    manifest = Manifest(
        id="muscriptor", version=PINNED, kind="local", capabilities=(Capability.TRANSCRIBE,),
        description="全曲多乐器扒谱，可以指定曲中有哪些乐器", instruments=INSTRUMENTS, adapters=True,
    )

    def _cmd(self) -> list[str]:
        return _proc.command(self.config, ["uvx", f"muscriptor@{PINNED}"])

    def problems(self) -> list[str]:
        return _proc.missing_command(self._cmd())

    def cache_identity(self) -> dict:
        keys = ("model", "quantize", *PASSTHROUGH)
        return {k: self.config[k] for k in keys if k in self.config and k != "device"}

    def build_command(self, task: Task, out) -> list[str]:
        audio = task.inputs[0]
        if audio.kind != "audio" or audio.path is None:
            raise PipelineError("muscriptor 需要一个本地音频文件")
        cmd = self._cmd() + ["transcribe", str(audio.path), "-o", str(out), "--format", "midi"]
        # 微调权重：--model 接受本地 safetensors 路径（见 docs/audio-pipeline.md 的 LoRA 一节）
        model = task.params.get("adapter") or self.config.get("model")
        if model:
            cmd += ["--model", str(model)]
        instruments = task.params.get("instruments") or []
        unknown = [i for i in instruments if i not in INSTRUMENTS]
        if unknown:
            raise PipelineError(f"muscriptor 不认识这些乐器：{', '.join(unknown)}")
        if instruments:
            cmd += ["--instruments", ",".join(instruments)]
        if self.config.get("quantize"):
            cmd += ["--quantize"]
        for key, flag in PASSTHROUGH.items():
            if key in self.config:
                cmd += [flag, str(self.config[key])]
        return cmd

    def run(self, task: Task, ctx: Context) -> list[Artifact]:
        out = ctx.workdir / "transcription.mid"
        # 权重在 Hugging Face 上是 gated 的：先在模型页同意许可，再给一个 read token
        env = {"HF_TOKEN": self.config["hfToken"]} if self.config.get("hfToken") else None
        _proc.run(self.build_command(task, out), ctx, env=env)
        if not out.exists():
            raise PipelineError("muscriptor 没有写出 MIDI")
        return [Artifact("midi", out, {"instruments": list(task.params.get("instruments") or [])})]
