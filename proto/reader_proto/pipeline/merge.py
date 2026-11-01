"""把几个分轨扒出来的 MIDI 拼成一个（一个声部一块），时间按原音频对齐。

各家写 MIDI 的时间基准不同：
- Basic Pitch：固定 tempo，tick 0 = 音频 0 秒
- MuScriptor：按检测到的节拍写 tempo map，为了让小节线落在强拍上会把开头往后挪，
  挪了多少秒记在 marker “muscriptor:bar_offset=<秒>”里（音频 0 秒在 MIDI 里的播放时刻）
所以先全部换回“音频秒”，再按主文件的 tempo map 写回 tick。主文件优先用带节拍信息的
（MuScriptor 的），这样小节线是对的；都没有就用 120 BPM。
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from ..smf import read_smf
from .midiwrite import TrackOut, seconds_to_ticks, ticks_to_seconds, write_smf

OFFSET_MARKER = re.compile(rb"muscriptor:bar_offset=(-?[0-9]+(?:\.[0-9]+)?)")


@dataclass
class Part:
    path: Path
    name: str                 # 写进 MIDI 的轨名（= 预设里的声部名）
    drum: bool = False
    program: int | None = None


def audio_offset(path: Path) -> float:
    m = OFFSET_MARKER.search(Path(path).read_bytes())
    return float(m.group(1)) if m else 0.0


def has_beat_grid(path: Path) -> bool:
    return OFFSET_MARKER.search(Path(path).read_bytes()) is not None


def merge(parts: list[Part], out: Path) -> Path:
    masters = [p for p in parts if has_beat_grid(p.path)]
    if masters:
        m = read_smf(masters[0].path)
        ppq, tempos, meters, m_off = m.ppq, m.tempos or [(0, 500_000)], m.meters, audio_offset(masters[0].path)
    else:
        ppq, tempos, meters, m_off = 480, [(0, 500_000)], [], 0.0

    tracks = []
    for part in parts:
        smf = read_smf(part.path)
        off = audio_offset(part.path)
        src_tempos = smf.tempos or [(0, 500_000)]
        notes = []
        for n in smf.all_notes():
            s_sec = ticks_to_seconds(src_tempos, smf.ppq, n.start) - off + m_off
            e_sec = ticks_to_seconds(src_tempos, smf.ppq, n.end) - off + m_off
            s = seconds_to_ticks(tempos, ppq, s_sec)
            e = max(s + 1, seconds_to_ticks(tempos, ppq, e_sec))
            notes.append((s, e, n.pitch, n.velocity))
        tracks.append(TrackOut(part.name, notes, drum=part.drum, program=part.program))
    write_smf(out, tracks, ppq, tempos, meters)
    return out
