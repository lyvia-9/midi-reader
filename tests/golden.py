"""“输入 MIDI → 期望结果”样例的运行器。

每个样例是 tests/cases/<name>/：
    input.mid               输入（不改）
    input.reader.json       可选：用户已有的 sidecar（手动映射、确认/否决、逐音修正）
    case.json               一句话说明这个样例考什么
    expected.reader.json    期望的 sidecar
    expected.lanes.json     期望的投影结果（LaneInfo + LaneNote，C++ 投影层要逐条对上）

    python tests/golden.py                 # 检查全部样例
    python tests/golden.py --update [名字]  # 在 FL 里核对无误后，把当前结果存为期望
"""
from __future__ import annotations

import argparse
import difflib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent))

from reader_proto import analyze as A  # noqa: E402
from reader_proto import project as P  # noqa: E402
from reader_proto import sidecar as S  # noqa: E402
from reader_proto.smf import read_smf  # noqa: E402

CASES = ROOT / "cases"


def case_dirs(names: list[str] | None = None) -> list[Path]:
    dirs = sorted(p for p in CASES.iterdir() if (p / "input.mid").exists())
    if names:
        dirs = [d for d in dirs if d.name in names]
    return dirs


def run_case(d: Path) -> tuple[str, str]:
    smf = read_smf(d / "input.mid")
    existing = S.load(d / "input.reader.json") if (d / "input.reader.json").exists() else None
    sc = A.analyze(smf, existing)
    return S.dumps(sc), P.dumps(P.project(smf, sc))


def check(d: Path) -> list[str]:
    got_sc, got_lanes = run_case(d)
    problems = []
    for fname, got in (("expected.reader.json", got_sc), ("expected.lanes.json", got_lanes)):
        f = d / fname
        if not f.exists():
            problems.append(f"{d.name}/{fname} missing (run with --update)")
            continue
        want = f.read_text(encoding="utf-8")
        if want != got:
            diff = "".join(list(difflib.unified_diff(want.splitlines(True), got.splitlines(True),
                                                     f"{fname} (expected)", f"{fname} (got)"))[:60])
            problems.append(f"{d.name}/{fname} differs:\n{diff}")
    return problems


def update(d: Path) -> None:
    got_sc, got_lanes = run_case(d)
    (d / "expected.reader.json").write_text(got_sc, encoding="utf-8")
    (d / "expected.lanes.json").write_text(got_lanes, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true")
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()
    dirs = case_dirs(args.names)
    if args.update:
        for d in dirs:
            update(d)
            print(f"updated {d.name}")
        return 0
    bad = 0
    for d in dirs:
        problems = check(d)
        print(("FAIL " if problems else "ok   ") + d.name)
        for p in problems:
            print(p)
        bad += bool(problems)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
