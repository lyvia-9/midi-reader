"""投影层：原 MIDI + sidecar → 逻辑声部（LaneModel 能回答的全部内容）。

这是 C++ 投影层要逐条复现的逻辑，写得尽量直白。
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from .instruments import UNASSIGNED_GROUP
from .model import Lane, LaneInfo, LaneNote, LaneRole, NoteFlag, note_sort_key
from .sidecar import LaneSpec, Sidecar
from .smf import SmfFile, Stream

LANES_FORMAT = "midi-reader-lanes"
LANES_VERSION = 1
UNASSIGNED_COLOUR = "#9a9a9a"


@dataclass
class Projection:
    unitsPerQuarter: int
    documentLength: int
    meterChanges: list[tuple[int, int, int]]
    lanes: list[Lane]
    keySwitchNotes: dict[str, list[LaneNote]] = field(default_factory=dict)  # lane id -> 被判为 keyswitch 的音
    warnings: list[str] = field(default_factory=list)

    def to_json(self) -> dict:
        return {
            "format": LANES_FORMAT,
            "version": LANES_VERSION,
            "unitsPerQuarter": self.unitsPerQuarter,
            "documentLength": self.documentLength,
            # 字段名待 B 线开工时对照 pianoroll::MeterChange 定稿
            "meterChanges": [{"tick": t, "numerator": n, "denominator": d} for t, n, d in self.meterChanges],
            "lanes": [{"id": l.id, "info": l.info.to_json(), "longestNote": l.longestNote,
                       "notes": [n.to_json() for n in l.notes]} for l in self.lanes],
            "warnings": self.warnings,
        }


@dataclass
class _StreamRules:
    ks_applied: set[int] = field(default_factory=set)
    ks_review: set[int] = field(default_factory=set)
    shift: int = 0
    shift_review: bool = False
    split_review: bool = False


def _rules_by_stream(sc: Sidecar, smf: SmfFile) -> dict[str, _StreamRules]:
    out = {sid: _StreamRules() for sid in smf.streams}
    for d in sc.decisions:
        if d.rule == "channelSplit":
            if d.pending:
                for sid in d.params.get("streams", []):
                    if sid in out:
                        out[sid].split_review = True
            continue
        r = out.get(d.target)
        if r is None:
            continue
        if d.rule == "keySwitch":
            if d.applied:
                r.ks_applied.update(d.params["pitches"])
            elif d.pending:
                r.ks_review.update(d.params["pitches"])
        elif d.rule == "octaveShift":
            if d.applied:
                r.shift = int(d.params["shift"])
            elif d.pending:
                r.shift_review = True
    return out


def _project_lane(lane_id: str, spec_info: LaneInfo, streams: list[Stream], rules: dict[str, _StreamRules],
                  overrides: dict[int, dict]) -> tuple[Lane, list[LaneNote]]:
    notes: list[LaneNote] = []
    ks_notes: list[LaneNote] = []
    for st in streams:
        r = rules[st.id]
        for n in st.notes:
            forced = overrides.get(n.sourceNote, {}).get("keyswitch")
            if forced is True:
                is_ks, review_ks = True, False
            elif forced is False:
                is_ks, review_ks = False, False
            else:
                is_ks = n.pitch in r.ks_applied
                review_ks = not is_ks and n.pitch in r.ks_review
            flags = NoteFlag.NONE
            pitch = n.pitch
            if is_ks:
                flags |= NoteFlag.KEY_SWITCH
            else:
                if r.shift:
                    shifted = n.pitch + r.shift
                    if 0 <= shifted <= 127:
                        pitch = shifted
                    else:
                        flags |= NoteFlag.REVIEW
                if review_ks:
                    flags |= NoteFlag.REVIEW
                if r.shift_review and not spec_info.limitLow <= n.pitch <= spec_info.limitHigh:
                    flags |= NoteFlag.REVIEW
                if not spec_info.comfortLow <= pitch <= spec_info.comfortHigh:
                    flags |= NoteFlag.SOFT_RANGE
                if not spec_info.limitLow <= pitch <= spec_info.limitHigh:
                    flags |= NoteFlag.HARD_RANGE
            if r.split_review:
                flags |= NoteFlag.REVIEW
            ln = LaneNote(n.start, n.length, pitch, n.velocity, n.sourceNote, int(flags))
            notes.append(ln)
            if is_ks:
                ks_notes.append(ln)
    notes.sort(key=note_sort_key)
    ks_notes.sort(key=note_sort_key)
    return Lane(lane_id, spec_info, notes), ks_notes


def _info(spec: LaneSpec, streams: list[Stream]) -> LaneInfo:
    return LaneInfo(
        name=spec.name, group=spec.group, colour=spec.colour, role=LaneRole(spec.role),
        sourceTrackCount=max(1, len({s.chunk for s in streams})),
        comfortLow=spec.comfortLow, comfortHigh=spec.comfortHigh,
        limitLow=spec.limitLow, limitHigh=spec.limitHigh,
    )


def project(smf: SmfFile, sc: Sidecar) -> Projection:
    warnings: list[str] = []
    if sc.source.get("sha256") and sc.source["sha256"] != smf.sha256:
        warnings.append("sidecar 是对着另一个版本的 MIDI 写的（sha256 不同），按 stream id 尽量套用")
    rules = _rules_by_stream(sc, smf)
    lanes: list[Lane] = []
    ks: dict[str, list[LaneNote]] = {}
    referenced: set[str] = set()
    for spec in sc.lanes:
        streams = []
        for sid in spec.sources:
            if sid in smf.streams:
                streams.append(smf.streams[sid])
                referenced.add(sid)
            else:
                warnings.append(f"声部 {spec.id}（{spec.name}）引用的 {sid} 在文件里不存在")
        lane, ks_notes = _project_lane(spec.id, _info(spec, streams), streams, rules, sc.noteOverrides)
        lanes.append(lane)
        ks[spec.id] = ks_notes
    # 看不懂的不丢：没被任何声部引用的 stream 进“未归类”
    for sid, st in smf.streams.items():
        if sid in referenced:
            continue
        label = st.trackName or f"轨 {st.chunk}"
        info = LaneInfo(name=f"{label} · 通道{st.channel + 1}", group=UNASSIGNED_GROUP, colour=UNASSIGNED_COLOUR)
        lane, ks_notes = _project_lane(f"U:{sid}", info, [st], rules, sc.noteOverrides)
        lanes.append(lane)
        ks[lane.id] = ks_notes
    return Projection(smf.ppq, smf.length, list(smf.meters) or [(0, 4, 4)], lanes, ks, warnings)


def dumps(p: Projection) -> str:
    """紧凑但可 diff：每条声部的音符一行一个。"""
    d = p.to_json()
    lanes = d.pop("lanes")
    head = json.dumps(d, ensure_ascii=False, indent=2)[:-2]
    parts = []
    for l in lanes:
        notes = ",\n        ".join(json.dumps(n) for n in l["notes"])
        parts.append(
            "    {\n"
            f'      "id": {json.dumps(l["id"], ensure_ascii=False)},\n'
            f'      "info": {json.dumps(l["info"], ensure_ascii=False)},\n'
            f'      "longestNote": {l["longestNote"]},\n'
            f'      "notes": [\n        {notes}\n      ]\n'
            "    }"
        )
    return head + ',\n  "lanes": [\n' + ",\n".join(parts) + "\n  ]\n}\n"


def save(p: Projection, path: str | Path) -> None:
    Path(path).write_text(dumps(p), encoding="utf-8")
