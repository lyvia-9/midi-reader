"""编制预设：一份预设说清楚“这首曲子有哪些声部”，管线和读谱两头都用它。

一个槽位（slot）同时决定：
- 全曲扒谱时告诉 MuScriptor 曲中有哪些乐器（muscriptor）
- 分轨路线里要分出哪一轨（stem）、用谁扒（transcriber）
- 扒出来的轨落到乐器库里的哪件乐器（instrument）、叫什么（lane）、什么职能（role）
格式说明见 docs/audio-pipeline.md；预设文件在仓库根的 presets/。
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

from .. import analyze as A
from .. import instruments as I
from ..model import LaneRole
from ..sidecar import USER_STATUSES, Sidecar
from ..smf import SmfFile
from .providers.muscriptor import INSTRUMENTS as MUSCRIPTOR_INSTRUMENTS
from .providers.muscriptor import track_name

FORMAT = "midi-reader-preset"
VERSION = 1
ROUTES = ("full", "stems")
PRESETS_DIR = Path(__file__).resolve().parents[3] / "presets"


class PresetError(ValueError):
    pass


@dataclass
class Slot:
    lane: str
    instrument: str | None = None
    group: str | None = None              # 只在 instrument 为空时用：乐器库的组 key
    role: str = "unknown"
    muscriptor: list[str] = field(default_factory=list)
    stem: str | None = None
    transcriber: str | None = None        # 分轨路线里扒这一轨的 provider，默认 basic-pitch
    match: list[str] = field(default_factory=list)   # 轨名正则（小写），认别家扒出来的轨用

    def track_names(self) -> set[str]:
        """这个槽位“认领”的轨名（规整后）：声部名本身 + MuScriptor 的轨名。"""
        return {_norm(self.lane)} | {_norm(track_name(m)) for m in self.muscriptor}


@dataclass
class Preset:
    id: str
    name: str
    slots: list[Slot]
    route: str = "full"
    separator: str = "demucs"
    transcriber: str = "muscriptor"       # 全曲路线的 provider
    description: str = ""

    @property
    def muscriptor_instruments(self) -> list[str]:
        seen: list[str] = []
        for s in self.slots:
            for m in s.muscriptor:
                if m not in seen:
                    seen.append(m)
        return seen

    @property
    def stems(self) -> list[str]:
        seen: list[str] = []
        for s in self.slots:
            if s.stem and s.stem not in seen:
                seen.append(s.stem)
        return seen

    @staticmethod
    def from_json(d: dict) -> "Preset":
        if d.get("format") != FORMAT:
            raise PresetError("not a midi-reader preset")
        if d.get("version") != VERSION:
            raise PresetError(f"unsupported preset version {d.get('version')}")
        slots = [Slot(lane=s["lane"], instrument=s.get("instrument"), group=s.get("group"),
                      role=s.get("role", "unknown"), muscriptor=list(s.get("muscriptor", [])),
                      stem=s.get("stem"), transcriber=s.get("transcriber"), match=list(s.get("match", [])))
                 for s in d.get("slots", [])]
        p = Preset(id=d["id"], name=d.get("name", d["id"]), slots=slots, route=d.get("route", "full"),
                   separator=d.get("separator", "demucs"), transcriber=d.get("transcriber", "muscriptor"),
                   description=d.get("description", ""))
        p.validate()
        return p

    def validate(self) -> None:
        if self.route not in ROUTES:
            raise PresetError(f"{self.id}: route 只能是 {' / '.join(ROUTES)}")
        if not self.slots:
            raise PresetError(f"{self.id}: 没有槽位")
        roles = {r.value for r in LaneRole}
        lanes = [s.lane for s in self.slots]
        if len(lanes) != len(set(lanes)):
            raise PresetError(f"{self.id}: 声部名重复")
        for s in self.slots:
            where = f"{self.id}/{s.lane}"
            if s.instrument is not None and s.instrument not in I.BY_ID:
                raise PresetError(f"{where}: 乐器库里没有 {s.instrument!r}")
            if s.group is not None and s.group not in I.GROUPS:
                raise PresetError(f"{where}: 没有这个组 {s.group!r}")
            if s.role not in roles:
                raise PresetError(f"{where}: role 只能是 {', '.join(sorted(roles))}")
            bad = [m for m in s.muscriptor if m not in MUSCRIPTOR_INSTRUMENTS]
            if bad:
                raise PresetError(f"{where}: MuScriptor 不认识 {', '.join(bad)}")
            for pat in s.match:
                re.compile(pat)
            if self.route == "stems" and not s.stem:
                raise PresetError(f"{where}: 分轨路线的每个槽位都要写 stem")


def _norm(name: str) -> str:
    return " ".join(name.lower().replace("_", " ").split())


def load(path: str | Path) -> Preset:
    return Preset.from_json(json.loads(Path(path).read_text(encoding="utf-8")))


def builtin(directory: Path = PRESETS_DIR) -> dict[str, Preset]:
    out = {}
    for f in sorted(directory.glob("*.json")):
        p = load(f)
        out[p.id] = p
    return out


def resolve(name_or_path: str) -> Preset:
    p = Path(name_or_path)
    if p.suffix == ".json" and p.exists():
        return load(p)
    presets = builtin()
    if name_or_path not in presets:
        raise PresetError(f"没有这个预设：{name_or_path}（可选：{', '.join(presets)}）")
    return presets[name_or_path]


# ---- 套到 sidecar 上 ----

def slot_for(preset: Preset, track_names: list[str]) -> int | None:
    names = [_norm(t) for t in track_names if t]
    for i, s in enumerate(preset.slots):        # 先认确切的轨名
        if any(n in s.track_names() for n in names):
            return i
    for i, s in enumerate(preset.slots):        # 再认正则
        if any(re.search(pat, n) for pat in s.match for n in names):
            return i
    return None


def apply(smf: SmfFile, sc: Sidecar, preset: Preset) -> Sidecar:
    """按预设给声部改名、定乐器和职能、排顺序；同一槽位的几条声部并成一条。

    预设是用户选的，所以乐器来源记 "user"（规则会据此自动应用）。没认领的声部原样留在后面。
    """
    claimed: dict[int, list] = {}
    rest = []
    for lane in sc.lanes:
        names = [smf.streams[s].trackName for s in lane.sources if s in smf.streams]
        i = slot_for(preset, names)
        if i is None:
            rest.append(lane)
        else:
            claimed.setdefault(i, []).append(lane)

    touched: set[str] = set()
    ordered = []
    for i, slot in enumerate(preset.slots):
        if i not in claimed:
            continue
        first, *others = claimed[i]
        for o in others:
            first.sources += o.sources
        first.name = slot.lane
        first.role = slot.role
        if slot.instrument:
            A._set_instrument(first, I.BY_ID[slot.instrument], "user")
        elif slot.group:
            first.group = I.group_name(slot.group)
            first.colour = I.GROUP_COLOURS[slot.group]
        touched.update(first.sources)
        ordered.append(first)
    sc.lanes = ordered + rest
    # 乐器变了，依赖它的自动结论作废重算
    sc.decisions = [d for d in sc.decisions if not (d.target in touched and d.status not in USER_STATUSES)]
    return A.analyze(smf, sc)
