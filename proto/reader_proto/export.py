"""导出“一个逻辑声部一条轨”的 MIDI，拖进 FL Studio 检查整理结果。

- 每条声部一条轨，轨名 = 声部名，音高是八度修正后的
- 被判为 keyswitch 的音不放进声部轨；默认另起一条“<声部名> · KS”轨紧跟其后，
  方便在 FL 里看清到底去掉了什么（--no-ks-tracks 关掉）
- 速度、拍号照抄原文件
"""
from __future__ import annotations

from pathlib import Path

import symusic

from .instruments import BY_ID
from .model import NoteFlag
from .project import Projection
from .sidecar import Sidecar
from .smf import SmfFile


def _program_for(sc: Sidecar, smf: SmfFile, lane_id: str) -> tuple[int, bool]:
    spec = next((l for l in sc.lanes if l.id == lane_id), None)
    sources = spec.sources if spec else [lane_id[2:]]
    streams = [smf.streams[s] for s in sources if s in smf.streams]
    is_drum = bool(streams) and all(s.isDrum for s in streams)
    for s in streams:
        if s.program is not None:
            return s.program, is_drum
    if spec and spec.instrument and BY_ID[spec.instrument].programs:
        return BY_ID[spec.instrument].programs[0], is_drum
    return 0, is_drum


def build_score(proj: Projection, sc: Sidecar, smf: SmfFile, ks_tracks: bool = True) -> symusic.Score:
    score = symusic.Score(proj.unitsPerQuarter)
    for tick, mspq in smf.tempos or [(0, 500000)]:
        score.tempos.append(symusic.Tempo(tick, qpm=60_000_000 / mspq))
    for tick, num, den in proj.meterChanges:
        score.time_signatures.append(symusic.TimeSignature(tick, num, den))
    for lane in proj.lanes:
        program, is_drum = _program_for(sc, smf, lane.id)
        tr = symusic.Track(lane.info.name, program, is_drum)
        for n in lane.notes:
            if not n.flags & NoteFlag.KEY_SWITCH:
                tr.notes.append(symusic.Note(n.start, n.length, n.pitch, n.velocity))
        score.tracks.append(tr)
        ks = proj.keySwitchNotes.get(lane.id, [])
        if ks_tracks and ks:
            kt = symusic.Track(f"{lane.info.name} · KS", program, is_drum)
            for n in ks:
                kt.notes.append(symusic.Note(n.start, n.length, n.pitch, n.velocity))
            score.tracks.append(kt)
    return score


def export(proj: Projection, sc: Sidecar, smf: SmfFile, out: str | Path, ks_tracks: bool = True) -> None:
    build_score(proj, sc, smf, ks_tracks).dump_midi(str(out))
