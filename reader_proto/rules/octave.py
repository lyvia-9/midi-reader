"""规则三：找音源八度偏移。

不同音源对“中央 C 在哪个八度”的约定不一样，或者某个音源本身就按记谱音高
（短笛、低音提琴、吉他）而不是实际音高接收 MIDI——导出来的音整体差一两个八度。

做法：需要乐器已知。把这一路正常音（去掉 keyswitch）整体平移 -24/-12/0/+12/+24，
数一数各自落到演奏极限外的音有多少，取最少的那个；并列时取平移量绝对值小的（能解决问题的
最小修正），再并列看超舒适音域的数量。
- review：原位超极限 ≥ 10%（且至少 2 个），平移后超极限 ≤ 5%，超舒适音域的也变少
- auto：在此之上，乐器来自用户指定或轨名，原位超极限 ≥ 20%，平移后一个超极限的都没有，
  超舒适音域的至少减半
只有少数几个音出界说明不了整条音源错了八度（可能就是写过了头），所以 auto 的门槛定得高。
"""
from __future__ import annotations

from ..instruments import TRUSTED_INSTRUMENT_SOURCES, Instrument
from ..sidecar import STATUS_AUTO, STATUS_REVIEW, Decision
from ..smf import RawNote, Stream

SHIFTS = (-24, -12, 0, 12, 24)
MIN_NOTES = 8
BAD_AT_ZERO = (1, 10)     # review：原位超极限 ≥ 10%
MIN_BAD_AT_ZERO = 2
GOOD_AFTER = (1, 20)      # review：平移后超极限 ≤ 5%
AUTO_BAD_AT_ZERO = (1, 5) # auto：原位超极限 ≥ 20%


def _outside(pitches: list[int], lo: int, hi: int, k: int) -> int:
    return sum(1 for p in pitches if not lo <= p + k <= hi)


def analyze(stream: Stream, inst: Instrument | None, inst_source: str | None,
            playing: list[RawNote]) -> Decision | None:
    if inst is None or not inst.hasRange or stream.isDrum:
        return None
    if len(playing) < MIN_NOTES:
        return None
    pitches = [n.pitch for n in playing]
    n = len(pitches)
    table = []
    for k in SHIFTS:
        hard = _outside(pitches, inst.limit[0], inst.limit[1], k)
        soft = _outside(pitches, inst.comfort[0], inst.comfort[1], k)
        table.append((hard, abs(k), soft, k))
    best = min(table)
    k = best[3]
    hard0 = next(t[0] for t in table if t[3] == 0)
    if k == 0:
        return None
    if hard0 < MIN_BAD_AT_ZERO or hard0 * BAD_AT_ZERO[1] < n * BAD_AT_ZERO[0]:
        return None
    if best[0] * GOOD_AFTER[1] > n * GOOD_AFTER[0]:
        return None
    soft0 = next(t[2] for t in table if t[3] == 0)
    if best[2] >= soft0:
        return None
    trusted = inst_source in TRUSTED_INSTRUMENT_SOURCES
    strong = hard0 * AUTO_BAD_AT_ZERO[1] >= n * AUTO_BAD_AT_ZERO[0]
    status = STATUS_AUTO if (trusted and best[0] == 0 and strong and best[2] * 2 <= soft0) else STATUS_REVIEW
    octs = k // 12
    reason = (f"按{inst.nameZh}的音域，原位有 {hard0}/{n} 个音超出演奏极限，"
              f"整体{'升' if k > 0 else '降'} {abs(octs)} 个八度后剩 {best[0]} 个")
    if status == STATUS_REVIEW:
        missing = []
        if not trusted:
            missing.append("乐器只从音色号猜出")
        if best[0]:
            missing.append("平移后仍有超极限的音")
        if not strong:
            missing.append("出界的音不到两成")
        if best[2] * 2 > soft0:
            missing.append("平移后超舒适音域的音没减少一半")
        reason += "；没把握：" + "、".join(missing)
    return Decision(
        id=f"oct:{stream.id}",
        rule="octaveShift",
        target=stream.id,
        status=status,
        params={"shift": k},
        evidence={
            "instrument": inst.id, "instrumentSource": inst_source, "notes": n,
            "outsideLimitByShift": {str(t[3]): t[0] for t in sorted(table, key=lambda t: t[3])},
            "outsideComfortByShift": {str(t[3]): t[2] for t in sorted(table, key=lambda t: t[3])},
        },
        reason=reason,
    )
