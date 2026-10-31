"""sidecar 文件：映射 + 规则结论 + 逐音修正。原 MIDI 一个字节不改。

格式说明见 docs/sidecar-format.md。这里只做读写和校验。
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

FORMAT = "midi-reader-sidecar"
VERSION = 1
SUFFIX = ".reader.json"

STATUS_AUTO = "auto"            # 规则有把握，已自动应用
STATUS_REVIEW = "review"        # 规则没把握，未应用，相关音符标黄
STATUS_CONFIRMED = "confirmed"  # 用户确认，应用
STATUS_REJECTED = "rejected"    # 用户否决，不应用、不标黄
STATUSES = (STATUS_AUTO, STATUS_REVIEW, STATUS_CONFIRMED, STATUS_REJECTED)
USER_STATUSES = (STATUS_CONFIRMED, STATUS_REJECTED)


class SidecarError(ValueError):
    pass


@dataclass
class Decision:
    id: str                 # 例 "split:t1" / "ks:t2c0:low" / "oct:t3c0"
    rule: str               # "channelSplit" | "keySwitch" | "octaveShift"
    target: str             # 块 "t1" 或 stream "t2c0"
    status: str
    params: dict[str, Any] = field(default_factory=dict)
    evidence: dict[str, Any] = field(default_factory=dict)
    reason: str = ""

    @property
    def applied(self) -> bool:
        return self.status in (STATUS_AUTO, STATUS_CONFIRMED)

    @property
    def pending(self) -> bool:
        return self.status == STATUS_REVIEW

    def to_json(self) -> dict:
        return {
            "id": self.id, "rule": self.rule, "target": self.target, "status": self.status,
            "params": self.params, "evidence": self.evidence, "reason": self.reason,
        }

    @staticmethod
    def from_json(d: dict) -> "Decision":
        if d.get("status") not in STATUSES:
            raise SidecarError(f"decision {d.get('id')}: bad status {d.get('status')!r}")
        return Decision(d["id"], d["rule"], d["target"], d["status"],
                        dict(d.get("params", {})), dict(d.get("evidence", {})), d.get("reason", ""))


@dataclass
class LaneSpec:
    id: str
    name: str
    sources: list[str]                    # stream id，顺序无关；一个 stream 只能属于一条 lane
    group: str = ""
    colour: str = "#808080"
    role: str = "unknown"
    instrument: str | None = None         # instruments.py 里的 id
    instrumentSource: str | None = None   # "user" | "name" | "program" | "drumChannel" | None
    comfortLow: int = 0
    comfortHigh: int = 127
    limitLow: int = 0
    limitHigh: int = 127

    def to_json(self) -> dict:
        return {
            "id": self.id, "name": self.name, "group": self.group, "colour": self.colour,
            "role": self.role, "instrument": self.instrument, "instrumentSource": self.instrumentSource,
            "range": {"comfortLow": self.comfortLow, "comfortHigh": self.comfortHigh,
                      "limitLow": self.limitLow, "limitHigh": self.limitHigh},
            "sources": list(self.sources),
        }

    @staticmethod
    def from_json(d: dict) -> "LaneSpec":
        r = d.get("range", {})
        return LaneSpec(
            id=d["id"], name=d["name"], sources=list(d["sources"]), group=d.get("group", ""),
            colour=d.get("colour", "#808080"), role=d.get("role", "unknown"),
            instrument=d.get("instrument"), instrumentSource=d.get("instrumentSource"),
            comfortLow=r.get("comfortLow", 0), comfortHigh=r.get("comfortHigh", 127),
            limitLow=r.get("limitLow", 0), limitHigh=r.get("limitHigh", 127),
        )


@dataclass
class Sidecar:
    source: dict[str, Any]
    streams: list[dict[str, Any]]
    lanes: list[LaneSpec]
    decisions: list[Decision]
    noteOverrides: dict[int, dict[str, Any]] = field(default_factory=dict)

    def decision(self, did: str) -> Decision | None:
        for d in self.decisions:
            if d.id == did:
                return d
        return None

    def to_json(self) -> dict:
        return {
            "format": FORMAT,
            "version": VERSION,
            "source": self.source,
            "streams": self.streams,
            "lanes": [l.to_json() for l in self.lanes],
            "decisions": [d.to_json() for d in self.decisions],
            "noteOverrides": {str(k): v for k, v in sorted(self.noteOverrides.items())},
        }

    @staticmethod
    def from_json(d: dict) -> "Sidecar":
        if d.get("format") != FORMAT:
            raise SidecarError("not a midi-reader sidecar")
        if d.get("version") != VERSION:
            raise SidecarError(f"unsupported sidecar version {d.get('version')}")
        sc = Sidecar(
            source=dict(d.get("source", {})),
            streams=list(d.get("streams", [])),
            lanes=[LaneSpec.from_json(x) for x in d.get("lanes", [])],
            decisions=[Decision.from_json(x) for x in d.get("decisions", [])],
            noteOverrides={int(k): dict(v) for k, v in d.get("noteOverrides", {}).items()},
        )
        sc.validate()
        return sc

    def validate(self) -> None:
        seen_lane: set[str] = set()
        owner: dict[str, str] = {}
        for l in self.lanes:
            if l.id in seen_lane:
                raise SidecarError(f"duplicate lane id {l.id}")
            seen_lane.add(l.id)
            for s in l.sources:
                if s in owner:
                    raise SidecarError(f"stream {s} is in both {owner[s]} and {l.id}")
                owner[s] = l.id
        ids = [d.id for d in self.decisions]
        if len(ids) != len(set(ids)):
            raise SidecarError("duplicate decision id")


def sidecar_path_for(midi_path: str | Path) -> Path:
    p = Path(midi_path)
    return p.with_name(p.name + SUFFIX)


def load(path: str | Path) -> Sidecar:
    return Sidecar.from_json(json.loads(Path(path).read_text(encoding="utf-8")))


def dumps(sc: Sidecar) -> str:
    return json.dumps(sc.to_json(), ensure_ascii=False, indent=2) + "\n"


def save(sc: Sidecar, path: str | Path) -> None:
    sc.validate()
    Path(path).write_text(dumps(sc), encoding="utf-8")
