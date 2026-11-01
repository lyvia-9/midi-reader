"""最小的 SMF 写出器 + tempo 换算，管线拼 MIDI 用（不依赖 symusic/mido）。

写出 type 1：第 0 块只放 tempo 和拍号，之后一件乐器一块，块首写轨名和 program change。
"""
from __future__ import annotations

import bisect
from dataclasses import dataclass, field
from pathlib import Path

DRUM_CHANNEL = 9


@dataclass
class TrackOut:
    name: str
    notes: list[tuple[int, int, int, int]]          # (起 tick, 止 tick, 音高, 力度)
    drum: bool = False
    program: int | None = None
    channel: int | None = None                       # 不填就自动分配（跳过 9）
    meta: dict = field(default_factory=dict)


def _segments(tempos: list[tuple[int, int]], ppq: int) -> tuple[list[int], list[float], list[int]]:
    tempos = sorted(tempos or [(0, 500_000)])
    if tempos[0][0] != 0:
        tempos = [(0, 500_000)] + tempos
    ticks, secs, us = [], [], []
    t_prev, s_prev, u_prev = 0, 0.0, tempos[0][1]
    for tick, u in tempos:
        s_prev += (tick - t_prev) * u_prev / 1e6 / ppq
        t_prev, u_prev = tick, u
        ticks.append(tick)
        secs.append(s_prev)
        us.append(u)
    return ticks, secs, us


def ticks_to_seconds(tempos: list[tuple[int, int]], ppq: int, tick: int) -> float:
    ticks, secs, us = _segments(tempos, ppq)
    i = bisect.bisect_right(ticks, tick) - 1
    return secs[i] + (tick - ticks[i]) * us[i] / 1e6 / ppq


def seconds_to_ticks(tempos: list[tuple[int, int]], ppq: int, sec: float) -> int:
    ticks, secs, us = _segments(tempos, ppq)
    i = max(0, bisect.bisect_right(secs, sec) - 1)
    return max(0, round(ticks[i] + (sec - secs[i]) * 1e6 * ppq / us[i]))


def _vlq(n: int) -> bytes:
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def _chunk(events: list[tuple[int, int, bytes]]) -> bytes:
    """events: (tick, 排序键, 字节)。同 tick 先 note-off（键 0）再其他（键 1）。"""
    events.sort(key=lambda e: (e[0], e[1]))
    body = bytearray()
    last = 0
    for tick, _, data in events:
        body += _vlq(tick - last) + data
        last = tick
    body += _vlq(0) + b"\xff\x2f\x00"
    return b"MTrk" + len(body).to_bytes(4, "big") + bytes(body)


def _text(kind: int, s: str) -> bytes:
    b = s.encode("utf-8")
    return bytes([0xFF, kind]) + _vlq(len(b)) + b


def write_smf(path: str | Path, tracks: list[TrackOut], ppq: int = 480,
              tempos: list[tuple[int, int]] | None = None,
              meters: list[tuple[int, int, int]] | None = None) -> None:
    head: list[tuple[int, int, bytes]] = []
    for tick, u in tempos or [(0, 500_000)]:
        head.append((tick, 1, b"\xff\x51\x03" + u.to_bytes(3, "big")))
    for tick, num, den in meters or []:
        head.append((tick, 1, bytes([0xFF, 0x58, 4, num, max(0, den.bit_length() - 1), 24, 8])))
    chunks = [_chunk(head)]

    free = [c for c in range(16) if c != DRUM_CHANNEL]
    nxt = 0
    for t in tracks:
        if t.channel is not None:
            ch = t.channel
        elif t.drum:
            ch = DRUM_CHANNEL
        else:
            ch = free[nxt % len(free)]
            nxt += 1
        ev: list[tuple[int, int, bytes]] = [(0, 1, _text(0x03, t.name))]
        if t.program is not None and not t.drum:
            ev.append((0, 1, bytes([0xC0 | ch, t.program & 0x7F])))
        for s, e, p, v in t.notes:
            ev.append((s, 2, bytes([0x90 | ch, p & 0x7F, max(1, min(127, v))])))
            ev.append((e, 0, bytes([0x80 | ch, p & 0x7F, 0])))
        chunks.append(_chunk(ev))

    header = b"MThd" + (6).to_bytes(4, "big") + (1).to_bytes(2, "big") + len(chunks).to_bytes(2, "big") + ppq.to_bytes(2, "big")
    Path(path).write_bytes(header + b"".join(chunks))
