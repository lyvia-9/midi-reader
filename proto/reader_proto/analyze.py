"""原始 MIDI → sidecar：建初始声部映射，跑三条规则。

已有 sidecar 时：声部映射原样保留（那是用户整理过的），只给新出现的 stream 补声部；
用户确认/否决过的结论原样保留，其余结论重新算。逐音修正（noteOverrides）永远保留。
"""
from __future__ import annotations

from pathlib import Path

from . import instruments as I
from .rules import channel_split, keyswitch, octave
from .sidecar import USER_STATUSES, Decision, LaneSpec, Sidecar
from .smf import SmfFile, Stream

MAX_ROUNDS = 3   # keyswitch ⇄ 八度 互相喂，转到不变为止


def _shade(hex_colour: str, i: int) -> str:
    """同组第 i 条：在组色基础上交替调亮/调暗（整数运算，C++ 逐位一致）。

    i=0 原色；奇数调亮（向白色混 12%·k），偶数调暗（向黑色混 12%·k），k = (i+1)//2，最多 3 档循环。
    """
    if i == 0:
        return hex_colour
    k = ((i + 1) // 2 - 1) % 3 + 1
    amount = 12 * k
    out = []
    for pos in (1, 3, 5):
        c = int(hex_colour[pos:pos + 2], 16)
        if i % 2:
            c = c + (255 - c) * amount // 100
        else:
            c = c * (100 - amount) // 100
        out.append(c)
    return "#{:02x}{:02x}{:02x}".format(*out)


def _lane_for(streams: list[Stream], split: bool) -> LaneSpec:
    st = streams[0]
    if split:
        inst, src = I.guess("", st.program, st.isDrum)
        if inst is None:
            inst, src = I.guess(st.trackName, st.program, st.isDrum)
    else:
        inst, src = I.guess(st.trackName, st.program, all(s.isDrum for s in streams))
    if split:
        base = st.trackName or f"轨 {st.chunk}"
        name = f"{base} · {inst.nameZh}" if inst else f"{base} · 通道{st.channel + 1}"
    else:
        name = st.trackName or (inst.nameZh if inst else f"轨 {st.chunk}")
    spec = LaneSpec(id="", name=name, sources=[s.id for s in streams])
    _set_instrument(spec, inst, src)
    return spec


def _set_instrument(spec: LaneSpec, inst: I.Instrument | None, src: str | None) -> None:
    spec.instrument = inst.id if inst else None
    spec.instrumentSource = src
    spec.comfortLow, spec.comfortHigh, spec.limitLow, spec.limitHigh = 0, 127, 0, 127
    if inst is not None:
        spec.group = I.group_name(inst.group)
        spec.colour = I.GROUP_COLOURS[inst.group]
        if inst.hasRange:
            spec.comfortLow, spec.comfortHigh = inst.comfort
            spec.limitLow, spec.limitHigh = inst.limit
    else:
        spec.group = ""
        spec.colour = I.GROUP_COLOURS["other"]


def build_lanes(smf: SmfFile, splits: dict[str, Decision], only: set[str] | None = None) -> list[LaneSpec]:
    """按块建声部；拆轨结论已应用的块一路一条，否则整块一条。按总谱顺序稳定排序。"""
    built: list[tuple[tuple, int, LaneSpec]] = []
    order = 0
    for chunk in smf.chunks:
        streams = [smf.streams[f"t{chunk.index}c{c}"] for c in chunk.channels]
        if only is not None:
            streams = [s for s in streams if s.id in only]
        if not streams:
            continue
        d = splits.get(f"split:t{chunk.index}")
        groups = [[s] for s in streams] if (d is not None and d.applied) else [streams]
        for g in groups:
            spec = _lane_for(g, split=len(groups) > 1)
            inst = I.BY_ID.get(spec.instrument) if spec.instrument else None
            built.append((I.score_order_key(inst), order, spec))
            order += 1
    built.sort(key=lambda x: (x[0], x[1]))
    return [b[2] for b in built]


def _assign_ids_and_colours(lanes: list[LaneSpec], existing: list[LaneSpec]) -> None:
    used = {l.id for l in existing}
    n = max([int(l.id[1:]) for l in existing if l.id[1:].isdigit()] + [0])
    per_group: dict[str, int] = {}
    for l in existing:
        per_group[l.group] = per_group.get(l.group, 0) + 1
    for l in lanes:
        n += 1
        while f"L{n}" in used:
            n += 1
        l.id = f"L{n}"
        used.add(l.id)
        i = per_group.get(l.group, 0)
        per_group[l.group] = i + 1
        l.colour = _shade(l.colour, i)


def _forced_keyswitch(overrides: dict[int, dict]) -> dict[int, bool]:
    return {sn: bool(v["keyswitch"]) for sn, v in overrides.items() if "keyswitch" in v}


def run_stream_rules(smf: SmfFile, stream: Stream, lane: LaneSpec | None,
                     kept: dict[str, Decision], forced: dict[int, bool]) -> list[Decision]:
    inst = I.BY_ID.get(lane.instrument) if lane and lane.instrument else None
    src = lane.instrumentSource if lane else None
    forced_here = {n.sourceNote: forced[n.sourceNote] for n in stream.notes if n.sourceNote in forced}
    oct_id = f"oct:{stream.id}"
    shift = kept[oct_id].params.get("shift", 0) if oct_id in kept and kept[oct_id].applied else 0
    ks_decs: list[Decision] = []
    oct_dec: Decision | None = None
    for _ in range(MAX_ROUNDS):
        computed = keyswitch.analyze(stream, smf.ppq, inst, src, shift, forced_here)
        by_side = {d.params["side"]: d for d in computed}
        ks_decs = []
        for side in ("low", "high"):
            kid = f"ks:{stream.id}:{side}"
            if kid in kept:
                ks_decs.append(kept[kid])
            elif side in by_side:
                ks_decs.append(by_side[side])
        excluded = {p for d in ks_decs if d.status != "rejected" for p in d.params["pitches"]}
        playing = [n for n in stream.notes
                   if not forced_here.get(n.sourceNote, False)
                   and (n.sourceNote in forced_here or n.pitch not in excluded)]
        oct_dec = kept.get(oct_id) or octave.analyze(stream, inst, src, playing)
        new_shift = oct_dec.params["shift"] if (oct_dec is not None and oct_dec.applied) else 0
        if new_shift == shift:
            break
        shift = new_shift
    return ks_decs + ([oct_dec] if oct_dec is not None else [])


def analyze(smf: SmfFile, existing: Sidecar | None = None, reset: bool = False) -> Sidecar:
    kept: dict[str, Decision] = {}
    overrides: dict[int, dict] = {}
    if existing is not None:
        kept = {d.id: d for d in existing.decisions if d.status in USER_STATUSES}
        overrides = dict(existing.noteOverrides)

    splits: dict[str, Decision] = {}
    for d in channel_split.analyze(smf):
        splits[d.id] = kept.get(d.id, d)

    if existing is not None and not reset:
        lanes = list(existing.lanes)
        referenced = {s for l in lanes for s in l.sources}
        missing = {sid for sid in smf.streams if sid not in referenced}
        new = build_lanes(smf, splits, only=missing) if missing else []
        _assign_ids_and_colours(new, lanes)
        lanes += new
    else:
        lanes = build_lanes(smf, splits)
        _assign_ids_and_colours(lanes, [])

    owner = {s: l for l in lanes for s in l.sources}
    forced = _forced_keyswitch(overrides)
    decisions: list[Decision] = []
    for chunk in smf.chunks:
        sd = splits.get(f"split:t{chunk.index}")
        if sd is not None:
            decisions.append(sd)
        for c in chunk.channels:
            st = smf.streams[f"t{chunk.index}c{c}"]
            decisions += run_stream_rules(smf, st, owner.get(st.id), kept, forced)
    produced = {d.id for d in decisions}
    decisions += [d for did, d in kept.items() if did not in produced]   # 目标已不在文件里的用户结论也别丢

    return Sidecar(
        source={"fileName": Path(smf.path).name, "sha256": smf.sha256, "format": smf.format,
                "ppq": smf.ppq, "noteCount": smf.noteCount},
        streams=[{"id": s.id, "chunk": s.chunk, "channel": s.channel, "trackName": s.trackName,
                  "program": s.program, "noteCount": len(s.notes)} for s in smf.streams.values()],
        lanes=lanes,
        decisions=decisions,
        noteOverrides=overrides,
    )


def apply_user_decision(smf: SmfFile, sc: Sidecar, did: str, verdict: str) -> Sidecar:
    """用户确认/否决一条结论，然后重跑规则（keyswitch 和八度会互相影响）。"""
    d = sc.decision(did)
    if d is None:
        raise KeyError(did)
    d.status = verdict
    if d.rule == "channelSplit" and verdict == "confirmed":
        _split_lane(smf, sc, d)
    return analyze(smf, sc)


def _split_lane(smf: SmfFile, sc: Sidecar, d: Decision) -> None:
    targets = set(d.params["streams"])
    for idx, lane in enumerate(sc.lanes):
        if set(lane.sources) == targets:
            new = build_lanes(smf, {d.id: d}, only=targets)
            _assign_ids_and_colours(new, [l for l in sc.lanes if l is not lane])
            sc.lanes[idx:idx + 1] = new
            return


def set_lane_instrument(smf: SmfFile, sc: Sidecar, lane_id: str, inst_id: str | None) -> Sidecar:
    for lane in sc.lanes:
        if lane.id == lane_id:
            inst = I.BY_ID[inst_id] if inst_id else None
            _set_instrument(lane, inst, "user" if inst else None)
            # 乐器变了，依赖它的自动结论作废重算
            sc.decisions = [x for x in sc.decisions if not (x.target in lane.sources and x.status not in USER_STATUSES)]
            return analyze(smf, sc)
    raise KeyError(lane_id)


def merge_lanes(smf: SmfFile, sc: Sidecar, lane_ids: list[str], name: str | None) -> Sidecar:
    """手动映射：把几条声部并成一条（保留第一条的乐器和位置）。"""
    lanes = [l for l in sc.lanes if l.id in lane_ids]
    if len(lanes) < 2:
        raise KeyError("need at least two existing lane ids")
    first = lanes[0]
    for l in lanes[1:]:
        first.sources += l.sources
    if name:
        first.name = name
    sc.lanes = [l for l in sc.lanes if l is first or l.id not in lane_ids]
    return analyze(smf, sc)
