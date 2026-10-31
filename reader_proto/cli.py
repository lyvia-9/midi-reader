"""命令行入口。

    python -m reader_proto run song.mid          # 分析 + 写 sidecar + 投影 JSON + 检查用 MIDI
    python -m reader_proto report song.mid       # 看规则结论
    python -m reader_proto decide song.mid ks:t2c0:low confirm|reject|reset
    python -m reader_proto note song.mid 1234 ks|not-ks|clear
    python -m reader_proto instrument song.mid L3 cello|none
    python -m reader_proto merge song.mid L4 L5 --name "小提琴 I"
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import analyze as A
from . import export as E
from . import project as P
from . import sidecar as S
from .instruments import BY_ID
from .smf import read_smf

VERDICTS = {"confirm": S.STATUS_CONFIRMED, "reject": S.STATUS_REJECTED}


def _paths(midi: str, sidecar: str | None) -> tuple[Path, Path]:
    m = Path(midi)
    return m, Path(sidecar) if sidecar else S.sidecar_path_for(m)


def _load_or_analyze(smf, sc_path: Path, reset: bool = False) -> S.Sidecar:
    existing = S.load(sc_path) if sc_path.exists() else None
    return A.analyze(smf, existing, reset=reset)


def _write_outputs(smf, sc: S.Sidecar, midi: Path, sc_path: Path, outdir: Path | None, ks_tracks: bool, quiet: bool) -> None:
    S.save(sc, sc_path)
    proj = P.project(smf, sc)
    base = (outdir or midi.parent) / midi.stem
    if outdir:
        outdir.mkdir(parents=True, exist_ok=True)
    lanes_path = Path(f"{base}.lanes.json")
    check_path = Path(f"{base}.lanes.mid")
    P.save(proj, lanes_path)
    E.export(proj, sc, smf, check_path, ks_tracks)
    if not quiet:
        print(f"sidecar   {sc_path}")
        print(f"投影 JSON {lanes_path}")
        print(f"检查 MIDI {check_path}")
        _print_report(smf, sc, proj)


def _print_report(smf, sc: S.Sidecar, proj: P.Projection | None = None) -> None:
    proj = proj or P.project(smf, sc)
    print(f"\n{Path(smf.path).name}: type {smf.format}, PPQ {smf.ppq}, {len(smf.chunks)} 轨, {smf.noteCount} 个音符")
    if smf.orphanNoteOffs or smf.unterminatedNotes or smf.retriggeredNotes:
        print(f"  解析：孤立 note-off {smf.orphanNoteOffs}，未结束音 {smf.unterminatedNotes}，同音重触发 {smf.retriggeredNotes}")
    print("\n声部：")
    for lane in proj.lanes:
        spec = next((l for l in sc.lanes if l.id == lane.id), None)
        inst = f"{BY_ID[spec.instrument].nameZh}（{spec.instrumentSource}）" if spec and spec.instrument else "—"
        flags = {"超舒适": 1, "超极限": 2, "待确认": 4, "KS": 8}
        counts = "  ".join(f"{k}{sum(1 for n in lane.notes if n.flags & v)}" for k, v in flags.items()
                           if any(n.flags & v for n in lane.notes))
        src = ",".join(spec.sources) if spec else lane.id[2:]
        print(f"  {lane.id:<6} {lane.info.name:<24} {inst:<14} [{src}]  {len(lane.notes)} 音  {counts}")
    print("\n规则结论：")
    mark = {"auto": "自动", "review": "待确认", "confirmed": "已确认", "rejected": "已否决"}
    if not sc.decisions:
        print("  （无）")
    for d in sc.decisions:
        print(f"  [{mark[d.status]}] {d.id}  {_summary(d)}\n           {d.reason}")
    if sc.noteOverrides:
        print(f"\n逐音修正 {len(sc.noteOverrides)} 条")
    for w in proj.warnings:
        print(f"\n警告：{w}")


def _summary(d: S.Decision) -> str:
    if d.rule == "channelSplit":
        return "按通道拆成 " + " / ".join(d.params["streams"])
    if d.rule == "keySwitch":
        return f"keyswitch 音高 {d.params['pitches']}"
    if d.rule == "octaveShift":
        return f"八度修正 {d.params['shift']:+d}"
    return ""


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="reader_proto", description="MIDI 阅读器 A 线：清洗规则原型")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("midi")
        p.add_argument("--sidecar", help="默认 <midi>.reader.json")
        return p

    p = common(sub.add_parser("run", help="分析并写出 sidecar、投影 JSON、检查用 MIDI"))
    p.add_argument("--out-dir", type=Path, help="投影 JSON 和检查 MIDI 的目录，默认与 MIDI 同目录")
    p.add_argument("--reset", action="store_true", help="重建声部映射（保留已确认/否决的结论和逐音修正）")
    p.add_argument("--no-ks-tracks", action="store_true", help="检查 MIDI 里不单独放 keyswitch 轨")
    p.add_argument("-q", "--quiet", action="store_true")

    common(sub.add_parser("report", help="打印规则结论（不写文件）"))

    p = common(sub.add_parser("decide", help="确认/否决一条规则结论"))
    p.add_argument("decision")
    p.add_argument("verdict", choices=["confirm", "reject", "reset"])

    p = common(sub.add_parser("note", help="逐音指定是否 keyswitch"))
    p.add_argument("sourceNote", type=int, nargs="+")
    p.add_argument("--as", dest="what", choices=["ks", "not-ks", "clear"], required=True)

    p = common(sub.add_parser("instrument", help="给声部指定乐器"))
    p.add_argument("lane")
    p.add_argument("instrument", help="乐器 id，或 none；可选：" + " ".join(BY_ID))

    p = common(sub.add_parser("merge", help="把几条声部合成一条"))
    p.add_argument("lanes", nargs="+")
    p.add_argument("--name")

    args = ap.parse_args(argv)
    midi, sc_path = _paths(args.midi, args.sidecar)
    smf = read_smf(midi)

    if args.cmd == "run":
        sc = _load_or_analyze(smf, sc_path, reset=args.reset)
        _write_outputs(smf, sc, midi, sc_path, args.out_dir, not args.no_ks_tracks, args.quiet)
        return 0
    if args.cmd == "report":
        _print_report(smf, _load_or_analyze(smf, sc_path))
        return 0

    sc = _load_or_analyze(smf, sc_path)
    try:
        if args.cmd == "decide":
            if args.verdict == "reset":
                d = sc.decision(args.decision)
                if d is None:
                    raise KeyError(args.decision)
                sc.decisions.remove(d)
                sc = A.analyze(smf, sc)
            else:
                sc = A.apply_user_decision(smf, sc, args.decision, VERDICTS[args.verdict])
        elif args.cmd == "note":
            for sn in args.sourceNote:
                if args.what == "clear":
                    sc.noteOverrides.pop(sn, None)
                else:
                    sc.noteOverrides[sn] = {"keyswitch": args.what == "ks"}
            sc = A.analyze(smf, sc)
        elif args.cmd == "instrument":
            sc = A.set_lane_instrument(smf, sc, args.lane, None if args.instrument == "none" else args.instrument)
        elif args.cmd == "merge":
            sc = A.merge_lanes(smf, sc, args.lanes, args.name)
    except KeyError as e:
        print(f"找不到：{e}", file=sys.stderr)
        return 2
    S.save(sc, sc_path)
    _print_report(smf, sc)
    return 0
