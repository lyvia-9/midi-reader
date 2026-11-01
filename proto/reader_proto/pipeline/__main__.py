"""音频管线命令行（都在 proto/ 目录里跑）。

    python -m reader_proto.pipeline providers                       # 有哪些 provider、能不能用
    python -m reader_proto.pipeline presets                         # 内置编制预设
    python -m reader_proto.pipeline transcribe song.wav --preset pop-band [--route full|stems]
    python -m reader_proto.pipeline cover song.wav --style "lofi jazz" [--then-transcribe pop-band]
    python -m reader_proto.pipeline credits                         # sunoapi 剩余点数
    python -m reader_proto.pipeline worker                          # 给阅读器用的 stdio 接口
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .. import cli as reader_cli
from .. import sidecar as S
from ..smf import read_smf
from . import presets as PR
from . import providers as REG
from .base import Artifact, Context, PipelineError
from .config import Config, config_path
from .run import Runner


def _print_event(e: dict) -> None:
    if e["event"] == "step":
        print(f"\n▶ {e['step']}  {e['provider']} · {e['capability']}", file=sys.stderr)
    elif e["event"] == "progress" and e.get("message"):
        print(f"  {e['message']}", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="reader_proto.pipeline", description="音频 → MIDI / remix 管线")
    ap.add_argument("--config", type=Path, help=f"默认 {config_path()}")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("providers", help="列出 provider 和缺什么")
    sub.add_parser("presets", help="列出内置预设")
    sub.add_parser("credits", help="sunoapi 剩余点数")
    sub.add_parser("worker", help="stdin/stdout JSON 行协议")

    p = sub.add_parser("transcribe", help="音频 → 按预设整理好的 MIDI + sidecar")
    p.add_argument("audio", type=Path)
    p.add_argument("--preset", required=True, help="预设 id，或一个 .json 文件")
    p.add_argument("--route", choices=PR.ROUTES, help="full = 全曲扒谱；stems = 先分离再逐轨扒。默认看预设")
    p.add_argument("--transcriber", help="全曲路线用哪个 provider（默认看预设，一般是 muscriptor）")
    p.add_argument("--separator", help="分轨路线用哪个 provider（默认看预设，一般是 demucs）")
    p.add_argument("--out-dir", type=Path)

    p = sub.add_parser("cover", help="音频 → 换风格的新音频（sunoapi upload-cover）")
    p.add_argument("audio", type=Path)
    p.add_argument("--style", required=True)
    p.add_argument("--title")
    p.add_argument("--lyrics")
    p.add_argument("--instrumental", action="store_true")
    p.add_argument("--provider", default="sunoapi")
    p.add_argument("--out-dir", type=Path)
    p.add_argument("--then-transcribe", metavar="PRESET", help="翻唱完把第一个版本按这个预设扒成 MIDI")
    p.add_argument("--route", choices=PR.ROUTES)
    p.add_argument("--transcriber")

    args = ap.parse_args(argv)
    config = Config.load(args.config)

    if args.cmd == "worker":
        from .worker import Worker
        Worker(sys.stdout, config).serve(sys.stdin)
        return 0
    if args.cmd == "providers":
        for pid in REG.BUILTIN:
            prov = REG.create(pid, config)
            m = prov.manifest
            probs = prov.problems()
            state = "可用" if not probs else "不可用：" + "；".join(probs)
            print(f"{m.id:<12} {m.kind:<6} {','.join(c.value for c in m.capabilities):<28} {state}")
            print(f"{'':<12} {m.description}")
        print(f"\n配置文件：{config.path}\n缓存目录：{config.cache_dir}")
        return 0
    if args.cmd == "presets":
        for pr in PR.builtin().values():
            print(f"{pr.id:<16} {pr.name}（{pr.route}）  " + " / ".join(s.lane for s in pr.slots))
            if pr.description:
                print(f"{'':<16} {pr.description}")
        return 0
    if args.cmd == "credits":
        prov = REG.create("sunoapi", config)
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            print(prov.credits(Context(Path(d))))
        return 0

    runner = Runner(config, on_event=_print_event)
    try:
        if args.cmd == "transcribe":
            r = runner.transcribe(Artifact("audio", args.audio), PR.resolve(args.preset), args.route,
                                  args.transcriber, args.separator, args.out_dir)
            _report(r.midi, r.run_dir)
            return 0
        if args.cmd == "cover":
            params = {"style": args.style, "title": args.title, "lyrics": args.lyrics,
                      "instrumental": args.instrumental or None}
            run_dir, arts = runner.cover(Artifact("audio", args.audio), params, args.provider, args.out_dir)
            print(f"\n运行目录 {run_dir}")
            for a in arts:
                print(f"  {a.path}")
            if args.then_transcribe:
                r = runner.transcribe(arts[0], PR.resolve(args.then_transcribe), args.route, args.transcriber,
                                      out_dir=run_dir / f"transcribe-{args.then_transcribe}")
                _report(r.midi, r.run_dir)
            return 0
    except PipelineError as e:
        print(f"\n失败：{e}", file=sys.stderr)
        return 1
    return 2


def _report(midi: Path, run_dir: Path) -> None:
    print(f"\n运行目录 {run_dir}\nMIDI     {midi}\n")
    smf = read_smf(midi)
    reader_cli._print_report(smf, S.load(S.sidecar_path_for(midi)))


if __name__ == "__main__":
    sys.exit(main())
