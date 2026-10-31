"""规则一：按通道拆轨。

一个 MTrk 块里有两个以上通道带音符时：
- auto：type 0 文件；或这些通道的音色两两不同（含“一个是鼓通道”）——明显是不同乐器。
- review：有两个通道音色相同（或都没设音色）——可能是同一乐器故意分通道
  （例如采样器按通道分演奏法），先合成一条声部、标黄，等确认要不要拆。
"""
from __future__ import annotations

from ..sidecar import STATUS_AUTO, STATUS_REVIEW, Decision
from ..smf import SmfFile


def _identity(stream) -> str:
    if stream.isDrum:
        return "drum"
    return f"prog{stream.program}" if stream.program is not None else "prog?"


def analyze(smf: SmfFile) -> list[Decision]:
    out: list[Decision] = []
    for chunk in smf.chunks:
        if len(chunk.channels) < 2:
            continue
        streams = [smf.streams[f"t{chunk.index}c{c}"] for c in chunk.channels]
        idents = [_identity(s) for s in streams]
        distinct = len(set(idents)) == len(idents) and "prog?" not in idents
        type0 = smf.format == 0
        status = STATUS_AUTO if (type0 or distinct) else STATUS_REVIEW
        if type0:
            reason = "type 0 文件，所有乐器挤在一条轨里，按通道拆开"
        elif distinct:
            reason = "同一轨里各通道音色不同，按通道拆开"
        else:
            reason = "同一轨里有通道音色相同或未设音色，可能是同一乐器分通道，暂不拆"
        out.append(Decision(
            id=f"split:t{chunk.index}",
            rule="channelSplit",
            target=f"t{chunk.index}",
            status=status,
            params={"streams": [s.id for s in streams]},
            evidence={
                "fileFormat": smf.format,
                "channels": [s.channel + 1 for s in streams],     # 1 起，便于对照 DAW
                "programs": [s.program for s in streams],
                "noteCounts": [len(s.notes) for s in streams],
            },
            reason=reason,
        ))
    return out
